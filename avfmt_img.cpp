#include "avfmt_img.h"

#include <cmath>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

std::string avErr2Str(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

double rationalToDouble(AVRational value) {
    if (value.num <= 0 || value.den <= 0)
        return 0.0;
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

double streamFrameRate(const AVStream *stream, const AVCodecContext *dec) {
    double fps = rationalToDouble(stream->avg_frame_rate);
    if (fps > 0.0)
        return fps;

    fps = rationalToDouble(stream->r_frame_rate);
    if (fps > 0.0)
        return fps;

    return rationalToDouble(dec->framerate);
}

int64_t frameDurationUsFromRate(double fps) {
    if (fps <= 0.0)
        return 0;
    return static_cast<int64_t>(std::llround(1000000.0 / fps));
}

AVPixelFormat v4l2ToAvPixelFormat(int pixfmt) {
    switch (pixfmt) {
    case V4L2_PIX_FMT_RGB24:
        return AV_PIX_FMT_RGB24;
    case V4L2_PIX_FMT_BGR24:
        return AV_PIX_FMT_BGR24;
    case V4L2_PIX_FMT_NV12:
        return AV_PIX_FMT_NV12;
    case V4L2_PIX_FMT_YUYV:
        return AV_PIX_FMT_YUYV422;
    case V4L2_PIX_FMT_UYVY:
        return AV_PIX_FMT_UYVY422;
    default:
        return AV_PIX_FMT_NONE;
    }
}

int bytesPerPixelForPacked(int pixfmt) {
    switch (pixfmt) {
    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_BGR24:
        return 3;
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
        return 2;
    default:
        return 0;
    }
}

const char *hardwareDecoderName(AVCodecID codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return "h264_rkmpp";
    case AV_CODEC_ID_HEVC:
        return "hevc_rkmpp";
    default:
        return nullptr;
    }
}

} // namespace

struct AvFmtImgReader::Impl {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *decoded = nullptr;
    AVFrame *sw = nullptr;
    SwsContext *sws = nullptr;
    int video_stream = -1;

    ~Impl() {
        if (sws)
            sws_freeContext(sws);
        if (sw)
            av_frame_free(&sw);
        if (decoded)
            av_frame_free(&decoded);
        if (pkt)
            av_packet_free(&pkt);
        if (dec)
            avcodec_free_context(&dec);
        if (fmt)
            avformat_close_input(&fmt);
    }
};

uint8_t *AvFmtImgFrame::data() {
    return dmabuf ? static_cast<uint8_t *>(dmabuf->getVa()) : bytes.data();
}

const uint8_t *AvFmtImgFrame::data() const {
    return dmabuf ? static_cast<const uint8_t *>(dmabuf->getVa()) : bytes.data();
}

size_t AvFmtImgFrame::size() const {
    return dmabuf ? dmabuf->getSize() : bytes.size();
}

int AvFmtImgFrame::fd() const {
    return dmabuf ? dmabuf->getFd() : -1;
}

AvFmtImgReader::AvFmtImgReader(const std::string &path, int output_pixfmt, AvFmtDecoderPreference pref, bool loop) {
    open(path, output_pixfmt, pref, loop);
}

AvFmtImgReader::AvFmtImgReader(const std::string &path, bool loop) {
    open(path, V4L2_PIX_FMT_YUYV, AvFmtDecoderPreference::Auto, loop);
}

AvFmtImgReader::~AvFmtImgReader() { close(); }

void AvFmtImgReader::setError(const std::string &msg) { last_error_ = msg; }

void AvFmtImgReader::updateOutputGeometry() {
    out_width_ = src_width_;
    out_height_ = src_height_;

    if (out_pixfmt_ == V4L2_PIX_FMT_NV12) {
        out_stride_ = out_width_;
        output_size_ = static_cast<size_t>(out_stride_) * out_height_ * 3 / 2;
        return;
    }

    int bpp = bytesPerPixelForPacked(out_pixfmt_);
    out_stride_ = bpp ? out_width_ * bpp : 0;
    output_size_ = static_cast<size_t>(out_stride_) * out_height_;
}

bool AvFmtImgReader::open(const std::string &path, int output_pixfmt, AvFmtDecoderPreference pref, bool loop) {
    close();

    path_ = path;
    pref_ = pref;
    loop_ = loop;
    out_pixfmt_ = output_pixfmt;
    eof_ = false;
    using_hw_ = false;
    tried_soft_fallback_ = false;
    frame_rate_ = 0.0;
    frame_duration_us_ = 0;
    next_seq_ = 0;
    decoder_name_.clear();
    last_error_.clear();

    if (v4l2ToAvPixelFormat(out_pixfmt_) == AV_PIX_FMT_NONE) {
        setError("unsupported output V4L2 pixel format");
        return false;
    }

    if (pref_ == AvFmtDecoderPreference::Software)
        return openSoftware();

    if (pref_ == AvFmtDecoderPreference::Hardware)
        return openHardware();

    std::string hw_error;
    if (openHardware())
        return true;

    hw_error = last_error_;
    if (openSoftware())
        return true;

    if (!hw_error.empty())
        last_error_ = hw_error + "; software fallback failed: " + last_error_;
    return false;
}

bool AvFmtImgReader::open(const std::string &path, bool loop) {
    return open(path, V4L2_PIX_FMT_YUYV, AvFmtDecoderPreference::Auto, loop);
}

void AvFmtImgReader::close() {
    impl_.reset();
    eof_ = false;
    loop_ = false;
    using_hw_ = false;
    frame_rate_ = 0.0;
    frame_duration_us_ = 0;
    decoder_name_.clear();
}

bool AvFmtImgReader::openHardware() { return openDecoder(true); }

bool AvFmtImgReader::openSoftware() { return openDecoder(false); }

bool AvFmtImgReader::openDecoder(bool hardware) {
    auto next = std::make_unique<Impl>();

    int ret = avformat_open_input(&next->fmt, path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        setError("avformat_open_input failed: " + avErr2Str(ret));
        return false;
    }

    ret = avformat_find_stream_info(next->fmt, nullptr);
    if (ret < 0) {
        setError("avformat_find_stream_info failed: " + avErr2Str(ret));
        return false;
    }

    ret = av_find_best_stream(next->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        setError("no video stream found: " + avErr2Str(ret));
        return false;
    }
    next->video_stream = ret;

    AVStream *stream = next->fmt->streams[next->video_stream];
    const AVCodec *codec = nullptr;
    if (hardware) {
        if (const char *name = hardwareDecoderName(stream->codecpar->codec_id))
            codec = avcodec_find_decoder_by_name(name);
        if (!codec) {
            setError("rkmpp hardware decoder is not available for this codec");
            return false;
        }
    } else {
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            setError("software decoder is not available for this codec");
            return false;
        }
    }

    next->dec = avcodec_alloc_context3(codec);
    if (!next->dec) {
        setError("avcodec_alloc_context3 failed");
        return false;
    }

    ret = avcodec_parameters_to_context(next->dec, stream->codecpar);
    if (ret < 0) {
        setError("avcodec_parameters_to_context failed: " + avErr2Str(ret));
        return false;
    }

    if (!hardware)
        next->dec->thread_count = 0;

    ret = avcodec_open2(next->dec, codec, nullptr);
    if (ret < 0) {
        setError(std::string("avcodec_open2 failed for ") + codec->name + ": " + avErr2Str(ret));
        return false;
    }

    next->pkt = av_packet_alloc();
    next->decoded = av_frame_alloc();
    next->sw = av_frame_alloc();
    if (!next->pkt || !next->decoded || !next->sw) {
        setError("failed to allocate packet/frame");
        return false;
    }

    src_width_ = next->dec->width;
    src_height_ = next->dec->height;
    frame_rate_ = streamFrameRate(stream, next->dec);
    frame_duration_us_ = frameDurationUsFromRate(frame_rate_);
    updateOutputGeometry();

    impl_ = std::move(next);
    using_hw_ = hardware;
    decoder_name_ = codec->name ? codec->name : "";
    eof_ = false;
    return true;
}

bool AvFmtImgReader::tryFallbackToSoftware() {
    if (!using_hw_ || pref_ == AvFmtDecoderPreference::Hardware || tried_soft_fallback_)
        return false;

    tried_soft_fallback_ = true;
    std::string hw_error = last_error_;
    impl_.reset();
    using_hw_ = false;
    eof_ = false;

    if (!openSoftware()) {
        last_error_ = hw_error + "; software fallback failed: " + last_error_;
        return false;
    }
    return true;
}

void AvFmtImgReader::releaseDecoderFrames() {
    if (!impl_)
        return;
    av_frame_unref(impl_->decoded);
    av_frame_unref(impl_->sw);
}

bool AvFmtImgReader::outputCurrentFrame(void *dst, size_t dst_size, AvFmtImgFrame &frame) {
    if (!dst) {
        setError("destination buffer is null");
        return false;
    }

    AVFrame *src = impl_->decoded;

    if (src->format == AV_PIX_FMT_DRM_PRIME || src->hw_frames_ctx) {
        int ret = av_hwframe_transfer_data(impl_->sw, src, 0);
        if (ret < 0) {
            setError("hardware decoded frame can not be transferred to userspace: " + avErr2Str(ret));
            return false;
        }
        src = impl_->sw;
    }

    AVPixelFormat dst_fmt = v4l2ToAvPixelFormat(out_pixfmt_);
    if (dst_fmt == AV_PIX_FMT_NONE) {
        setError("unsupported output pixel format");
        return false;
    }

    size_t required = output_size_;
    if (dst_size < required) {
        std::ostringstream oss;
        oss << "destination buffer too small: need " << required << " bytes, got " << dst_size;
        setError(oss.str());
        return false;
    }

    impl_->sws = sws_getCachedContext(impl_->sws, src->width, src->height, static_cast<AVPixelFormat>(src->format),
                                      out_width_, out_height_, dst_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws) {
        setError("sws_getCachedContext failed");
        return false;
    }

    uint8_t *dst_data[4] = {};
    int dst_linesize[4] = {};
    int ret = av_image_fill_arrays(dst_data, dst_linesize, static_cast<uint8_t *>(dst), dst_fmt, out_width_, out_height_,
                                   1);
    if (ret < 0) {
        setError("av_image_fill_arrays failed: " + avErr2Str(ret));
        return false;
    }

    ret = sws_scale(impl_->sws, src->data, src->linesize, 0, src->height, dst_data, dst_linesize);
    if (ret <= 0) {
        setError("sws_scale failed");
        return false;
    }

    frame.width = out_width_;
    frame.height = out_height_;
    frame.stride = out_stride_;
    frame.pixfmt = out_pixfmt_;
    frame.pts = src->pts;
    return true;
}

bool AvFmtImgReader::validateDmaBuffer(const std::shared_ptr<ImgDMABuf> &imgd) {
    if (!imgd) {
        setError("ImgDMABuf is null");
        return false;
    }

    if (imgd->img_get_width() != out_width_ || imgd->img_get_height() != out_height_ ||
        imgd->img_get_fmt() != out_pixfmt_) {
        setError("ImgDMABuf geometry or pixel format does not match decoder output");
        return false;
    }

    if (imgd->getSize() < output_size_) {
        std::ostringstream oss;
        oss << "ImgDMABuf is too small for decoder output: need " << output_size_ << " bytes, got " << imgd->getSize();
        setError(oss.str());
        return false;
    }

    return true;
}

bool AvFmtImgReader::rewindToStart() {
    if (!impl_ || impl_->video_stream < 0)
        return false;

    AVStream *stream = impl_->fmt->streams[impl_->video_stream];
    int64_t timestamp = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    int ret = av_seek_frame(impl_->fmt, impl_->video_stream, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        setError("av_seek_frame(loop) failed: " + avErr2Str(ret));
        return false;
    }

    avcodec_flush_buffers(impl_->dec);
    av_packet_unref(impl_->pkt);
    releaseDecoderFrames();
    eof_ = false;
    return true;
}

bool AvFmtImgReader::readDecodedFrame() {
    releaseDecoderFrames();
    bool rewind_done = false;

    for (;;) {
        int ret = avcodec_receive_frame(impl_->dec, impl_->decoded);
        if (ret == 0) {
            if (!src_width_ || !src_height_ || src_width_ != impl_->decoded->width || src_height_ != impl_->decoded->height) {
                src_width_ = impl_->decoded->width;
                src_height_ = impl_->decoded->height;
                updateOutputGeometry();
            }

            return true;
        }

        if (ret == AVERROR_EOF) {
            if (loop_ && !rewind_done) {
                rewind_done = true;
                if (rewindToStart())
                    continue;
            }
            eof_ = true;
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            setError("avcodec_receive_frame failed: " + avErr2Str(ret));
            return false;
        }

        ret = av_read_frame(impl_->fmt, impl_->pkt);
        if (ret == AVERROR_EOF) {
            ret = avcodec_send_packet(impl_->dec, nullptr);
            if (ret < 0) {
                setError("avcodec_send_packet(flush) failed: " + avErr2Str(ret));
                return false;
            }
            continue;
        }
        if (ret < 0) {
            setError("av_read_frame failed: " + avErr2Str(ret));
            return false;
        }

        if (impl_->pkt->stream_index == impl_->video_stream) {
            ret = avcodec_send_packet(impl_->dec, impl_->pkt);
            av_packet_unref(impl_->pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                setError("avcodec_send_packet failed: " + avErr2Str(ret));
                return false;
            }
        } else {
            av_packet_unref(impl_->pkt);
        }
    }
}

bool AvFmtImgReader::read(AvFmtImgFrame &frame) {
    if (!impl_) {
        setError("reader is not open");
        return false;
    }

    for (;;) {
        if (!readDecodedFrame())
            return false;

        frame = {};
        frame.pixfmt = out_pixfmt_;
        frame.bytes.resize(output_size_);

        bool ok = outputCurrentFrame(frame.bytes.data(), frame.bytes.size(), frame);
        releaseDecoderFrames();

        if (ok)
            return true;

        frame.bytes.clear();
        if (!tryFallbackToSoftware())
            return false;
    }
}

bool AvFmtImgReader::read(std::shared_ptr<ImgDMABuf> imgd, AvFmtImgFrame &frame) {
    if (!imgd) {
        setError("ImgDMABuf is null");
        return false;
    }

    if (!impl_) {
        setError("reader is not open");
        return false;
    }

    for (;;) {
        if (!readDecodedFrame())
            return false;

        if (!validateDmaBuffer(imgd)) {
            releaseDecoderFrames();
            return false;
        }

        AvFmtImgFrame out;
        out.pixfmt = out_pixfmt_;
        bool ok = outputCurrentFrame(imgd->getVa(), imgd->getSize(), out);
        releaseDecoderFrames();

        if (ok) {
            imgd->syncCpuToDevice();
            imgd->img_set_seq(next_seq_++);
            out.dmabuf = std::move(imgd);
            frame = std::move(out);
            return true;
        }

        if (!tryFallbackToSoftware())
            return false;
    }
}
