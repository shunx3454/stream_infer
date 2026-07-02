#include "img_dmabuf.h"

ImgDMABuf::ImgDMABuf(size_t n, int index) {}

ImgDMABuf::ImgDMABuf(int width, int height, int w_stride, int h_stride, int pixfmt)
    : DmaBuf(w_stride * h_stride), width_(width), height_(height), w_stride_(w_stride), h_stride_(h_stride),
      v4l2PixFmt_(pixfmt), img_size_(w_stride * h_stride), index_(0), seq_(0), pkt_len_(0), pkt_ptr_(NULL), eos(0) {}

void ImgDMABuf::pkt_set_ptr(void *ptr) { pkt_ptr_ = ptr; }

void ImgDMABuf::pkt_set_len(size_t len) { pkt_len_ = len; }

void ImgDMABuf::img_set_fps(int fps)
{
    fps_ = fps;
}

int ImgDMABuf::img_get_fps() { return fps_; }

int ImgDMABuf::img_get_fmt() { return v4l2PixFmt_; }

int ImgDMABuf::img_get_height() { return height_; }

int ImgDMABuf::img_get_width() { return width_; }

void ImgDMABuf::img_set_stream_id(int id) { stream_id_ = id; }

int ImgDMABuf::img_get_stream_id() { return stream_id_; }

void ImgDMABuf::img_set_seq(int seq) { seq_ = seq; }

int ImgDMABuf::img_get_seq() { return seq_; }

unsigned int  ImgDMABuf::img_get_index() { return index_; }

void ImgDMABuf::img_set_index(unsigned int  index) { index_ = index; }

void ImgDMABuf::img_set_user_data(void *data) {
    data_ = data;
}

void *ImgDMABuf::img_get_user_data() { return data_; }

ImgDMABufPool::ImgDMABufPool(size_t n) {
    capacity_ = n;
    for (int i = 0; i < n; ++i) {
        pool_queue_.push(std::make_shared<ImgDMABuf>());
    }
}

ImgDMABufPool::ImgDMABufPool(size_t n, unsigned int width, unsigned int height, unsigned int w_stride,
                             unsigned int h_stride, unsigned int pixfmt) {
    capacity_ = n;
    for (int i = 0; i < n; ++i) {
        pool_queue_.push(std::make_shared<ImgDMABuf>(width, height, w_stride, h_stride, pixfmt));
    }
}

std::shared_ptr<ImgDMABuf> ImgDMABufPool::get(std::atomic<bool> &exited) {
    std::shared_ptr<ImgDMABuf> buf;
    {
        std::unique_lock lock(mtx_);

        while (!exited.load() && pool_queue_.empty()) {
            cv_get_.wait(lock);
        }

        if (exited.load()) {
            return {};
        }

        buf = pool_queue_.front();
        pool_queue_.pop();
        --capacity_;
    }

    cv_put_.notify_one();

    return buf;
}

void ImgDMABufPool::put(std::shared_ptr<ImgDMABuf> pb, std::atomic<bool> &exited) {

    unsigned int index = pb->img_get_index();
    {
        std::unique_lock lock(mtx_);

        while (!exited.load() && pool_queue_.size() == capacity_) {
            cv_put_.wait(lock);
        }

        if (exited.load())
            return;

        pool_queue_.push(std::move(pb));
        ++capacity_;
    }

    cv_get_.notify_one();
}

void ImgDMABufPool::wakeup() {
    cv_put_.notify_all();
    cv_get_.notify_all();
}
