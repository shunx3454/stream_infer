#pragma once
#include "dmabuf.h"

#include <atomic>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>

class ImgDMABuf : public DmaBuf {

  public:
    ImgDMABuf() = default;
    ~ImgDMABuf() = default;

    ImgDMABuf(int width, int height, int w_stride, int h_stride, int pixfmt, int index);

    ImgDMABuf(const ImgDMABuf &) = delete;
    ImgDMABuf& operator=(const ImgDMABuf &) = delete;

    // 移动语义
    ImgDMABuf(ImgDMABuf&& ) noexcept = delete;
    ImgDMABuf& operator=(ImgDMABuf&& ) noexcept = delete;

    unsigned int getIndex();

  private:
    int width_;
    int height_;
    int w_stride_; // bytes
    int h_stride_; // lines
    int img_size_; // bytes
    int v4l2PixFmt_;

    unsigned int index_; // for v4l2 index
    int seq_;   // dq buf sequence
};



class ImgDMABufPool {
public:
    ImgDMABufPool() = delete;
    ImgDMABufPool(size_t n, int width, int height, int w_stride, int h_stride, int pixfmt);

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