#pragma once
#include "dmabuf.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

class ImgDMABuf : public DmaBuf {

  public:
    ImgDMABuf() = default;
    ~ImgDMABuf() = default;

    ImgDMABuf(size_t n, int index);
    ImgDMABuf(int width, int height, int w_stride, int h_stride, int pixfmt);

    ImgDMABuf(const ImgDMABuf &) = delete;
    ImgDMABuf &operator=(const ImgDMABuf &) = delete;

    // 移动语义
    ImgDMABuf(ImgDMABuf &&) noexcept = delete;
    ImgDMABuf &operator=(ImgDMABuf &&) noexcept = delete;

    // function
    void pkt_set_ptr(void *ptr);
    void pkt_set_len(size_t len);

    void img_set_fps(int fps);
    int img_get_fps();

    int img_get_fmt();
    int img_get_height();
    int img_get_width();

    void img_set_stream_id(int id);
    int img_get_stream_id();

    void img_set_seq(int seq);
    int img_get_seq();

    unsigned int img_get_index();
    void img_set_index(unsigned int index);

    void img_set_user_data(void *data);
    void *img_get_user_data();

  private:
    int width_;
    int height_;
    int w_stride_; // bytes
    int h_stride_; // lines
    int img_size_; // bytes
    int v4l2PixFmt_;
    int fps_;

    // pkt domain
    size_t pkt_len_;
    void *pkt_ptr_;
    int eos;

    unsigned int index_; // for v4l2 index
    int stream_id_;      // stream id
    int seq_;            // frame seq

    // user data
    void *data_;
};

class ImgDMABufPool {
  public:
    ImgDMABufPool() = delete;
    ImgDMABufPool(size_t n);
    ImgDMABufPool(size_t n, unsigned int width, unsigned int height, unsigned int w_stride, unsigned int h_stride,
                  unsigned int pixfmt);

    std::shared_ptr<ImgDMABuf> get(std::atomic<bool> &);
    void put(std::shared_ptr<ImgDMABuf>, std::atomic<bool> &);
    void wakeup();

  private:
    std::mutex mtx_;
    std::condition_variable cv_put_;
    std::condition_variable cv_get_;
    size_t capacity_;
    std::queue<std::shared_ptr<ImgDMABuf>> pool_queue_;
};
