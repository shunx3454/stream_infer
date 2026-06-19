/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include <fcntl.h>
#include <fmt/core.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <ctime>

#include <RockchipRga.h>
#include <im2d.hpp>
#include <im2d_type.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv4/opencv2/opencv.hpp>

#include "mpp_enc.h"
#include "rtsp.h"
#include "v4l2_cap.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "dma_alloc.h"
#include "postprocess.h"
#include "preprocess.h"
#include "utils.h"

#include "dmabuf.h"
#include "rga.h"
#include "rknn.h"

#define W_ALIGN 16
#define H_ALIGN 2

int main(int argc, char *argv[]) {

    unsigned int rknn_img_w = 640;
    unsigned int rknn_img_h = 640;
    unsigned int rknn_img_fmt = 640;

    unsigned int w = 1920;
    unsigned int h = 1088;
    unsigned int w_s = w * 2;
    unsigned int h_s = h;
    unsigned int fps = 60;
    unsigned int gop = 60;
    unsigned int v4l2_fmt = V4L2_PIX_FMT_UYVY;
    MppFrameFormat mpp_fmt = MPP_FMT_YUV422_UYVY;

    // unsigned int w = 640;
    // unsigned int h = 480;
    // unsigned int w_s = w * 2;
    // unsigned int h_s = h;
    // unsigned int fps = 12;
    // unsigned int gop = 12;
    // unsigned int v4l2_fmt = V4L2_PIX_FMT_YUYV;
    // MppFrameFormat mpp_fmt = MPP_FMT_YUV422_YUYV;

    if (argc < 1) {
        return -1;
    }

    // imgd pool
    ImgDMABufPool Pool1(4, w, h, w_s, h_s, v4l2_fmt);
    ImgDMABufPool Pool2(4, w, h, w_s, h_s, v4l2_fmt);

    // v4l2 video
    Video video(w, h, v4l2_fmt, fps, w_s, w_s * h_s);
    video.init(argv[1]);
    video.streamon(4);

    // mpp
    MppEncoder encoder(w, h, mpp_fmt, w_s, h_s, fps, gop);
    encoder.init();
    auto hdr = encoder.getHdr();

    // rtsp
    RtspPusher rtsp_client;
    rtsp_client.init("rtsp://192.168.31.100:8554/test", w, h, fps, hdr.data(), hdr.size());

    // rknn
    RKNN rknn("/home/rock/c_cpp/stream_infer/model/person_relu.rknn");

    std::atomic<bool> exited = false;
    int target_frames = fps * 10;
    auto pktDbuf = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt, 0);

    std::thread t1([&video, &Pool1, &exited, &encoder, &pktDbuf]() {
        while (!exited) {
            video.cap_frame_put(Pool1.get(exited));
        }
    });

    std::thread t2([&video, &Pool1, &encoder, &exited, &pktDbuf, &fps, &rtsp_client, &rknn, &target_frames]() {
        int i = 0;
        // std::ofstream fl("/home/rock/c_cpp/stream_infer/640x480_YUYV_frames.yuv",
        //                  std::ios::binary | std::ios::trunc);
        // if (!fl.is_open()) {
        //     return;
        // }
        // auto yuv_data = read_file("/home/rock/c_cpp/stream_infer/800x600_YUYV_frames.yuv");


        size_t pos = 0;
        while (!exited) {
            auto t1 = get_now_ms();
            auto dbuf = video.cap_frame_get();
            dbuf->syncDeviceToCpu();

            // frmDbuf->syncDeviceToCpu();
            // memcpy(frmDbuf->getVa(), yuv_data.data() + pos, frmDbuf->getSize());
            // frmDbuf->syncCpuToDevice();
            // pos += frmDbuf->getSize();
            // if (pos >= yuv_data.size()) {
            //     pos = 0;
            // }

            // frmDbuf->syncDeviceToCpu();
            // fl.write((const char*)dbuf->getVa(), dbuf->getSize());


            if (rknn.infer(dbuf) < 0) {
                continue;
            }

            auto t2 = get_now_ms();

            int64_t t3, t4;
            encoder.encode(dbuf, pktDbuf, (i % fps) == 0 ? 1 : 0, 0,
                           [&rtsp_client, &t3, &t4](const void *ptr, size_t len, RK_U32 is_keyframe, RK_U32 eos) {
                               t3 = get_now_ms();
                               rtsp_client.push_h264_packet((const uint8_t *)ptr, len, is_keyframe);
                               t4 = get_now_ms();
                           });
            // encoder.encode(dbuf, pktDbuf, (i % fps) == 0 ? 1 : 0, i == (target_frames - 1) ? 1 : 0);

            Pool1.put(dbuf, exited);

            auto t5 = get_now_ms();

            fmt::print("time elapse: {}, {}, {}, {}\n", t2 - t1, t3 - t2, t4 - t3, t5 - t1);

            // std::this_thread::sleep_for(std::chrono::milliseconds(45));

            ++i;
            // if (i == target_frames) {
            //     exited.store(true);
            //     Pool.wakeup();
            //     break;
            // }
        }
    });

    t1.join();
    t2.join();

    return 0;
}