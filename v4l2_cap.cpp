#include "v4l2_cap.h"

static char v4l2_pix_type_str[5] = {0};

static const char *v4l2PixFmtStr(unsigned int ifmt) {
    v4l2_pix_type_str[0] = (char)(ifmt & 0xFF);
    v4l2_pix_type_str[1] = (char)((ifmt >> 8) & 0xFF);
    v4l2_pix_type_str[2] = (char)((ifmt >> 16) & 0xFF);
    v4l2_pix_type_str[3] = (char)((ifmt >> 24) & 0xFF);
    v4l2_pix_type_str[4] = '\0';
    return v4l2_pix_type_str;
}

Video::Video(unsigned int width, unsigned int height, unsigned int pixfmt, unsigned int fps, unsigned int perlinebytes,
             size_t imgsize)
    : width_(width), height_(height), fps_(fps), pixfmt_(pixfmt), perlinebytes_(perlinebytes), imgsize_(imgsize) {}

Video::~Video() {
    streamoff();
    close(vfd);
}

int Video::init(const char *dev) {
    vfd = open(dev, O_RDWR);
    if (vfd < 0) {
        fmt::print("Open device={} failed, reason={}\n", dev, strerror(errno));
        return -errno;
    }

    // capability
    {
        struct v4l2_capability capcity{};
        int ret = ioctl(vfd, VIDIOC_QUERYCAP, &capcity);
        if (ret < 0) {
            fmt::print("Open device={} failed, reason={}\n", dev, strerror(errno));
            return -errno;
        }
        fmt::print("\ndriver: {}, version: {}\ncard: {}\nbus_info: {}\ncapabilities: {:#x}\n",
                   (const char *)capcity.driver, capcity.version, (const char *)capcity.card,
                   (const char *)capcity.bus_info, capcity.capabilities);

        // device class
        if (capcity.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            isMplane = true;
            v4l2BufType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            fmt::print("multiple plane capture device\n");
        }
        if (capcity.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
            isMplane = false;
            v4l2BufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            fmt::print("single plane capture device\n");
        }
        if (capcity.capabilities & V4L2_CAP_STREAMING) {
            isStreaming = true;
            fmt::print("stream capture device\n");
        }
        std::cout << "\n";
    }

    // format query
    {
        struct v4l2_fmtdesc fmtdesc = {
            .index = 0,
            .type = v4l2BufType,
        };
        fmt::print("Support pixfmt: ");

        for (;;) {
            if (ioctl(vfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
                ++fmtdesc.index;
                fmt::print("{}, ", (const char *)fmtdesc.description);
            } else {
                fmt::print("\n\n");
                break;
            }
        }
    }

    // frame size query
    {
        unsigned int fmt_query_array[] = {V4L2_PIX_FMT_NV12,   V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_JPEG,
                                          V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY};
        struct v4l2_frmsizeenum framesize{};
        // FMT
        for (int i = 0; i < sizeof(fmt_query_array) / sizeof(fmt_query_array[0]); ++i) {
            framesize.index = 0;
            framesize.pixel_format = fmt_query_array[i];

            fmt::print("Pix_fmt={}, frame size/interval enum:\n", v4l2PixFmtStr(fmt_query_array[i]));
            for (;;) {
                // FRM SIZE
                if (ioctl(vfd, VIDIOC_ENUM_FRAMESIZES, &framesize) == 0) {
                    ++framesize.index;

                    struct v4l2_frmivalenum frmInterval = {
                        .index = 0,
                        .pixel_format = framesize.pixel_format,
                    };

                    if (framesize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                        fmt::print("\tdiscrete size: {}x{}\n", framesize.discrete.width, framesize.discrete.height);
                        frmInterval.width = framesize.discrete.width;
                        frmInterval.height = framesize.discrete.height;
                    } else if (framesize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                        fmt::print("\tstepwise size: Min={}x{} ~ Max={}x{}, step={}x{}\n", framesize.stepwise.min_width,
                                   framesize.stepwise.min_height, framesize.stepwise.max_width,
                                   framesize.stepwise.max_height, framesize.stepwise.step_width,
                                   framesize.stepwise.step_height);
                        frmInterval.width = framesize.stepwise.max_width;
                        frmInterval.height = framesize.stepwise.max_height;
                    } else if (framesize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
                        fmt::print("\tcontinus size: Min={}x{} ~ Max={}x{}\n", framesize.stepwise.min_width,
                                   framesize.stepwise.min_height, framesize.stepwise.max_width,
                                   framesize.stepwise.max_height);
                        frmInterval.width = framesize.stepwise.max_width;
                        frmInterval.height = framesize.stepwise.max_height;
                    }

                    for (;;) {
                        //  FRM RATE (FRM MAX SIZE)
                        if (ioctl(vfd, VIDIOC_ENUM_FRAMEINTERVALS, &frmInterval) == 0) {
                            ++frmInterval.index;

                            if (frmInterval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                                fmt::print("\t\tdiscrete interval: {}/{}\n", frmInterval.discrete.denominator,
                                           frmInterval.discrete.numerator);
                            } else if (frmInterval.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
                                fmt::print("\t\tstepwise interval: Min={}/{} ~ Max={}/{}, step={}/{}\n",
                                           frmInterval.stepwise.min.denominator, frmInterval.stepwise.min.numerator,
                                           frmInterval.stepwise.max.denominator, frmInterval.stepwise.max.numerator,
                                           frmInterval.stepwise.step.denominator, frmInterval.stepwise.step.numerator);
                            } else if (frmInterval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
                                fmt::print("\t\tcontinus interval: Min={}/{} ~ Max={}/{}\n",
                                           frmInterval.stepwise.min.denominator, frmInterval.stepwise.min.numerator,
                                           frmInterval.stepwise.max.denominator, frmInterval.stepwise.max.numerator);
                            }
                        } else {
                            break;
                        }
                    }
                    if (frmInterval.index == 0) {
                        fmt::print("  No frame intervals found or format not supported.\n");
                    }
                } else {
                    break;
                }
            }
        }
        std::cout << "\n";
    }

    // format
    {
        struct v4l2_format fmt = {
            .type = v4l2BufType,
        };
        if (ioctl(vfd, VIDIOC_G_FMT, &fmt) == 0) {
            if (isMplane) {
                fmt::print("Get multiple plane fmt: size={}x{}, pixfmt={}, field={}, colorspace={}, planes={}\n",
                           +fmt.fmt.pix_mp.width, +fmt.fmt.pix_mp.height, v4l2PixFmtStr(+fmt.fmt.pix_mp.pixelformat),
                           +fmt.fmt.pix_mp.field, +fmt.fmt.pix_mp.colorspace, +fmt.fmt.pix_mp.num_planes);
                for (int i = 0; i < fmt.fmt.pix_mp.num_planes; ++i) {
                    fmt::print("plane {}: bytesperline={}, sizeimage={}\n", i,
                               +fmt.fmt.pix_mp.plane_fmt[i].bytesperline, +fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
                }

            } else {
                fmt::print("Get single plane fmt: size={}x{}, pixfmt={}, field={}, colorspace={}, bytesperline={}, "
                           "sizeimage={} \n",
                           fmt.fmt.pix.width, fmt.fmt.pix.height, v4l2PixFmtStr(fmt.fmt.pix.pixelformat),
                           fmt.fmt.pix.field, fmt.fmt.pix.colorspace, fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
            }
            std::cout << "\n";
        } else {
            fmt::print("VIDIOC_G_FMT failed, reason={}\n", strerror(errno));
            return -errno;
        }
    }

    // stream param
    {
        struct v4l2_streamparm streamparm = {
            .type = v4l2BufType,
        };
        if (ioctl(vfd, VIDIOC_G_PARM, &streamparm) == 0) {

            fmt::print("Get stream param: capability={:#x}, captrue_mode={}, timeperframe={}/{}, readbuffers={}\n",
                       streamparm.parm.capture.capability, streamparm.parm.capture.capturemode,
                       streamparm.parm.capture.timeperframe.denominator, streamparm.parm.capture.timeperframe.numerator,
                       streamparm.parm.capture.readbuffers);

            if (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
                fmt::print("Support setting frame rate\n");
                isTimePerFrameSupported = true;
            }
        } else {
            fmt::print("VIDIOC_G_PARM failed, reason={}\n", strerror(errno));
            return -errno;
        }
        std::cout << "\n";
    }

    // buffer type
    {
        struct v4l2_requestbuffers req = {
            .count = 0, // 设为0时，驱动返回最大支持的缓冲区数
            .type = v4l2BufType,
            .memory = V4L2_MEMORY_DMABUF,
        };

        if (ioctl(vfd, VIDIOC_REQBUFS, &req) == 0) {
            fmt::print("Req buffer: capability={:#x}\n", req.capabilities);

            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP) {
                fmt::print("Supports MMAP\n");
            }
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_USERPTR) {
                fmt::print("Supports USERPTR\n");
            }
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF) {
                fmt::print("Supports DMABUF\n");
            }
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF) {
                fmt::print("Supports M2M_HOLD_CAPTURE_BUFFER\n");
            }
        } else {
            fmt::print("VIDIOC_REQBUFS failed, reason={}\n", strerror(errno));
            return -errno;
        }
        std::cout << "\n";
    }

    return 0;
}

int Video::streamon_mp_dmabuf(unsigned int n) {
    // 设置颜色格式
    struct v4l2_format fmt = {
        .type = v4l2BufType,
        .fmt =
            {
                .pix_mp =
                    {
                        .width = width_,
                        .height = height_,
                        .pixelformat = pixfmt_,
                        .field = V4L2_FIELD_NONE,
                        .colorspace = V4L2_COLORSPACE_SRGB,
                        .plane_fmt =
                            {
                                [0] =
                                    {
                                        .sizeimage = imgsize_,
                                        .bytesperline = perlinebytes_,
                                    },
                            },
                    },
            },
    };

    if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        fmt::print("VIDIOC_S_FMT failed, reason={}\n", strerror(errno));
        return -errno;
    }

    memset(&fmt, 0, sizeof(struct v4l2_format));
    fmt.type = v4l2BufType;

    if (ioctl(vfd, VIDIOC_G_FMT, &fmt) < 0) {
        fmt::print("VIDIOC_G_FMT failed, reason={}\n", strerror(errno));
        return -errno;
    }
    height_ = fmt.fmt.pix_mp.height;
    width_ = fmt.fmt.pix_mp.width;
    imgsize_ = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    pixfmt_ = fmt.fmt.pix_mp.pixelformat;
    num_planes = fmt.fmt.pix_mp.num_planes;

    fmt::print("Set fortmat: type={}, width={}, height={}, pixfmt={}, n_planes={}, frame_size={}\n", fmt.type, width_,
               height_, v4l2PixFmtStr(pixfmt_), num_planes, imgsize_);

    // v4l2申请DMA缓冲区
    struct v4l2_requestbuffers req = {
        .count = n,
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,
    };
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        fmt::print("VIDIOC_REQBUFS failed, reason={}\n", strerror(errno));
        return -errno;
    }
    if (req.count < n) {
        fmt::print("get buffers less than req\n");
        return -1;
    }
    fmt::print("Allocated {} buffers\n", req.count);

    // 设置帧率控制
    if (set_sensor_fps(fps_) < 0) {
        fmt::print("set_sensor_fps failed, reason={}\n", strerror(errno));
        return -errno;
    }
    if (get_v4l2_ctrl(V4L2_CID_VBLANK, &vblank_) < 0) {
        fmt::print("get_v4l2_ctrl failed, reason={}\n", strerror(errno));
        return -errno;
    }
    fmt::print("vblank={}\n", vblank_);

    // 启动采集
    if (ioctl(vfd, VIDIOC_STREAMON, &v4l2BufType) < 0) {
        fmt::print("VIDIOC_STREAMON failed, reason={}\n", strerror(errno));
        return -errno;
    }
    fmt::print("#### Stream on ####\n");
    isCapturing = true;

    return 0;
}

int Video::streamon_dmabuf(unsigned int n) {
    // UAC 720p fmt
    struct v4l2_format fmt = {
        .type = v4l2BufType,
        .fmt =
            {
                .pix =
                    {
                        .width = width_,
                        .height = height_,
                        .pixelformat = pixfmt_,
                        .bytesperline = perlinebytes_,
                        .sizeimage = imgsize_,
                        .colorspace = V4L2_COLORSPACE_SRGB,
                    },
            },
    };
    if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        fmt::print("VIDIOC_S_FMT failed, reason={}\n", strerror(errno));
        return -errno;
    }

    memset(&fmt, 0, sizeof(struct v4l2_format));
    fmt.type = v4l2BufType;
    if (ioctl(vfd, VIDIOC_G_FMT, &fmt) < 0) {
        fmt::print("VIDIOC_G_FMT failed, reason={}\n", strerror(errno));
        return -errno;
    }
    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    imgsize_ = fmt.fmt.pix.sizeimage;
    pixfmt_ = fmt.fmt.pix.pixelformat;
    num_planes = 0;
    fmt::print("Set fortmat: type={}, width={}, height={}, pixfmt={}, n_planes={} frame_size={}\n", fmt.type, width_,
               height_, v4l2PixFmtStr(pixfmt_), num_planes, imgsize_);

    // 设置帧率
    if (isTimePerFrameSupported) {
        struct v4l2_streamparm streamparm = {
            .type = v4l2BufType,
        };

        // 填充隐藏驱动参数
        if (ioctl(vfd, VIDIOC_G_PARM, &streamparm) < 0) {
            fmt::print("streamon_dmabuf VIDIOC_G_PARM failed, reason={}\n", strerror(errno));
            return -errno;
        }

        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = 30;

        // 写入配置
        if (ioctl(vfd, VIDIOC_S_PARM, &streamparm) != 0) {
            fmt::print("VIDIOC_S_PARM failed, reason={}\n", strerror(errno));
            return -errno;
        }

        // 检查当前参数
        memset(&streamparm, 0, sizeof(struct v4l2_streamparm));
        streamparm.type = v4l2BufType;
        ioctl(vfd, VIDIOC_G_PARM, &streamparm);

        fmt::print("Set stream param: capability={:#x}, captrue_mode={}, timeperframe={}/{}, readbuffers={}\n",
                   streamparm.parm.capture.capability, streamparm.parm.capture.capturemode,
                   streamparm.parm.capture.timeperframe.denominator, streamparm.parm.capture.timeperframe.numerator,
                   streamparm.parm.capture.readbuffers);
    }

    // 请求缓冲区
    struct v4l2_requestbuffers req = {
        .count = n,
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,

    };
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        fmt::print("VIDIOC_REQBUFS failed, reason={}\n", strerror(errno));
        return -errno;
    }
    if (req.count != n) {
        fmt::print("driver allocated fewer buffers than requested\n");
        return -1;
    }
    fmt::print("driver allocated {} buffers\n", req.count);

    // 启动采集
    if (ioctl(vfd, VIDIOC_STREAMON, &v4l2BufType) < 0) {
        fmt::print("VIDIOC_STREAMON failed, reason={}\n", strerror(errno));
        return -errno;
    }
    fmt::print("#### Stream on ####\n");
    isCapturing = true;

    return 0;
}

int Video::streamon(unsigned int n) {

    imgdbufs_.reserve(n);

    for (unsigned int i = 0; i < n; ++i) {
        imgdbufs_.emplace(i, std::make_shared<ImgDMABuf>());
    }

    if (isMplane) {
        return streamon_mp_dmabuf(n);
    } else {
        return streamon_dmabuf(n);
    }
}

void Video::streamoff() {
    if (isCapturing) {
        if (ioctl(vfd, VIDIOC_STREAMOFF, &v4l2BufType) < 0) {
            fmt::print("VIDIOC_STREAMON failed, reason={}\n", strerror(errno));
            return;
        }
        fmt::print("#### Stream off ####\n");
    }

    // 清除buffer
    struct v4l2_requestbuffers req;
    req.type = v4l2BufType;
    req.count = 0;
    req.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        fmt::print("VIDIOC_REQBUFS failed, reason={}\n", strerror(errno));
        return;
    }
}

std::shared_ptr<ImgDMABuf> Video::cap_frame_get() {
    struct v4l2_plane planes[1]{};
    struct v4l2_buffer buf = {
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,
    };

    if (v4l2BufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.length = 1;
        buf.m.planes = planes;
    }

    // 从采集队列中取出一个缓冲区
    if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
        fmt::print("cap_frame_get VIDIOC_DQBUF failed, reason={}\n", strerror(errno));
        return {};
    }

    // 返回所有权
    {
        std::lock_guard lock(mtx_);
        auto it = imgdbufs_.find(buf.index);
        if (it == imgdbufs_.end()) {
            fmt::print("v4l2 never put this buffer\n");
            return {};
        }
        return std::move(it->second);
    }
}

void Video::cap_frame_put(std::shared_ptr<ImgDMABuf> pb) {
    if (pb == nullptr) {
        return;
    }
    if (!pb->isValid()) {
        return;
    }

    struct v4l2_plane planes[1] = {
        [0] =
            {
                .length = static_cast<unsigned int>(pb->getSize()),
                .m =
                    {
                        .fd = pb->getFd(),
                    },
            },
    };

    struct v4l2_buffer buf = {
        .index = pb->img_get_index(),
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,
        .m =
            {
                .fd = pb->getFd(),
            },
    };

    if (v4l2BufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.length = 1;
        buf.m.planes = planes;
    }

    if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
        fmt::print("cap_frame_put VIDIOC_QBUF failed, reason={}\n", strerror(errno));
        return;
    }

    // 引用资源
    {
        std::lock_guard lock(mtx_);
        auto it = imgdbufs_.find(pb->img_get_index());
        if (it == imgdbufs_.end()) {
            fmt::print("Index error\n");
        } else {
            it->second = std::move(pb);
        }
    }
}

// int Video::captrue_mp_dma_test() {
//     int ret = 0;

//     if (!isCapturing) {
//         return -1;
//     }

//     struct v4l2_plane planes[1];
//     memset(&planes, 0, sizeof(struct v4l2_plane));

//     struct v4l2_buffer buf;
//     memset(&buf, 0, sizeof(struct v4l2_buffer));

//     buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
//     buf.memory = V4L2_MEMORY_DMABUF;
//     buf.m.planes = planes;
//     buf.length = 1;

//     // 从采集队列中取出一个缓冲区
//     if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
//         perror("VIDIOC_DQBUF");
//         return -1;
//     }

//     fflush(stdout);
//     fmt::print("DMABUF QBUF: index={}, seq={}, fd={}, bytesused={}\r", buf.index, buf.sequence, buf.m.planes[0].m.fd,
//                buf.m.planes[0].bytesused);

//     src = wrapbuffer_fd(buffers[buf.index].dmafd, width, height, RK_FORMAT_VYUY_422);
//     src_rect.x = 0;
//     src_rect.y = 0;
//     src_rect.width = width;
//     src_rect.height = height;

//     dst_rect.x = 0;
//     dst_rect.y = 0;
//     dst_rect.width = width;
//     dst_rect.height = height;

//     ret = imcheck(src, dst, src_rect, dst_rect);
//     if (IM_STATUS_NOERROR != ret) {
//         fprintf(stderr, "rga check error! %s", imStrError((IM_STATUS)ret));
//         return -1;
//     }

//     IM_STATUS status = imcvtcolor(src, dst, RK_FORMAT_VYUY_422, RK_FORMAT_RGB_888);
//     if (status != IM_STATUS_SUCCESS) {
//         perror("imcvtcolor");
//         return -1;
//     }

//     // 同步到CPU
//     dma_sync_device_to_cpu(buffers[buf.index].dmafd);

//     // 读取NV12 数据 buffers[buf.index].vaddr 到OpenCV
//     // cv::Mat raw_uvuy(height, width, CV_8UC2, buffers[buf.index].vaddr);
//     // cv::Mat bgr;
//     // cv::cvtColor(raw_uvuy, bgr, cv::COLOR_YUV2BGR_UYVY);
//     // if (bgr.empty()) {
//     //     fmt::print("Color convert fail\n");
//     //     ret = -1;
//     // }
//     cv::Mat rgb(height, width, CV_8UC3, rga_dst_buf);
//     cv::imshow("capture", rgb);

//     int key = cv::waitKey(40);
//     if (key == 27 || key == 'q') {
//         ret = -1;
//     }

//     // if (!cv::imwrite("/home/rock/c_cpp/stream_infer/asset/captrue_" + std::to_string(buf.sequence) + ".jpg",
//     // bgr)) {
//     //     fmt::print("Image write fail\n");
//     //     return -1;
//     // }

//     // 处理完成后，将缓冲区重新入队
//     if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
//         perror("VIDIOC_QBUF");
//         return -1;
//     }

//     return ret;
// }

// void Video::capture_test() {
//     if (!isCapturing) {
//         // LOG(WARNING) << "not capturing";
//         return;
//     }

//     struct v4l2_plane planes[1];
//     memset(&planes, 0, sizeof(struct v4l2_plane));

//     struct v4l2_buffer buf;
//     memset(&buf, 0, sizeof(struct v4l2_buffer));

//     buf.type = v4l2BufType;
//     buf.memory = V4L2_MEMORY_MMAP;
//     if (isMplane) {
//         buf.m.planes = planes;
//         buf.length = VIDEO_MAX_PLANES;
//     }
//     // 从采集队列中取出一个缓冲区
//     if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
//         LOG(ERROR) << "can not dequeue buffer";
//         return;
//     }
//     if (isMplane) {
//         buffers[buf.index].used_len = buf.m.planes[0].bytesused;
//         printf("index=%u plane0.bytesused=%u sequence=%u timestamp=%ld.%06ld\n", buf.index,
//         buf.m.planes[0].bytesused,
//                buf.sequence, buf.timestamp.tv_sec, buf.timestamp.tv_usec);
//     } else {
//         buffers[buf.index].used_len = buf.bytesused;
//         printf("index=%u bytesused=%u sequence=%u timestamp=%ld.%06ld\n", buf.index, buf.bytesused, buf.sequence,
//                buf.timestamp.tv_sec, buf.timestamp.tv_usec);
//     }
//     // 处理捕获到的数据，数据地址为 buffers[buf.index].start，长度为 buf.length
//     cv::Mat img;
//     if (fmt_ == NV12) {
//         cv::Mat timg = cv::Mat(height * 3 / 2, width, CV_8UC1, buffers[buf.index].vaddr);
//         cv::cvtColor(timg, img, cv::COLOR_YUV2BGR_NV12);
//     } else if (fmt_ == MJPG) {
//         std::vector<uchar> jpg_data((uchar *)buffers[buf.index].vaddr,
//                                     (uchar *)buffers[buf.index].vaddr + buffers[buf.index].used_len);
//         img = cv::imdecode(jpg_data, cv::IMREAD_COLOR);
//     }
//     cv::imwrite("/home/rock/c_cpp/stream_infer/asset/captrue_" + std::to_string(buf.sequence) + ".jpg", img);

//     // 处理完成后，将缓冲区重新入队
//     if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
//         LOG(ERROR) << "can not re-enqueue buffer";
//     }
// }