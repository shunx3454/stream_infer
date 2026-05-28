#pragma once

#include <asm-generic/errno-base.h>
#include <cerrno>
#include <cstdio>
#include <fmt/core.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
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
#include "dmabuf.h"
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
    enum mem_type { NONE, MMAP, DMABUF };

  private:
    int vfd;
    bool isMplane;
    bool isStreaming;
    bool isCapturing;
    bool isTimePerFrameSupported;
    enum v4l2_buf_type v4l2BufType;
    enum v4l2_memory mem_type;

    int mp_num{0};

    std::vector<Buffer> buffers;
    std::vector<DmaBuf> DBufs;
    std::unordered_map<int, int> fdmapindex;
    std::unordered_map<int, int> indexmapfd;

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
    enum mem_type mem_ { NONE };

  public:
    Video()
        : vfd(-1), isMplane(0), isStreaming(0), isCapturing(0), isTimePerFrameSupported(0),
          v4l2BufType(V4L2_BUF_TYPE_VIDEO_CAPTURE), width(0), height(0), fmt_(fmt_type::NV12) {}

    ~Video() {
        streamoff();
        close(vfd);
    }

    int init(const char *dev);
    int streamon_mp_dmabuf(std::vector<DmaBuf> &dbufs);

    DmaBuf cap_frame_get();
    void cap_frame_put(DmaBuf &&dmabuf);

    int streamon(enum mem_type mem_t = mem_type::MMAP);

    void streamoff();

    int captrue_mp_dma_test();

    void capture_test();

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