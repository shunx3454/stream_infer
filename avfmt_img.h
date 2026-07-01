#pragma once

#include "img_dmabuf.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <linux/videodev2.h>

enum class AvFmtDecoderPreference {
    Auto,
    Hardware,
    Software,
};

struct AvFmtImgFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    int pixfmt = V4L2_PIX_FMT_RGB24;
    int64_t pts = 0;

    std::vector<uint8_t> bytes;
    std::shared_ptr<ImgDMABuf> dmabuf;

    bool isDma() const { return dmabuf != nullptr; }
    uint8_t *data();
    const uint8_t *data() const;
    size_t size() const;
    int fd() const;
};

class AvFmtImgReader {
  public:
    AvFmtImgReader() = default;
    AvFmtImgReader(const std::string &path, bool loop);
    AvFmtImgReader(const std::string &path, int output_pixfmt = V4L2_PIX_FMT_YUYV,
                   AvFmtDecoderPreference pref = AvFmtDecoderPreference::Auto, bool loop = false);
    ~AvFmtImgReader();

    AvFmtImgReader(const AvFmtImgReader &) = delete;
    AvFmtImgReader &operator=(const AvFmtImgReader &) = delete;

    bool open(const std::string &path, int output_pixfmt = V4L2_PIX_FMT_YUYV,
              AvFmtDecoderPreference pref = AvFmtDecoderPreference::Auto, bool loop = false);
    bool open(const std::string &path, bool loop);
    void close();

    bool read(AvFmtImgFrame &frame);
    bool read(std::shared_ptr<ImgDMABuf> imgd, AvFmtImgFrame &frame);

    bool eof() const { return eof_; }
    bool usingHardwareDecoder() const { return using_hw_; }
    const std::string &decoderName() const { return decoder_name_; }
    const std::string &lastError() const { return last_error_; }

    int sourceWidth() const { return src_width_; }
    int sourceHeight() const { return src_height_; }
    int outputWidth() const { return out_width_; }
    int outputHeight() const { return out_height_; }
    int outputStride() const { return out_stride_; }
    int outputPixfmt() const { return out_pixfmt_; }
    size_t outputSize() const { return output_size_; }
    double frameRate() const { return frame_rate_; }
    int64_t frameDurationUs() const { return frame_duration_us_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool openHardware();
    bool openSoftware();
    bool openDecoder(bool hardware);
    bool tryFallbackToSoftware();
    bool readDecodedFrame();
    bool outputCurrentFrame(void *dst, size_t dst_size, AvFmtImgFrame &frame);
    bool validateDmaBuffer(const std::shared_ptr<ImgDMABuf> &imgd);
    bool rewindToStart();
    void releaseDecoderFrames();
    void setError(const std::string &msg);
    void updateOutputGeometry();

    std::string path_;
    AvFmtDecoderPreference pref_ = AvFmtDecoderPreference::Auto;
    bool eof_ = false;
    bool loop_ = false;
    bool using_hw_ = false;
    bool tried_soft_fallback_ = false;
    std::string decoder_name_;
    std::string last_error_;

    int src_width_ = 0;
    int src_height_ = 0;
    int out_width_ = 0;
    int out_height_ = 0;
    int out_stride_ = 0;
    int out_pixfmt_ = V4L2_PIX_FMT_RGB24;
    size_t output_size_ = 0;
    double frame_rate_ = 0.0;
    int64_t frame_duration_us_ = 0;
    int next_seq_ = 0;
};
