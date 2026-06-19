#pragma once

#include <RockchipRga.h>
#include <im2d.hpp>
#include <im2d_type.h>

#include "img_dmabuf.h"
#include "postprocess.h"

#include <memory>
#include <iostream>
#include <fmt/core.h>
#include <fmt/chrono.h>

#include <linux/videodev2.h>

class RGA
{
private:
    /* data */

    static int v4l2FmtToRga(int v4l2fmt) {
        if(v4l2fmt == V4L2_PIX_FMT_UYVY) 
            return RK_FORMAT_UYVY_422;
        else if(v4l2fmt == V4L2_PIX_FMT_YUYV)
            return RK_FORMAT_YUYV_422;
        else if(v4l2fmt == V4L2_PIX_FMT_RGB24)
            return RK_FORMAT_RGB_888;
        
        else return -1;
    }

public:
    RGA(/* args */);

    static void resizeAndCvtColor(std::shared_ptr<ImgDMABuf> imgd_i, std::shared_ptr<ImgDMABuf> imgd_o);
    static void drawRect(std::shared_ptr<ImgDMABuf> imgd, detect_result_group_t group);

    ~RGA();
};
