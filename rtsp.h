#pragma once

#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

class RtspPusher {
  private:
    AVFormatContext *fmt_ctx = nullptr;
    AVStream *video_stream = nullptr;
    int64_t frame_index = 0;
    int fps = 30;

  public:
    RtspPusher() {}

    ~RtspPusher() {
        if (fmt_ctx) {
            // 写入流尾部标识，关闭 RTSP 会话
            av_write_trailer(fmt_ctx);
            if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&fmt_ctx->pb);
            }
            avformat_free_context(fmt_ctx);
        }
    }

    // 初始化 RTSP 推流
    // rtsp_url 形式如: "rtsp://192.168.1.100:8554/live/mystream"
    int init(const std::string &rtsp_url, int width, int height, int fps_val, const uint8_t *sps_pps_buf = nullptr,
             size_t sps_pps_len = 0);
    int push_h264_packet(const uint8_t *data, size_t size, bool is_keyframe);

};

