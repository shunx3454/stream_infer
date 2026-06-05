#pragma once
#include "dmabuf.h"

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
    ImgDMABuf(int width, int height, int w_stride, int h_stride, int pixfmt, int index);

    ImgDMABuf(const ImgDMABuf &) = delete;
    ImgDMABuf &operator=(const ImgDMABuf &) = delete;

    // 移动语义
    ImgDMABuf(ImgDMABuf &&) noexcept = delete;
    ImgDMABuf &operator=(ImgDMABuf &&) noexcept = delete;

    unsigned int getIndex();
    void pkt_set_ptr(void *ptr);
    void pkt_set_len(size_t len);

  private:
    int width_;
    int height_;
    int w_stride_; // bytes
    int h_stride_; // lines
    int img_size_; // bytes
    int v4l2PixFmt_;

    // pkt domain
    size_t pkt_len_;
    void *pkt_ptr_;
    int eos;

    unsigned int index_; // for v4l2 index
    int seq_;            // dq buf sequence
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
    std::vector<std::shared_ptr<ImgDMABuf>> pool_;
    std::queue<unsigned int> index_queue_;
};