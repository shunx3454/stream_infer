#include "rtsp.h"

int RtspPusher::init(const std::string &rtsp_url, int width, int height, int fps_val,
                     const uint8_t *sps_pps_buf, size_t sps_pps_len) {
    this->fps = fps_val;

    // 1. 创建输出上下文，指定格式为 "rtsp"
    int ret = avformat_alloc_output_context2(&fmt_ctx, nullptr, "rtsp", rtsp_url.c_str());
    if (ret < 0 || !fmt_ctx) {
        std::cerr << "Could not create RTSP output context" << std::endl;
        return -1;
    }

    // 2. 创建视频流
    video_stream = avformat_new_stream(fmt_ctx, nullptr);
    if (!video_stream) {
        std::cerr << "Failed to create new stream" << std::endl;
        return -1;
    }

    // 3. 配置流的编码参数
    AVCodecParameters *codecpar = video_stream->codecpar;
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = AV_CODEC_ID_H264; // H264 编码
    codecpar->width = width;
    codecpar->height = height;
    codecpar->format = AV_PIX_FMT_YUV420P;

    // 关键点：如果你的 MPP 编码器能直接提取出纯粹的 SPS/PPS（通常包含在第一帧前面）
    // 最好塞给 extradata。这样 RTSP 握手时的 SDP 信息就会包含正确的配置。
    if (sps_pps_buf && sps_pps_len > 0) {
        codecpar->extradata = (uint8_t *)av_mallocz(sps_pps_len + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(codecpar->extradata, sps_pps_buf, sps_pps_len);
        codecpar->extradata_size = sps_pps_len;
    }

    // 配置时间基准：RTSP 内部通常也会转换为 90kHz 的 RTP 时间戳进行传输
    video_stream->time_base = {1, 90000};

    // 4. 配置 RTSP 专用的参数选项
    AVDictionary *options = nullptr;

    // 【核心优化】默认情况下 RTSP 走 UDP 传输，容易在高分辨率下丢包导致花屏。
    // 推荐强制走 TCP 传输（即 RTSP Interleaved 模式）
    av_dict_set(&options, "rtsp_transport", "tcp", 0);

    // 设置握手超时时间（单位：微秒，这里设为 5 秒）
    av_dict_set(&options, "stimeout", "5000000", 0);

    // 5. 连接 RTSP 服务器并写入头部（内部包含完整的 RTSP 状态机握手）
    // 注意：RTSP 协议内部会自己处理网络链接，通常不需要手动调用 avio_open
    ret = avformat_write_header(fmt_ctx, &options);
    av_dict_free(&options); // 释放参数字典

    if (ret < 0) {
        std::cerr << "Error occurred when connecting to RTSP server or writing header" << std::endl;
        return -1;
    }

    std::cout << "Successfully connected to RTSP server: " << rtsp_url << std::endl;
    return 0;
}

// 推送一帧由 MPP 编码出来的 H.264 NALU 数据
int RtspPusher::push_h264_packet(const uint8_t *data, size_t size, bool is_keyframe) {
    if (!fmt_ctx || !video_stream)
        return -1;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return -1;

    // 分配并拷贝内存
    if (av_new_packet(pkt, size) < 0) {
        av_packet_free(&pkt);
        return -1;
    }
    memcpy(pkt->data, data, size);

    pkt->stream_index = video_stream->index;
    if (is_keyframe) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    // 计算时间戳 (以 90000Hz 时钟基准计算步进)
    int64_t calc_duration = 90000 / fps;
    pkt->pts = frame_index * calc_duration;
    pkt->dts = pkt->pts;
    pkt->duration = calc_duration;

    // 写入网络流
    int ret = av_interleaved_write_frame(fmt_ctx, pkt);
    if (ret < 0) {
        std::cerr << "Error writing frame to RTSP stream" << std::endl;
    }

    frame_index++;
    av_packet_free(&pkt);
    return ret;
}