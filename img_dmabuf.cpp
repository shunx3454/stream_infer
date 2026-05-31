#include "img_dmabuf.h"

ImgDMABuf::ImgDMABuf(int width, int height, int w_stride, int h_stride, int pixfmt, int index)
    : DmaBuf(w_stride * h_stride), width_(width), height_(height), w_stride_(w_stride), h_stride_(h_stride),
      v4l2PixFmt_(pixfmt), img_size_(w_stride * h_stride), index_(index), seq_(0) {}

unsigned int ImgDMABuf::getIndex() { return index_; }

ImgDMABufPool::ImgDMABufPool(size_t n, int width, int height, int w_stride, int h_stride, int pixfmt) {
    pool_.reserve(n);
    for (int i = 0; i < n; ++i) {
        pool_.push_back(std::make_shared<ImgDMABuf>(width, height, w_stride, h_stride, pixfmt, i));
        index_queue_.push(i);
    }
}

std::shared_ptr<ImgDMABuf> ImgDMABufPool::get(std::atomic<bool> &exited) {
    unsigned int index = 0;
    {
        std::unique_lock lock(mtx_);

        while (!exited.load() && index_queue_.empty()) {
            cv_get_.wait(lock);
        }

        if (exited.load()) {
            return {};
        }

        index = index_queue_.front();
        index_queue_.pop();
    }

    cv_put_.notify_one();

    return std::move(pool_[index]);
}

void ImgDMABufPool::put(std::shared_ptr<ImgDMABuf> pb, std::atomic<bool> &exited) {
    
    unsigned int index = pb->getIndex();
    {
        std::unique_lock lock(mtx_);

        while (!exited.load() && index_queue_.size() == pool_.size()) {
            cv_put_.wait(lock);
        }

        if (exited.load())
            return;

        pool_[index] = std::move(pb);
        index_queue_.push(index);
    }

    cv_get_.notify_one();
}

void ImgDMABufPool::wakeup() {
    cv_put_.notify_all();
    cv_get_.notify_all();
}
