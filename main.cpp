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

#include "avfmt_img.h"
#include "dmabuf.h"
#include "lf_queue.hpp"
#include "rga.h"
#include "rknn.h"

#define W_ALIGN 16
#define H_ALIGN 2

struct videoThreadCfg {
    int width;
    int height;
    int v4l2Fmt;
    int fps;
    int width_stride;
    int height_stride;
    int size;
    int n_buffers;
    int stream_id;

    LFArrayQueue<std::shared_ptr<ImgDMABuf>, 16> *rknn_in_queue;
    std::string path;
};

struct ProcessAndStreamingThreadCfg {
    videoThreadCfg vcfg;
    MppFrameFormat mppFmt;
    std::string rtsp_url;
    LFArrayQueue<std::shared_ptr<ImgDMABuf>, 16> *rknn_out_queue;
    int gop;
    int stream_count;
};

struct rknnThreadCfg {
    std::string model_path;
    LFArrayQueue<std::shared_ptr<ImgDMABuf>, 16> *rknn_in_queue;
    LFArrayQueue<std::shared_ptr<ImgDMABuf>, 16> *rknn_out_queue;
    rknn_core_mask cm;
};

void v4l2FrameThread(videoThreadCfg *cfg) {
    // 视频流初始化
    Video video(cfg->width, cfg->height, cfg->v4l2Fmt, cfg->fps, cfg->width_stride, cfg->size);
    video.init(cfg->path.c_str());
    video.streamon(cfg->n_buffers);
    int64_t frameDurationMS = 1000. / (double)cfg->fps;

    // 视频帧队列
    auto *stream_queue = new LFArrayQueue<std::shared_ptr<ImgDMABuf>, 4>();

    // QBUF
    for (int i = 0; i < cfg->n_buffers; ++i) {
        auto imgd =
            std::make_shared<ImgDMABuf>(cfg->width, cfg->height, cfg->width_stride, cfg->height_stride, cfg->v4l2Fmt);
        imgd->img_set_index(i);
        imgd->img_set_stream_id(cfg->stream_id);
        imgd->img_set_user_data(stream_queue);
        imgd->img_set_fps(cfg->fps);
        stream_queue->push(imgd);
        imgd.reset();
    }

    std::thread t([&stream_queue, &video] {
        std::shared_ptr<ImgDMABuf> oimgd;

        // QBUF
        for (;;) {
            while (!stream_queue->pop(oimgd)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            video.cap_frame_put(oimgd);

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    for (;;) {
        // DBUF
        auto imgd = video.cap_frame_get();
        imgd->syncDeviceToCpu();

        // send to rknn_in_queue
        while (!cfg->rknn_in_queue->push(imgd)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        imgd.reset();
    }

    t.join();
    delete stream_queue;
}

void avfmtFrameThread(videoThreadCfg *cfg) {
    // avfmt video
    AvFmtImgReader avimg(cfg->path, cfg->v4l2Fmt, AvFmtDecoderPreference::Hardware, true);
    fmt::print("dec name:{}, hwacc:{}, src_w:{}, src_h:{}, out_w:{}, out_h:{}, out_sw:{}, fmt:{}, fps:{}, size:{}\n",
               avimg.decoderName(), avimg.usingHardwareDecoder(), avimg.sourceWidth(), avimg.sourceHeight(),
               avimg.outputWidth(), avimg.outputHeight(), avimg.outputStride(), avimg.outputPixfmt(), avimg.frameRate(),
               avimg.outputSize());
    auto frameDurationMS = avimg.frameDurationUs() / 1000;

    // 视频帧队列
    auto *stream_queue = new LFArrayQueue<std::shared_ptr<ImgDMABuf>, 4>;
    for (int i = 0; i < cfg->n_buffers; ++i) {
        auto imgd = std::make_shared<ImgDMABuf>(avimg.outputWidth(), avimg.outputHeight(), avimg.outputStride(),
                                                avimg.outputHeight(), cfg->v4l2Fmt);
        imgd->img_set_stream_id(cfg->stream_id);
        imgd->img_set_user_data(stream_queue);
        imgd->img_set_fps(cfg->fps);
        stream_queue->push(imgd);
        imgd.reset();
    }

    // avfmt 解码
    AvFmtImgFrame frame{};
    std::shared_ptr<ImgDMABuf> imgd = nullptr;

    for (;;) {
        auto ts = get_now_ms();

        if (stream_queue->pop(imgd)) {
            // 传递流配置信息
            while (!avimg.read(imgd, frame))
                ;
            cfg->rknn_in_queue->push(imgd);
        }

        // 帧率控制
        int64_t sleepMS = frameDurationMS - (get_now_ms() - ts);
        if (sleepMS < 0)
            sleepMS = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMS));
    }

    delete stream_queue;
}

void imgInferThread(rknnThreadCfg *cfg) {
    // rknn
    RKNN rknn(cfg->model_path.c_str(), cfg->cm);
    std::shared_ptr<ImgDMABuf> imgd = nullptr;

    for (;;) {
        while (!cfg->rknn_in_queue->pop(imgd)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (rknn.infer(imgd) < 0) {
            fmt::print("rknn.infer error\n");
        }

        while (!cfg->rknn_out_queue->push(imgd))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        imgd.reset();
    }
}

void processAndRTSPThread(ProcessAndStreamingThreadCfg *cfg) {
    RGA rga;

    // mpp
    MppEncoder encoder(cfg->vcfg.width, cfg->vcfg.height, cfg->mppFmt, cfg->vcfg.width_stride, cfg->vcfg.height_stride,
                       cfg->vcfg.fps, cfg->gop);
    encoder.init();
    auto hdr = encoder.getHdr();

    // rtsp
    RtspPusher rtsp_client;
    rtsp_client.init(cfg->rtsp_url, cfg->vcfg.width, cfg->vcfg.height, cfg->vcfg.fps, hdr.data(), hdr.size());

    // 建立4个待RGA缩放的imgds数组
    std::vector<std::shared_ptr<ImgDMABuf>> imgds(cfg->stream_count);
    // 4个RGA缩放后的imgds数组
    std::vector<std::shared_ptr<ImgDMABuf>> rga_imgds(cfg->stream_count);
    for (int i = 0; i < cfg->stream_count; ++i) {
        rga_imgds[i] =
            std::make_shared<ImgDMABuf>(cfg->vcfg.width / 2, cfg->vcfg.height / 2, cfg->vcfg.width_stride / 2,
                                        cfg->vcfg.height_stride / 2, cfg->vcfg.v4l2Fmt);
    }

    // 以及4个新imgd标志位
    // int ready_imgs = 0;

    // 建立一个大的 imgd, 编码器输入
    std::shared_ptr<ImgDMABuf> Imgd = std::make_shared<ImgDMABuf>(
        cfg->vcfg.width, cfg->vcfg.height, cfg->vcfg.width_stride, cfg->vcfg.height_stride, cfg->vcfg.v4l2Fmt);
    size_t Imgd_seq = 0;

    // 一个大的pktd 编码器输出，rtsp输入
    std::shared_ptr<ImgDMABuf> pktd = std::make_shared<ImgDMABuf>(
        cfg->vcfg.width, cfg->vcfg.height, cfg->vcfg.width_stride, cfg->vcfg.height_stride, cfg->vcfg.v4l2Fmt);

    std::shared_ptr<ImgDMABuf> im;
    int64_t frameDurationMS = 1000. / (double)cfg->vcfg.fps;
    int64_t ts, t1, t2, t3, t4;

    for (;;) {
        ts = get_now_ms();

        // 获取 rknn out imgd
        while (cfg->rknn_out_queue->pop(im)) {
            auto *stream_queue = (LFArrayQueue<std::shared_ptr<ImgDMABuf>, 4> *)im->img_get_user_data();
            int stream_id = im->img_get_stream_id();

            if (imgds[stream_id] != nullptr) {
                // 释放上一帧 占用
                stream_queue->push(imgds[stream_id]);
                imgds[stream_id].reset();
            }
            // 保存最新的
            imgds[stream_id] = std::move(im);
            // RGA缩放
            RGA::resizeAndCvtColor(imgds[stream_id], rga_imgds[stream_id]);
        }

        t1 = get_now_ms();

        // RGA拼接更新
        RGA::spliceImgs(rga_imgds, Imgd);

        t2 = get_now_ms();

        // MPP 编码 & RTSP 发送
        encoder.encode(Imgd, pktd, (Imgd_seq % cfg->vcfg.fps) == 0 ? 1 : 0, 0,
                       [&](const void *ptr, size_t len, RK_U32 is_keyframe, RK_U32 eos) {
                           t3 = get_now_ms();
                           rtsp_client.push_h264_packet((const uint8_t *)ptr, len, is_keyframe);
                           t4 = get_now_ms();
                       });
        ++Imgd_seq;

        auto t5 = get_now_ms();
        fmt::print("time elapse: get rknn img {} ms, rga process {} ms, mpp encode {} ms, rtsp send {} ms, process "
                   "completion {} ms\n",
                   t1 - ts, t2 - t1, t3 - t2, t4 - t3, t5 - ts);

        int64_t sleepms = frameDurationMS - (get_now_ms() - ts);
        if (sleepms < 0)
            sleepms = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepms));
    }
}

int main(int argc, char *argv[]) {

    unsigned int rknn_img_w = 640;
    unsigned int rknn_img_h = 640;
    unsigned int rknn_img_fmt = 640;

    // unsigned int w = 1920;
    // unsigned int h = 1088;
    // unsigned int w_s = w * 2;
    // unsigned int h_s = h;
    // unsigned int fps = 60;
    // unsigned int gop = 60;
    // unsigned int v4l2_fmt = V4L2_PIX_FMT_UYVY;
    // MppFrameFormat mpp_fmt = MPP_FMT_YUV422_UYVY;

    // unsigned int w = 640;
    // unsigned int h = 360;
    // unsigned int w_s = w * 2;
    // unsigned int h_s = h;
    // unsigned int fps = 30;
    // unsigned int gop = 30;
    // unsigned int v4l2_fmt = V4L2_PIX_FMT_YUYV;
    // MppFrameFormat mpp_fmt = MPP_FMT_YUV422_YUYV;

    auto *rknn_in_queue = new LFArrayQueue<std::shared_ptr<ImgDMABuf>, 16>();
    auto *rknn_out_queue = new LFArrayQueue<std::shared_ptr<ImgDMABuf>, 16>();

    // v4l2 获取帧
    videoThreadCfg *cfg0 = new videoThreadCfg();
    cfg0->path = "/dev/video11";
    cfg0->width = 1920;
    cfg0->height = 1088;
    cfg0->v4l2Fmt = V4L2_PIX_FMT_UYVY;
    cfg0->width_stride = cfg0->width * 2;
    cfg0->height_stride = cfg0->height;
    cfg0->size = cfg0->width_stride * cfg0->height_stride;
    cfg0->fps = 60;
    cfg0->stream_id = 0;
    cfg0->n_buffers = 4;
    cfg0->rknn_in_queue = rknn_in_queue;
    std::thread v4l2Thread0(&v4l2FrameThread, cfg0);

    videoThreadCfg *cfg1 = new videoThreadCfg();
    cfg1->path = "/dev/video21";
    cfg1->width = 640;
    cfg1->height = 480;
    cfg1->v4l2Fmt = V4L2_PIX_FMT_YUYV;
    cfg1->width_stride = cfg1->width * 2;
    cfg1->height_stride = cfg1->height;
    cfg1->size = cfg1->width_stride * cfg1->height_stride;
    cfg1->fps = 30;
    cfg1->stream_id = 1;
    cfg1->n_buffers = 4;
    cfg1->rknn_in_queue = rknn_in_queue;
    std::thread v4l2Thread1(&v4l2FrameThread, cfg1);

    // avfmt 获取帧
    videoThreadCfg *cfg2 = new videoThreadCfg();
    cfg2->path = "/home/rock/c_cpp/stream_infer/asset/CosmicPrincessKaguya_Reply_MV.mp4";
    cfg2->v4l2Fmt = V4L2_PIX_FMT_YUYV;
    cfg2->stream_id = 2;
    cfg2->n_buffers = 4;
    cfg2->rknn_in_queue = rknn_in_queue;
    std::thread avfmtThread0(&avfmtFrameThread, cfg2);

    videoThreadCfg *cfg3 = new videoThreadCfg();
    cfg3->path = "/home/rock/c_cpp/stream_infer/asset/CosmicPrincessKaguya_ray_MV.mp4";
    cfg3->v4l2Fmt = V4L2_PIX_FMT_YUYV;
    cfg3->stream_id = 3;
    cfg3->n_buffers = 4;
    cfg3->rknn_in_queue = rknn_in_queue;
    std::thread avfmtThread1(&avfmtFrameThread, cfg3);

    // rknn多线程推理
    rknnThreadCfg *rknnCfg0 = new rknnThreadCfg();
    rknnCfg0->cm = RKNN_NPU_CORE_0;
    rknnCfg0->model_path = "/home/rock/c_cpp/stream_infer/model/yolov5s_anime_rk3588.rknn";
    rknnCfg0->rknn_in_queue = rknn_in_queue;
    rknnCfg0->rknn_out_queue = rknn_out_queue;
    std::thread RknnThread0(&imgInferThread, rknnCfg0);

    rknnThreadCfg *rknnCfg1 = new rknnThreadCfg();
    rknnCfg1->cm = RKNN_NPU_CORE_1;
    rknnCfg1->model_path = "/home/rock/c_cpp/stream_infer/model/yolov5s_anime_rk3588.rknn";
    rknnCfg1->rknn_in_queue = rknn_in_queue;
    rknnCfg1->rknn_out_queue = rknn_out_queue;
    std::thread RknnThread1(&imgInferThread, rknnCfg1);

    rknnThreadCfg *rknnCfg2 = new rknnThreadCfg();
    rknnCfg2->cm = RKNN_NPU_CORE_2;
    rknnCfg2->model_path = "/home/rock/c_cpp/stream_infer/model/yolov5s_anime_rk3588.rknn";
    rknnCfg2->rknn_in_queue = rknn_in_queue;
    rknnCfg2->rknn_out_queue = rknn_out_queue;
    std::thread RknnThread2(&imgInferThread, rknnCfg2);

    // 拼接帧和推流
    ProcessAndStreamingThreadCfg *StreamingCfg = new ProcessAndStreamingThreadCfg();
    StreamingCfg->vcfg.path = "";
    StreamingCfg->vcfg.width = 1920;
    StreamingCfg->vcfg.height = 1088;
    StreamingCfg->vcfg.width_stride = StreamingCfg->vcfg.width * 2;
    StreamingCfg->vcfg.height_stride = StreamingCfg->vcfg.height;
    StreamingCfg->vcfg.v4l2Fmt = V4L2_PIX_FMT_UYVY;
    StreamingCfg->vcfg.fps = 60;
    StreamingCfg->vcfg.size = StreamingCfg->vcfg.width_stride * StreamingCfg->vcfg.height_stride;
    StreamingCfg->vcfg.rknn_in_queue = nullptr;
    StreamingCfg->stream_count = 4;

    StreamingCfg->gop = 30;
    StreamingCfg->mppFmt = MPP_FMT_YUV422_UYVY;
    StreamingCfg->rtsp_url = "rtsp://192.168.31.100:8554/test";
    StreamingCfg->rknn_out_queue = rknn_out_queue;
    std::thread StreamingProcessThread(&processAndRTSPThread, StreamingCfg);

    v4l2Thread0.join();
    v4l2Thread1.join();
    avfmtThread0.join();
    avfmtThread1.join();

    RknnThread0.join();
    RknnThread1.join();
    RknnThread2.join();

    StreamingProcessThread.join();

    delete rknn_in_queue;
    delete rknn_out_queue;

#if 0
    // mpp
    MppEncoder encoder(w, h, mpp_fmt, w_s, h_s, fps, gop);
    encoder.init();
    auto hdr = encoder.getHdr();

    // rtsp
    RtspPusher rtsp_client;
    rtsp_client.init("rtsp://192.168.31.100:8554/test", w, h, fps, hdr.data(), hdr.size());

    // avfmt
    AvFmtImgReader avimg("/home/rock/c_cpp/stream_infer/asset/1.mp4", V4L2_PIX_FMT_YUYV, AvFmtDecoderPreference::Auto,
                         true);
    fmt::print("dec name:{}, hwacc:{}, src_w:{}, src_h:{}, out_w:{}, out_h:{}, out_stride:{}, fmt:{}, size:{}\n",
               avimg.decoderName(), avimg.usingHardwareDecoder(), avimg.sourceWidth(), avimg.sourceHeight(),
               avimg.outputWidth(), avimg.outputHeight(), avimg.outputStride(), avimg.outputPixfmt(),
               avimg.outputSize());

    AvFmtImgFrame frame;
    std::shared_ptr<ImgDMABuf> imgd = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);
    std::shared_ptr<ImgDMABuf> pktd = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);

    std::thread t2([&]() {
        int i = 0;
        // std::ofstream fl("/home/rock/c_cpp/stream_infer/640x480_YUYV_frames.yuv",
        //                  std::ios::binary | std::ios::trunc);
        // if (!fl.is_open()) {
        //     return;
        // }
        // auto yuv_data = read_file("/home/rock/c_cpp/stream_infer/800x600_YUYV_frames.yuv");

        size_t pos = 0;
        while (true) {
            auto t1 = get_now_ms();

            if (!avimg.read(imgd, frame)) {
                continue;
            }

            auto t2 = get_now_ms();

            fps /= 4;
            int64_t t3, t4;
            encoder.encode(imgd, pktd, (i % fps) == 0 ? 1 : 0, 0,
                           [&rtsp_client, &t3, &t4](const void *ptr, size_t len, RK_U32 is_keyframe, RK_U32 eos) {
                               t3 = get_now_ms();
                               rtsp_client.push_h264_packet((const uint8_t *)ptr, len, is_keyframe);
                               t4 = get_now_ms();
                           });
            // encoder.encode(dbuf, pktDbuf, (i % fps) == 0 ? 1 : 0, i == (target_frames - 1) ? 1 : 0);

            auto t5 = get_now_ms();

            fmt::print("time elapse: {}, {}, {}, {}\n", t2 - t1, t3 - t2, t4 - t3, t5 - t1);

            std::this_thread::sleep_for(std::chrono::milliseconds(45));

            ++i;
            // if (i == target_frames) {
            //     exited.store(true);
            //     Pool.wakeup();
            //     break;
            // }
        }
    });

    t2.join();

#endif

#if 0
    RGA rga;

    Video video(w, h, v4l2_fmt, fps, w_s, w_s * h_s);
    video.init("/dev/video11");
    video.streamon(4);

    // mpp
    MppEncoder encoder(w, h, mpp_fmt, w_s, h_s, fps, gop);
    encoder.init();
    auto hdr = encoder.getHdr();

    // rtsp
    RtspPusher rtsp_client;
    rtsp_client.init("rtsp://192.168.31.100:8554/test", w, h, fps, hdr.data(), hdr.size());

    auto pktd = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);
    auto Imgd = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);

    auto imgd1 = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);
    auto imgd2 = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);
    auto imgd3 = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);
    auto imgd4 = std::make_shared<ImgDMABuf>(w, h, w_s, h_s, v4l2_fmt);
    imgd1->img_set_index(0);
    imgd2->img_set_index(1);
    imgd3->img_set_index(2);
    imgd4->img_set_index(3);
    // 保存RGA缩放后的图片
    std::vector<std::shared_ptr<ImgDMABuf>> rga_imgds(4);
    for (int i = 0; i < 4; ++i) {
        rga_imgds[i] = std::make_shared<ImgDMABuf>(w / 2, h / 2, w_s / 2, h_s / 2, v4l2_fmt);
    }

    video.cap_frame_put(imgd1);
    video.cap_frame_put(imgd2);
    video.cap_frame_put(imgd3);
    video.cap_frame_put(imgd4);


    std::thread t2([&]() {
        int i = 0;
        // std::ofstream fl("/home/rock/c_cpp/stream_infer/640x480_YUYV_frames.yuv",
        //                  std::ios::binary | std::ios::trunc);
        // if (!fl.is_open()) {
        //     return;
        // }
        // auto yuv_data = read_file("/home/rock/c_cpp/stream_infer/800x600_YUYV_frames.yuv");

        size_t pos = 0;
        while (true) {
            auto t1 = get_now_ms();

            // DBUF
            auto video_imgd1 = video.cap_frame_get();
            video_imgd1->syncDeviceToCpu();
            auto video_imgd2 = video.cap_frame_get();
            video_imgd2->syncDeviceToCpu();
            auto video_imgd3 = video.cap_frame_get();
            video_imgd3->syncDeviceToCpu();
            auto video_imgd4 = video.cap_frame_get();
            video_imgd4->syncDeviceToCpu();

            // RGA缩放
            RGA::resizeAndCvtColor(video_imgd1, rga_imgds[0]);
            RGA::resizeAndCvtColor(video_imgd2, rga_imgds[1]);
            RGA::resizeAndCvtColor(video_imgd3, rga_imgds[2]);
            RGA::resizeAndCvtColor(video_imgd4, rga_imgds[3]);

            // RGA拼接更新
            RGA::spliceImgs(rga_imgds, Imgd);

            // frmDbuf->syncDeviceToCpu();
            // memcpy(frmDbuf->getVa(), yuv_data.data() + pos, frmDbuf->getSize());
            // frmDbuf->syncCpuToDevice();
            // pos += frmDbuf->getSize();
            // if (pos >= yuv_data.size()) {
            //     pos = 0;
            // }

            // frmDbuf->syncDeviceToCpu();
            // fl.write((const char*)dbuf->getVa(), dbuf->getSize());

            auto t2 = get_now_ms();

            fps /= 4;
            int64_t t3, t4;
            encoder.encode(Imgd, pktd, (i % fps) == 0 ? 1 : 0, 0,
                           [&rtsp_client, &t3, &t4](const void *ptr, size_t len, RK_U32 is_keyframe, RK_U32 eos) {
                               t3 = get_now_ms();
                               rtsp_client.push_h264_packet((const uint8_t *)ptr, len, is_keyframe);
                               t4 = get_now_ms();
                           });
            // encoder.encode(dbuf, pktDbuf, (i % fps) == 0 ? 1 : 0, i == (target_frames - 1) ? 1 : 0);

            video.cap_frame_put(video_imgd1);
            video.cap_frame_put(video_imgd2);
            video.cap_frame_put(video_imgd3);
            video.cap_frame_put(video_imgd4);

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

    t2.join();
#endif
    return 0;
}