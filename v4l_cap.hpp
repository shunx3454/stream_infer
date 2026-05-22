#pragma once

#include <asm-generic/errno-base.h>
#include <cerrno>
#include <cstdio>
#include <fmt/core.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <opencv2/core/mat.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glog/logging.h>

#include "dma_alloc.h"
#include "im2d_buffer.h"
#include "im2d_single.h"
#include <RockchipRga.h>
#include <im2d.hpp>

#define SENSOR_DEV "/dev/v4l-subdev2"
#define MIN_VBLANK 58
#define MAX_VBLANK 30575
#define REQUEST_BUF_COUNT 4

struct Buffer {
    void *vaddr;
    size_t length;
    size_t used_len;
    size_t offset;
    int dmafd;
};

class Video {
  public:
    enum fmt_type { NV12, MJPG };
    enum mem_type { MMAP, DMABUF };

  private:
    int vfd;
    bool isMplane;
    bool isStreaming;
    bool isCapturing;
    bool isTimePerFrameSupported;
    enum v4l2_buf_type buf_type;
    enum v4l2_memory mem_type;

    int mp_num{0};

    Buffer buffers[REQUEST_BUF_COUNT];
    size_t width;
    size_t height;
    size_t frame_size{0};
    int rga_dst_dma_fd{-1};
    void *rga_dst_buf{NULL};

    rga_buffer_t src{};
    rga_buffer_t dst{};
    rga_buffer_handle_t src_handle{};
    rga_buffer_handle_t dst_handle{};
    im_rect src_rect{};
    im_rect dst_rect{};

    enum fmt_type fmt_ { NV12 };
    enum mem_type mem_ { MMAP };

  public:
    Video()
        : vfd(-1), isMplane(0), isStreaming(0), isCapturing(0), isTimePerFrameSupported(0),
          buf_type(V4L2_BUF_TYPE_VIDEO_CAPTURE), width(0), height(0), fmt_(fmt_type::NV12) {}

    ~Video() {
        streamoff();
        close(vfd);
    }

    int init(const char *dev) {
        vfd = open(dev, O_RDWR);
        if (vfd < 0) {
            throw std::invalid_argument("can not open file");
        }

        struct v4l2_capability capcity{};
        int ret = ioctl(vfd, VIDIOC_QUERYCAP, &capcity);
        if (ret < 0) {
            perror("VIDIOC_QUERYCAP");
            return -1;
        }

        LOG(INFO) << "driver: " << capcity.driver;
        LOG(INFO) << "version: " << capcity.version;
        LOG(INFO) << "card: " << capcity.card;
        LOG(INFO) << "bus_info: " << capcity.bus_info;
        LOG(INFO) << "capabilities: 0x" << std::hex << capcity.capabilities;
        std::cout << "\n";

        // device class
        if (capcity.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
            LOG(INFO) << "multi planes capture device";
            isMplane = 1;
            buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        }
        if (capcity.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
            LOG(INFO) << "capture device";
            buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        }
        if (capcity.capabilities & V4L2_CAP_STREAMING) {
            LOG(INFO) << "stream capture device";
            isStreaming = true;
        }
        std::cout << "\n";

        // format query
        // V4L2_BUF_TYPE_VIDEO_OUTPUT 视频输出
        // V4L2_BUF_TYPE_VIDEO_CAPTURE 视频输入
        // V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 视频输入，多平面
        struct v4l2_fmtdesc fmtdesc{};
        fmtdesc.type = buf_type;
        fmtdesc.index = 0;
        while (ioctl(vfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
            if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUV420) {
                LOG(INFO) << "Support YUV420";
            } else if (fmtdesc.pixelformat == V4L2_PIX_FMT_NV12) {
                LOG(INFO) << "Support NV12";
            } else if (fmtdesc.pixelformat == V4L2_PIX_FMT_NV21) {
                LOG(INFO) << "Support NV21";
            } else if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUV420) {
                LOG(INFO) << "Support YUV420";
            } else if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG) {
                LOG(INFO) << "Support MJPEG";
            } else if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV) {
                LOG(INFO) << "Support YUYV 422";
            }
            LOG(INFO) << fmtdesc.description;
            LOG(INFO) << fmtdesc.pixelformat;
            fmtdesc.index++;
        }
        std::cout << "\n";

        // frame size query
        {
            LOG(INFO) << "Supported frame sizes and frame rates for format type ";
            struct v4l2_frmsizeenum framesize{};
            struct v4l2_frmivalenum frame_rate{};

            framesize.type = buf_type;
            framesize.pixel_format = V4L2_PIX_FMT_NV12;
            framesize.index = 0;
            while (ioctl(vfd, VIDIOC_ENUM_FRAMESIZES, &framesize) == 0) {
                LOG(INFO) << "NV12: " << framesize.stepwise.max_width << "x" << framesize.stepwise.max_height;
                framesize.index++;
                frame_rate.index = 0;
                frame_rate.pixel_format = framesize.pixel_format;
                frame_rate.width = framesize.stepwise.max_width;
                frame_rate.height = framesize.stepwise.max_height;
                frame_rate.type = framesize.type;
                while (ioctl(vfd, VIDIOC_ENUM_FRAMEINTERVALS, &frame_rate) == 0) {
                    frame_rate.index++;
                    double min_fps = (double)frame_rate.stepwise.min.denominator / frame_rate.stepwise.min.numerator;
                    double max_fps = (double)frame_rate.stepwise.max.denominator / frame_rate.stepwise.max.numerator;
                    double step_fps = (double)frame_rate.stepwise.step.denominator / frame_rate.stepwise.step.numerator;
                    printf("  [%u] %s: %.3f ~ %.3f fps (step %.3f)\n", frame_rate.index,
                           (frame_rate.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) ? "Continuous" : "Stepwise", min_fps,
                           max_fps, step_fps);
                }
            }

            framesize.pixel_format = V4L2_PIX_FMT_NV21;
            framesize.index = 0;
            while (ioctl(vfd, VIDIOC_ENUM_FRAMESIZES, &framesize) == 0) {
                LOG(INFO) << "NV21: " << framesize.stepwise.max_width << "x" << framesize.stepwise.max_height;
                framesize.index++;
                frame_rate.index = 0;
                frame_rate.pixel_format = framesize.pixel_format;
                frame_rate.width = framesize.stepwise.max_width;
                frame_rate.height = framesize.stepwise.max_height;
                frame_rate.type = framesize.type;
                while (ioctl(vfd, VIDIOC_ENUM_FRAMEINTERVALS, &frame_rate) == 0) {
                    frame_rate.index++;
                    double min_fps = (double)frame_rate.stepwise.min.denominator / frame_rate.stepwise.min.numerator;
                    double max_fps = (double)frame_rate.stepwise.max.denominator / frame_rate.stepwise.max.numerator;
                    double step_fps = (double)frame_rate.stepwise.step.denominator / frame_rate.stepwise.step.numerator;
                    printf("  [%u] %s: %.3f ~ %.3f fps (step %.3f)\n", frame_rate.index,
                           (frame_rate.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) ? "Continuous" : "Stepwise", min_fps,
                           max_fps, step_fps);
                }
            }

            framesize.pixel_format = V4L2_PIX_FMT_MJPEG;
            framesize.index = 0;
            while (ioctl(vfd, VIDIOC_ENUM_FRAMESIZES, &framesize) == 0) {
                LOG(INFO) << "MJPEG: " << framesize.discrete.width << "x" << framesize.discrete.height;
                framesize.index++;
                frame_rate.index = 0;
                frame_rate.pixel_format = framesize.pixel_format;
                frame_rate.width = framesize.discrete.width;
                frame_rate.height = framesize.discrete.height;
                frame_rate.type = framesize.type;
                while (ioctl(vfd, VIDIOC_ENUM_FRAMEINTERVALS, &frame_rate) == 0) {
                    frame_rate.index++;
                    double min_fps = (double)frame_rate.stepwise.min.denominator / frame_rate.stepwise.min.numerator;
                    double max_fps = (double)frame_rate.stepwise.max.denominator / frame_rate.stepwise.max.numerator;
                    double step_fps = (double)frame_rate.stepwise.step.denominator / frame_rate.stepwise.step.numerator;
                    printf("  [%u] %s: %.3f ~ %.3f fps (step %.3f)\n", frame_rate.index,
                           (frame_rate.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) ? "Continuous" : "Stepwise", min_fps,
                           max_fps, step_fps);
                }
            }

            framesize.pixel_format = V4L2_PIX_FMT_YUV420;
            framesize.index = 0;
            while (ioctl(vfd, VIDIOC_ENUM_FRAMESIZES, &framesize) == 0) {
                LOG(INFO) << "YUV420: " << framesize.discrete.width << "x" << framesize.discrete.height;
                framesize.index++;
                frame_rate.index = 0;
                frame_rate.pixel_format = framesize.pixel_format;
                frame_rate.width = framesize.discrete.width;
                frame_rate.height = framesize.discrete.height;
                frame_rate.type = framesize.type;
                while (ioctl(vfd, VIDIOC_ENUM_FRAMEINTERVALS, &frame_rate) == 0) {
                    frame_rate.index++;
                    double min_fps = (double)frame_rate.stepwise.min.denominator / frame_rate.stepwise.min.numerator;
                    double max_fps = (double)frame_rate.stepwise.max.denominator / frame_rate.stepwise.max.numerator;
                    double step_fps = (double)frame_rate.stepwise.step.denominator / frame_rate.stepwise.step.numerator;
                    printf("  [%u] %s: %.3f ~ %.3f fps (step %.3f)\n", frame_rate.index,
                           (frame_rate.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) ? "Continuous" : "Stepwise", min_fps,
                           max_fps, step_fps);
                }
            }
            std::cout << "\n";
        }

        // 查看当前格式设置
        LOG(INFO) << "Current format settings:";
        struct v4l2_format fmt{};
        fmt.type = buf_type;
        if (ioctl(vfd, VIDIOC_G_FMT, &fmt) < 0) {
            LOG(ERROR) << "can not get format";
        } else {
            if (isMplane) {
                LOG(INFO) << "Get fmt type: " << fmt.type;
                LOG(INFO) << "Get fmt width: " << fmt.fmt.pix_mp.width;
                LOG(INFO) << "Get fmt height: " << fmt.fmt.pix_mp.height;
                LOG(INFO) << "Get fmt pixfmt: " << fmt.fmt.pix_mp.pixelformat;
                LOG(INFO) << "Get fmt colorspace: " << fmt.fmt.pix_mp.colorspace;
            } else {
                LOG(INFO) << "Get fmt type: " << fmt.type;
                LOG(INFO) << "Get fmt width: " << fmt.fmt.pix.width;
                LOG(INFO) << "Get fmt height: " << fmt.fmt.pix.height;
                LOG(INFO) << "Get fmt pixfmt: " << fmt.fmt.pix.pixelformat;
                LOG(INFO) << "Get fmt colorspace: " << fmt.fmt.pix.colorspace;
            }
            std::cout << "\n";
        }

        // 流信息查询
        LOG(INFO) << "Current stream settings:";
        struct v4l2_streamparm streamparm{};
        streamparm.type = buf_type;
        if ((ret = ioctl(vfd, VIDIOC_G_PARM, &streamparm)) < 0) {
            LOG(ERROR) << "can not get stream info: " << ret;
        } else {
            LOG(INFO) << "Get stream cpability " << std::hex << streamparm.parm.capture.capability;

            if (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
                LOG(INFO) << "Device supports V4L2_CAP_TIMEPERFRAME";
                isTimePerFrameSupported = true;
            } else {
                LOG(WARNING) << "Device does not support V4L2_CAP_TIMEPERFRAME";
            }
        }
        std::cout << "\n";

        // 缓冲类型查询
        LOG(INFO) << "Buffer type: " << buf_type;
        struct v4l2_requestbuffers req{};
        memset(&req, 0, sizeof(req));
        req.type = buf_type;
        req.memory = V4L2_MEMORY_DMABUF; // 或 USERPTR/DMABUF
        req.count = 0;                   // 设为0时，驱动返回最大支持的缓冲区数
        if (ioctl(vfd, VIDIOC_REQBUFS, &req) == 0) {
            LOG(INFO) << "Capabilities: 0x" << std::hex << req.capabilities;
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP) {
                LOG(INFO) << "Supports MMAP";
            }
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_USERPTR) {
                LOG(INFO) << "Supports USERPTR";
            }
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF) {
                LOG(INFO) << "Supports DMABUF";
            }
            if (req.capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF) {
                LOG(INFO) << "Supports M2M_HOLD_CAPTURE_BUFFER";
            }
        } else {
            LOG(ERROR) << "can not get request buffers";
        }
        return 0;
    }

    int streamon_mp_dmabuf() {
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
        int mp0_img_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        fmt::print("Set fortmat: type={}, width={}, height={}, pixfmt={}, n_planes={} mp0_img_size={}\n", fmt.type,
                   width, height, pixelformat, num_planes, mp0_img_size);

        // 申请缓冲区
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(struct v4l2_requestbuffers));

        req.type = buf_type;
        req.count = REQUEST_BUF_COUNT;
        req.memory = V4L2_MEMORY_DMABUF;
        if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
            perror("VIDIOC_REQBUFS");
            return -1;
        }
        printf("driver allocated %u buffers\n", req.count);
        if (req.count < REQUEST_BUF_COUNT) {
            fmt::print("get buffers less than req\n");
            return -1;
        }

        // 申请DMABUF
        for (int i = 0; i < REQUEST_BUF_COUNT; i++) {
            int fd = -1;
            void *addr = NULL;
            int length = frame_size;
            if (dma_buf_alloc("/dev/dma_heap/cma", length, &fd, &addr) < 0) {
                perror("dma_buf_alloc");
                return -1;
            }
            this->buffers[i].length = length;
            this->buffers[i].dmafd = fd;
            this->buffers[i].vaddr = addr;
            fmt::print("Allocated DMABUF: index={}, fd={}, vaddr={}, length={}\n", i, fd, addr, length);
        }

        // DMA fd 加入队列
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(struct v4l2_buffer));

        buf.type = buf_type;
        buf.memory = V4L2_MEMORY_DMABUF;

        for (int i = 0; i < REQUEST_BUF_COUNT; ++i) {
            struct v4l2_plane planes[1];
            memset(planes, 0, sizeof(struct v4l2_plane));

            buf.index = i;
            buf.m.planes = planes;
            buf.length = 1;
            buf.m.planes[0].m.fd = this->buffers[i].dmafd;
            buf.m.planes[0].length = this->buffers[i].length;

            int ret = ioctl(vfd, VIDIOC_QBUF, &buf);
            if (ret < 0) {
                perror("VIDIOC_QBUF");
                return -1;
            }
        }

        // 申请 RGA DMABUF
        int rga_dst_buf_size = height * width * 3;
        ret = dma_buf_alloc("/dev/dma_heap/cma", rga_dst_buf_size, &rga_dst_dma_fd, (void **)&rga_dst_buf);
        if (ret < 0) {
            printf("alloc src dma_heap buffer failed!\n");
            return -1;
        }
        dst = wrapbuffer_fd(rga_dst_dma_fd, width, height, RK_FORMAT_RGB_888);

        // 启动采集
        if (isStreaming) {
            if (ioctl(vfd, VIDIOC_STREAMON, &buf_type) < 0) {
                LOG(ERROR) << "can not start streaming";
                return -1;
            }
            LOG(INFO) << "streaming started";
            isCapturing = true;
        }

        return 0;
    }

    int streamon(enum mem_type mem_t = mem_type::MMAP) {
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
            fmt.type = buf_type;
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
            streamparm.type = buf_type;
            streamparm.parm.capture.timeperframe.numerator = 1;
            streamparm.parm.capture.timeperframe.denominator = 30;

            if (ioctl(vfd, VIDIOC_S_PARM, &streamparm) < 0) {
                LOG(ERROR) << "can not set stream info";
                return -1;
            } else {
                memset(&streamparm, 0, sizeof(struct v4l2_streamparm));
                streamparm.type = buf_type;
                ioctl(vfd, VIDIOC_G_PARM, &streamparm);
                // 驱动可能返回实际设置的帧率（可能与请求不同）
                LOG(INFO) << "Actual FPS set to: "
                          << streamparm.parm.capture.timeperframe.denominator /
                                 streamparm.parm.capture.timeperframe.numerator;
            }
        }

        // 请求缓冲区
        struct v4l2_requestbuffers req{};
        req.type = buf_type;
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
            buf.type = buf_type;
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
                this->buffers[i].vaddr = mmap(NULL, this->buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd,
                                              this->buffers[i].offset);
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
            buf.type = buf_type;
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
            if (ioctl(vfd, VIDIOC_STREAMON, &buf_type) < 0) {
                LOG(ERROR) << "can not start streaming";
                return -1;
            }
            LOG(INFO) << "streaming started";
            isCapturing = true;
        }
        return 0;
    }

    void streamoff() {
        if (isCapturing) {
            if (ioctl(vfd, VIDIOC_STREAMOFF, &buf_type) < 0) {
                LOG(ERROR) << "can not stop streaming";
            }
            LOG(INFO) << "streaming stopped";
        }

        // 清除buffer
        struct v4l2_requestbuffers req;
        req.type = buf_type;
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

    int captrue_mp_dma_test() {
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
        fmt::print("DMABUF QBUF: index={}, seq={}, fd={}, bytesused={}\r", buf.index, buf.sequence,
                   buf.m.planes[0].m.fd, buf.m.planes[0].bytesused);

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

    void capture_test() {
        if (!isCapturing) {
            // LOG(WARNING) << "not capturing";
            return;
        }

        struct v4l2_plane planes[1];
        memset(&planes, 0, sizeof(struct v4l2_plane));

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(struct v4l2_buffer));

        buf.type = buf_type;
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
            printf("index=%u plane0.bytesused=%u sequence=%u timestamp=%ld.%06ld\n", buf.index,
                   buf.m.planes[0].bytesused, buf.sequence, buf.timestamp.tv_sec, buf.timestamp.tv_usec);
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

    void mpp_encode() {}

  private:
    /*
     * 你当前实测出的 IMX415 mode:
     *
     * height = 2192
     * min vertical_blanking = 58
     * base fps = 60
     *
     * fps = 135000 / (2192 + vblank)
     * vblank = 135000 / target_fps - 2192
     */
    static int calc_vblank_from_fps(int fps) {
        const int height = 2192;
        const int line_rate = 135000;

        if (fps <= 0) {
            return -1;
        }

        return line_rate / fps - height;
    }

    static int set_v4l2_ctrl(const char *dev, unsigned int id, int value) {
        int fd;
        struct v4l2_control ctrl;

        fd = open(dev, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
            return -1;
        }

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = id;
        ctrl.value = value;

        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            fprintf(stderr, "VIDIOC_S_CTRL failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }

        close(fd);
        return 0;
    }

    static int get_v4l2_ctrl(const char *dev, unsigned int id, int *value) {
        int fd;
        struct v4l2_control ctrl;

        fd = open(dev, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
            return -1;
        }

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = id;

        if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
            fprintf(stderr, "VIDIOC_G_CTRL failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }

        *value = ctrl.value;

        close(fd);
        return 0;
    }
};