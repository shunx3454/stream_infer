#include "v4l2_cap.h"

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
        fmt::print("\ndriver: {}\n version: {}\n card: {}\n bus_info: {}\n capabilities: {:#x}\n",
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
        fmt::print("Support pixfmt: \n");

        for (;;) {
            if (ioctl(vfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
                fmt::print("{}, ", (const char *)fmtdesc.description);
                ++fmtdesc.index;
            } else {
                fmt::print("\n");
                break;
            }
        }
    }

    // frame size query
    {
        unsigned int fmt_query_array[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUV420,
                                          V4L2_PIX_FMT_YUYV};
        struct v4l2_frmsizeenum framesize{};
        // FMT
        for (int i = 0; i < sizeof(fmt_query_array) / sizeof(fmt_query_array[0]); ++i) {
            framesize.index = 0;
            framesize.pixel_format = fmt_query_array[i];

            for (;;) {
                if (ioctl(vfd, VIDIOC_ENUM_FRAMESIZES, &framesize) == 0) {
                    ++framesize.index;

                    // FRM SIZE
                    if (framesize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                        fmt::print("frame discrete size: {}x{}\n", framesize.discrete.width, framesize.discrete.height);
                    } else if (framesize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
                        fmt::print("frame stepwise size: Min={}x{} ~ Max={}x{}, step={}x{}\n",
                                   framesize.stepwise.min_width, framesize.stepwise.min_height,
                                   framesize.stepwise.max_width, framesize.stepwise.max_height,
                                   framesize.stepwise.step_width, framesize.stepwise.step_height);
                    } else {
                        fmt::print("frame continus size: Min={}x{} ~ Max={}x{}\n", framesize.stepwise.min_width,
                                   framesize.stepwise.min_height, framesize.stepwise.max_width,
                                   framesize.stepwise.max_height);
                    }

                    //  FRM RATE (FRM MAX SIZE)
                    struct v4l2_frmivalenum frmInterval = {
                        .index = 0,
                        .pixel_format = framesize.pixel_format,
                        .width = framesize.stepwise.max_width,
                        .height = framesize.stepwise.max_height,
                    };

                    for (;;) {
                        if (ioctl(vfd, VIDIOC_ENUM_FRAMEINTERVALS, &frmInterval) == 0) {
                            ++frmInterval.index;
                            if (frmInterval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                                fmt::print("frame discrete interval: {}/{}\n", frmInterval.discrete.numerator,
                                           frmInterval.discrete.denominator);
                            } else if (frmInterval.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
                                fmt::print("frame stepwise interval: Min={}/{} ~ Max={}/{}, step={}/{}\n",
                                           frmInterval.stepwise.min.numerator, frmInterval.stepwise.min.denominator,
                                           frmInterval.stepwise.max.numerator, frmInterval.stepwise.max.denominator,
                                           frmInterval.stepwise.step.numerator, frmInterval.stepwise.step.denominator);
                            } else {
                                fmt::print("frame continus interval: Min={}/{} ~ Max={}/{}\n",
                                           frmInterval.stepwise.min.numerator, frmInterval.stepwise.min.denominator,
                                           frmInterval.stepwise.max.numerator, frmInterval.stepwise.max.denominator);
                            }
                        } else {
                            break;
                        }
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
                           +fmt.fmt.pix_mp.width, +fmt.fmt.pix_mp.height, +fmt.fmt.pix_mp.pixelformat,
                           +fmt.fmt.pix_mp.field, +fmt.fmt.pix_mp.colorspace, +fmt.fmt.pix_mp.num_planes);
                for (int i = 0; i < fmt.fmt.pix_mp.num_planes; ++i) {
                    fmt::print("plane {}: bytesperline={}, sizeimage={}\n", i,
                               +fmt.fmt.pix_mp.plane_fmt[i].bytesperline, +fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
                }

            } else {
                fmt::print("Get single plane fmt: size={}x{}, pixfmt={}, field={}, colorspace={}, bytesperline={}, "
                           "sizeimage={} \n",
                           fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat, fmt.fmt.pix.field,
                           fmt.fmt.pix.colorspace, fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
            }
            std::cout << "\n";
        } else {
            fmt::print("VIDIOC_G_FMT failed, reason={}\n", strerror(errno));
            return -errno;
        }
    }

    // stream
    {
        struct v4l2_streamparm streamparm = {
            .type = v4l2BufType,
        };
        if (ioctl(vfd, VIDIOC_G_PARM, &streamparm) == 0) {
            fmt::print("Get stream param: capability={:#x}, captrue_mode={}, timeperframe={}/{}, readbuffers={}\n",
                       streamparm.parm.capture.capability, streamparm.parm.capture.capturemode,
                       streamparm.parm.capture.timeperframe.numerator, streamparm.parm.capture.timeperframe.denominator,
                       streamparm.parm.capture.readbuffers);
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
    }

    return 0;
}

int Video::streamon_mp_dmabuf(std::vector<DmaBuf> &dbufs) {
    int ret = 0;
    mem_ = DMABUF;
    fmt_ = NV12;

    // 设置颜色格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(struct v4l2_format));

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = 1920;
    fmt.fmt.pix_mp.height = 1088;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_UYVY;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 1920 * 2;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1920 * 1088 * 2;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    memset(&fmt, 0, sizeof(struct v4l2_format));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(vfd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("VIDIOC_G_FMT");
        return -1;
    }
    height = fmt.fmt.pix_mp.height;
    width = fmt.fmt.pix_mp.width;
    frame_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    int pixelformat = fmt.fmt.pix_mp.pixelformat;
    int num_planes = fmt.fmt.pix_mp.num_planes;
    fmt::print("Set fortmat: type={}, width={}, height={}, pixfmt={}, n_planes={} frame_size={}\n", fmt.type, width,
               height, pixelformat, num_planes, frame_size);

    // v4l2申请DMA缓冲区
    unsigned int req_num = static_cast<unsigned int>(dbufs.size());
    struct v4l2_requestbuffers req = {
        .count = req_num,
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,
    };
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }
    if (req.count < req_num) {
        fmt::print("get buffers less than req\n");
        return -1;
    }
    fmt::print("Allocated {} buffers\n", req.count);

    // 申请DMABUF
    buffers = std::vector<Buffer>(dbufs.size());
    DBufs = std::vector<DmaBuf>(dbufs.size());

    for (int i = 0; i < dbufs.size(); ++i) {
        // dmabuf 转移
        DBufs[i] = std::move(dbufs[i]);

        // 建立 fd  map index
        fdmapindex[DBufs[i].getFd()] = i;
        indexmapfd[i] = DBufs[i].getFd();

        // 记录
        this->buffers[i].length = DBufs[i].getSize();
        this->buffers[i].dmafd = DBufs[i].getFd();
        this->buffers[i].vaddr = DBufs[i].getVa();
        fmt::print("Allocated DMABUF: index={}, fd={}, vaddr={}, size={}\n", i, DBufs[i].getFd(), DBufs[i].getVa(),
                   DBufs[i].getSize());
    }

    // DMA fd 加入队列
    struct v4l2_buffer buf = {
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,
    };

    for (int i = 0; i < buffers.size(); ++i) {
        struct v4l2_plane planes[1]{};

        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;
        buf.m.planes[0].m.fd = DBufs[i].getFd();
        buf.m.planes[0].length = DBufs[i].getSize();

        int ret = ioctl(vfd, VIDIOC_QBUF, &buf);
        if (ret < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    // 申请 RGA DMABUF
    // int rga_dst_buf_size = height * width * 3;
    // ret = dma_buf_alloc("/dev/dma_heap/cma", rga_dst_buf_size, &rga_dst_dma_fd, (void **)&rga_dst_buf);
    // if (ret < 0) {
    //     printf("alloc src dma_heap buffer failed!\n");
    //     return -1;
    // }
    // dst = wrapbuffer_fd(rga_dst_dma_fd, width, height, RK_FORMAT_RGB_888);

    // 启动采集
    if (ioctl(vfd, VIDIOC_STREAMON, &v4l2BufType) < 0) {
        LOG(ERROR) << "can not start streaming";
        return -1;
    }
    LOG(INFO) << "streaming started";
    isCapturing = true;

    return 0;
}

void Video::streamoff() {
    if (isCapturing) {
        if (ioctl(vfd, VIDIOC_STREAMOFF, &v4l2BufType) < 0) {
            LOG(ERROR) << "can not stop streaming";
        }
        LOG(INFO) << "streaming stopped";
    }

    // 清除buffer
    struct v4l2_requestbuffers req;
    req.type = v4l2BufType;
    req.count = 0;
    req.memory = mem_ == DMABUF ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        LOG(ERROR) << "can not 0 request buffers";
    }

    // mnumap的缓冲区不需要手动释放，关闭设备时会自动释放
    if (mem_ == MMAP) {
        for (int i = 0; i < REQUEST_BUF_COUNT; i++) {
            if (buffers[i].vaddr != MAP_FAILED) {
                munmap(buffers[i].vaddr, buffers[i].length);
                LOG(INFO) << "MMAP Buffer " << i << " unmapped";
            }
        }
    }

    if (mem_ == DMABUF) {
        for (int i = 0; i < REQUEST_BUF_COUNT; ++i) {
            if (buffers[i].vaddr != MAP_FAILED) {
                dma_buf_free(buffers[i].length, buffers[i].dmafd, buffers[i].vaddr);
                fmt::print("DMABUF buffer: index={}, fd={}, vaddr={}, length={} unmapped\n", i, buffers[i].dmafd,
                           buffers[i].vaddr, buffers[i].length);
            }
        }
    }
}

int Video::streamon(enum mem_type mem_t) {
    // 设置缓冲区类型并开始采集
    // 1080p NV12
    mem_ = mem_t;
    struct v4l2_format fmt{};
    if (isMplane) {
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = 1920;
        fmt.fmt.pix_mp.height = 1088;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.num_planes = 1;
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 1920;
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1920 * 1088;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt_ = NV12;
    } else {
        // 720p MJPEG
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 1280;
        fmt.fmt.pix.height = 720;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt_ = MJPG;
    }
    if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG(ERROR) << "can not set format";
        return -1;
    } else {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = v4l2BufType;
        ioctl(vfd, VIDIOC_G_FMT, &fmt);
        if (isMplane) {
            LOG(INFO) << "Set fmt type: " << fmt.type;
            LOG(INFO) << "Set fmt width: " << fmt.fmt.pix_mp.width;
            LOG(INFO) << "Set fmt height: " << fmt.fmt.pix_mp.height;
            LOG(INFO) << "Set fmt pixfmt: " << fmt.fmt.pix_mp.pixelformat;
            LOG(INFO) << "Set fmt planes: " << (int)fmt.fmt.pix_mp.num_planes;
            LOG(INFO) << "Set plane 0 bytesperline: " << fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
            LOG(INFO) << "Set plane 0 sizeimage: " << fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
            this->mp_num = (int)fmt.fmt.pix_mp.num_planes;
            frame_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage; // 更新帧大小为驱动实际设置的值
            width = fmt.fmt.pix_mp.width;
            height = fmt.fmt.pix_mp.height;
        } else {
            LOG(INFO) << "Set fmt type: " << fmt.type;
            LOG(INFO) << "Set fmt width: " << fmt.fmt.pix.width;
            LOG(INFO) << "Set fmt height: " << fmt.fmt.pix.height;
            LOG(INFO) << "Set fmt pixfmt: " << fmt.fmt.pix.pixelformat;
            frame_size = fmt.fmt.pix.sizeimage; // 更新帧大小为驱动实际设置的值
            width = fmt.fmt.pix.width;
            height = fmt.fmt.pix.height;
        }
        std::cout << "\n";
    }

    // 设置帧率
    if (isMplane) {
        int target_fps = 15;
        int vblank = calc_vblank_from_fps(target_fps);
        if (vblank < MIN_VBLANK || vblank > MAX_VBLANK) {
            LOG(ERROR) << "target fps " << target_fps << " is out of range, vblank should be between " << MIN_VBLANK
                       << " and " << MAX_VBLANK;
            vblank = MIN_VBLANK;
        }

        if (set_v4l2_ctrl(SENSOR_DEV, V4L2_CID_VBLANK, vblank) < 0) {
            LOG(ERROR) << "can not set vblank";
            return -1;
        }

        if (get_v4l2_ctrl(SENSOR_DEV, V4L2_CID_VBLANK, &vblank) == 0) {
            printf("readback vertical_blanking = %d\n", vblank);
            printf("estimated fps = %.3f\n", 135000.0 / (2192 + vblank));
        }
    }

    if (isTimePerFrameSupported) {
        struct v4l2_streamparm streamparm{};
        memset(&streamparm, 0, sizeof(struct v4l2_streamparm));
        // 设置目标帧率：30fps = 1/30 秒每帧
        streamparm.type = v4l2BufType;
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = 30;

        if (ioctl(vfd, VIDIOC_S_PARM, &streamparm) < 0) {
            LOG(ERROR) << "can not set stream info";
            return -1;
        } else {
            memset(&streamparm, 0, sizeof(struct v4l2_streamparm));
            streamparm.type = v4l2BufType;
            ioctl(vfd, VIDIOC_G_PARM, &streamparm);
            // 驱动可能返回实际设置的帧率（可能与请求不同）
            LOG(INFO) << "Actual FPS set to: "
                      << streamparm.parm.capture.timeperframe.denominator /
                             streamparm.parm.capture.timeperframe.numerator;
        }
    }

    // 请求缓冲区
    struct v4l2_requestbuffers req{};
    req.type = v4l2BufType;
    req.count = REQUEST_BUF_COUNT;
    req.memory = mem_ == DMABUF ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        LOG(ERROR) << "can not request buffers";
        return -1;
    }
    printf("driver allocated %u buffers\n", req.count);
    if (req.count < REQUEST_BUF_COUNT) {
        LOG(WARNING) << "driver allocated fewer buffers than requested";
        return -1;
    }

    // 如果是MMAP, 查询并进行内存映射
    if (mem_ == mem_type::MMAP) {
        struct v4l2_buffer buf{};
        buf.type = v4l2BufType;
        buf.type = V4L2_MEMORY_MMAP;

        for (int i = 0; i < REQUEST_BUF_COUNT; ++i) {
            buf.index = i;
            struct v4l2_plane planes[this->mp_num];
            memset(planes, 0, sizeof(planes));

            if (isMplane) {
                buf.m.planes = planes;
                buf.length = mp_num;
            }

            if (ioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
                LOG(ERROR) << "can not query buffer " << i;
                continue;
            }

            if (isMplane) {
                this->buffers[i].length = buf.m.planes[0].length;
                this->buffers[i].offset = buf.m.planes[0].m.mem_offset;
            } else {
                this->buffers[i].length = buf.length;
                this->buffers[i].offset = buf.m.offset;
            }
            this->buffers[i].vaddr =
                mmap(NULL, this->buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, this->buffers[i].offset);
            if (this->buffers[i].vaddr == MAP_FAILED) {
                fmt::print("Mplane buffer {} mmap fail\n", i);
                continue;
            }
            fmt::print("MMAP buffer: index={}, vaddr={}, length={}, offset={:#x}\n", i, this->buffers[i].vaddr,
                       this->buffers[i].length, this->buffers[i].offset);
        }

        // 加入队列
        for (int i = 0; i < REQUEST_BUF_COUNT; i++) {
            buf.index = i;
            if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
                LOG(ERROR) << "can not enqueue buffer " << i;
                return -1;
            }
        }
    }

    // DMBUF 缓冲
    if (mem_ == DMABUF) {
        // DMA alloc
        LOG(INFO) << "Using DMABUF memory";
        for (int i = 0; i < REQUEST_BUF_COUNT; i++) {
            int fd = -1;
            void *addr = NULL;
            int length = frame_size;
            if (dma_buf_alloc("/dev/dma_heap/cma", length, &fd, &addr) < 0) {
                LOG(ERROR) << "can not allocate dma buffer " << i;
                return -1;
            }
            this->buffers[i].length = length;
            this->buffers[i].dmafd = fd;
            this->buffers[i].vaddr = addr;
            fmt::print("Allocated DMABUF: index={}, fd={}, vaddr={}, length={}\n", i, fd, addr, length);
        }

        // 查询 DMABUF

        // DMA fd 加入队列
        struct v4l2_buffer buf{};
        buf.type = v4l2BufType;
        buf.memory = V4L2_MEMORY_DMABUF;

        for (int i = 0; i < REQUEST_BUF_COUNT; ++i) {
            struct v4l2_plane planes[1];
            memset(planes, 0, sizeof(struct v4l2_plane));

            buf.index = i;
            if (isMplane) {
                buf.m.planes = planes;
                buf.length = 1;
                buf.m.planes[0].m.fd = this->buffers[i].dmafd;
                buf.m.planes[0].length = this->buffers[i].length;
            } else {
                buf.length = this->buffers[i].length;
                buf.m.fd = this->buffers[i].dmafd;
            }
            int ret = ioctl(vfd, VIDIOC_QBUF, &buf);
            if (ret < 0) {
                perror("VIDIOC_QBUF");
                LOG(ERROR) << "DMABUF QBUF error, errno=" << errno;
                return -1;
            }
        }

        for (int i = 0; i < REQUEST_BUF_COUNT; ++i) {
            buf.index = i;
            struct v4l2_plane planes[this->mp_num];
            memset(planes, 0, sizeof(struct v4l2_plane) * this->mp_num);

            if (ioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
                LOG(ERROR) << "DMABUF QUERYBUF error " << i;
                continue;
            }

            int fd = -1;
            size_t length = 0;
            size_t usedbytes = 0;
            if (isMplane) {
                fd = buf.m.planes[0].m.fd;
                length = buf.m.planes[0].length;
                usedbytes = buf.m.planes[0].bytesused;
            } else {
                fd = buf.m.fd;
                length = buf.length;
                usedbytes = buf.bytesused;
            }

            fmt::print("QUARY DMABUF index={}, fd={}, length={}, usedvytes={}\n", i, fd, length, usedbytes);
        }
    }

    // 启动采集
    if (isStreaming) {
        if (ioctl(vfd, VIDIOC_STREAMON, &v4l2BufType) < 0) {
            LOG(ERROR) << "can not start streaming";
            return -1;
        }
        LOG(INFO) << "streaming started";
        isCapturing = true;
    }
    return 0;
}

DmaBuf Video::cap_frame_get() {

    struct v4l2_plane planes[1]{};

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_DMABUF,
        .m =
            {
                .planes = &planes[0],
            },
        .length = 1,
    };

    // 从采集队列中取出一个缓冲区
    if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return {};
    }

    return std::move(DBufs[buf.index]);
}

void Video::cap_frame_put(DmaBuf &&dmabuf) {
    // DMA fd 加入队列
    struct v4l2_buffer buf = {
        .type = v4l2BufType,
        .memory = V4L2_MEMORY_DMABUF,
    };

    struct v4l2_plane planes[1]{};

    auto it = fdmapindex.find(dmabuf.getFd());
    if (it == fdmapindex.end())
        return;

    // 获取索引
    int index = it->second;
    // 移动资源
    if (DBufs[index].isValid()) {
        LOG(ERROR) << "cap_frame_put error: dmabuf move to valid dmabuf";
    }
    DBufs[index] = std::move(dmabuf);

    buf.index = index;
    buf.m.planes = planes;
    buf.length = 1;
    buf.m.planes[0].m.fd = DBufs[index].getFd();
    buf.m.planes[0].length = DBufs[index].getSize();

    int ret = ioctl(vfd, VIDIOC_QBUF, &buf);
    if (ret < 0) {
        perror("VIDIOC_QBUF");
    }
}

int Video::captrue_mp_dma_test() {
    int ret = 0;

    if (!isCapturing) {
        return -1;
    }

    struct v4l2_plane planes[1];
    memset(&planes, 0, sizeof(struct v4l2_plane));

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(struct v4l2_buffer));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;
    buf.length = 1;

    // 从采集队列中取出一个缓冲区
    if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return -1;
    }

    fflush(stdout);
    fmt::print("DMABUF QBUF: index={}, seq={}, fd={}, bytesused={}\r", buf.index, buf.sequence, buf.m.planes[0].m.fd,
               buf.m.planes[0].bytesused);

    src = wrapbuffer_fd(buffers[buf.index].dmafd, width, height, RK_FORMAT_VYUY_422);
    src_rect.x = 0;
    src_rect.y = 0;
    src_rect.width = width;
    src_rect.height = height;

    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.width = width;
    dst_rect.height = height;

    ret = imcheck(src, dst, src_rect, dst_rect);
    if (IM_STATUS_NOERROR != ret) {
        fprintf(stderr, "rga check error! %s", imStrError((IM_STATUS)ret));
        return -1;
    }

    IM_STATUS status = imcvtcolor(src, dst, RK_FORMAT_VYUY_422, RK_FORMAT_RGB_888);
    if (status != IM_STATUS_SUCCESS) {
        perror("imcvtcolor");
        return -1;
    }

    // 同步到CPU
    dma_sync_device_to_cpu(buffers[buf.index].dmafd);

    // 读取NV12 数据 buffers[buf.index].vaddr 到OpenCV
    // cv::Mat raw_uvuy(height, width, CV_8UC2, buffers[buf.index].vaddr);
    // cv::Mat bgr;
    // cv::cvtColor(raw_uvuy, bgr, cv::COLOR_YUV2BGR_UYVY);
    // if (bgr.empty()) {
    //     fmt::print("Color convert fail\n");
    //     ret = -1;
    // }
    cv::Mat rgb(height, width, CV_8UC3, rga_dst_buf);
    cv::imshow("capture", rgb);

    int key = cv::waitKey(40);
    if (key == 27 || key == 'q') {
        ret = -1;
    }

    // if (!cv::imwrite("/home/rock/c_cpp/stream_infer/asset/captrue_" + std::to_string(buf.sequence) + ".jpg",
    // bgr)) {
    //     fmt::print("Image write fail\n");
    //     return -1;
    // }

    // 处理完成后，将缓冲区重新入队
    if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }

    return ret;
}

void Video::capture_test() {
    if (!isCapturing) {
        // LOG(WARNING) << "not capturing";
        return;
    }

    struct v4l2_plane planes[1];
    memset(&planes, 0, sizeof(struct v4l2_plane));

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(struct v4l2_buffer));

    buf.type = v4l2BufType;
    buf.memory = V4L2_MEMORY_MMAP;
    if (isMplane) {
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
    }
    // 从采集队列中取出一个缓冲区
    if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
        LOG(ERROR) << "can not dequeue buffer";
        return;
    }
    if (isMplane) {
        buffers[buf.index].used_len = buf.m.planes[0].bytesused;
        printf("index=%u plane0.bytesused=%u sequence=%u timestamp=%ld.%06ld\n", buf.index, buf.m.planes[0].bytesused,
               buf.sequence, buf.timestamp.tv_sec, buf.timestamp.tv_usec);
    } else {
        buffers[buf.index].used_len = buf.bytesused;
        printf("index=%u bytesused=%u sequence=%u timestamp=%ld.%06ld\n", buf.index, buf.bytesused, buf.sequence,
               buf.timestamp.tv_sec, buf.timestamp.tv_usec);
    }
    // 处理捕获到的数据，数据地址为 buffers[buf.index].start，长度为 buf.length
    cv::Mat img;
    if (fmt_ == NV12) {
        cv::Mat timg = cv::Mat(height * 3 / 2, width, CV_8UC1, buffers[buf.index].vaddr);
        cv::cvtColor(timg, img, cv::COLOR_YUV2BGR_NV12);
    } else if (fmt_ == MJPG) {
        std::vector<uchar> jpg_data((uchar *)buffers[buf.index].vaddr,
                                    (uchar *)buffers[buf.index].vaddr + buffers[buf.index].used_len);
        img = cv::imdecode(jpg_data, cv::IMREAD_COLOR);
    }
    cv::imwrite("/home/rock/c_cpp/stream_infer/asset/captrue_" + std::to_string(buf.sequence) + ".jpg", img);

    // 处理完成后，将缓冲区重新入队
    if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
        LOG(ERROR) << "can not re-enqueue buffer";
    }
}