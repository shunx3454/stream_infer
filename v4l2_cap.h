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
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glog/logging.h>

#include "dma_alloc.h"
#include "dmabuf.h"
#include "img_dmabuf.h"

#include "im2d_buffer.h"
#include "im2d_single.h"
#include <RockchipRga.h>
#include <im2d.hpp>

#define SENSOR_DEV "/dev/v4l-subdev2"
#define MIN_VBLANK 58
#define MAX_VBLANK 30575
#define REQUEST_BUF_COUNT 4

class Video {
  public:
  private:
    int vfd;
    bool isMplane;
    bool isStreaming;
    bool isCapturing;
    bool isTimePerFrameSupported;
    enum v4l2_buf_type v4l2BufType;
    enum v4l2_memory mem_type;

    // index:sptr
    std::mutex mtx_;
    std::unordered_map<unsigned int, std::shared_ptr<ImgDMABuf>> imgdbufs_;

    unsigned int width_;
    unsigned int height_;
    unsigned int pixfmt_;
    unsigned int fps_;
    unsigned int perlinebytes_;
    unsigned int imgsize_;
     int vblank_;
    unsigned int num_planes;

  public:
    Video() = delete;
    Video(unsigned int width, unsigned int height, unsigned int pixfmt, unsigned int fps, unsigned int perlinebytes,
          size_t imgsize);
    ~Video();

    int init(const char *dev);
    int streamon_mp_dmabuf(unsigned int n);
    int streamon_dmabuf(unsigned int n);
    int streamon(unsigned int n);
    void streamoff();

    std::shared_ptr<ImgDMABuf> cap_frame_get();
    void cap_frame_put(std::shared_ptr<ImgDMABuf> pb);

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
        const int min_vblank = 58;
        const int max_vblank = 30575;

        if (fps <= 0) {
            return -1;
        }

        int vblank = (line_rate + fps / 2) / fps - height;

        if (vblank < min_vblank) {
            vblank = min_vblank;
        } else if (vblank > max_vblank) {
            vblank = max_vblank;
        }

        return vblank;
    }

    int set_v4l2_ctrl(unsigned int id, int value) {
        int fd = open(SENSOR_DEV, O_RDWR);
        if (fd < 0) {
            return -errno;
        }

        struct v4l2_control ctrl = {
            .id = id,
            .value = value,
        };

        if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            fmt::print("VIDIOC_S_CTRL failed, reason={}\n", strerror(errno));
            close(fd);
            return -errno;
        }

        close(fd);
        return 0;
    }

    int set_sensor_fps(int fps) {
        int vblank = calc_vblank_from_fps(fps);
        if (vblank < 0) {
            return -EINVAL;
        }

        return set_v4l2_ctrl(V4L2_CID_VBLANK, vblank);
    }

    int get_v4l2_ctrl(unsigned int id, int *value) {
        int fd = open(SENSOR_DEV, O_RDWR);
        if (fd < 0) {
            return -errno;
        }

        struct v4l2_control ctrl = {
            .id = id,
        };

        if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
            fmt::print("\t#VIDIOC_S_CTRL failed, reason={}\n", strerror(errno));
            close(fd);
            return -errno;
        }

        *value = ctrl.value;

        close(fd);
        return 0;
    }
};