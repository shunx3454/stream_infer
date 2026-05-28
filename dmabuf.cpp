#include "dmabuf.h"
#include <stdexcept>
#include <utility> // std::swap

DmaBuf::DmaBuf() : fd_(-1), va_(nullptr), size_(0) {}

DmaBuf::DmaBuf(size_t size, const char *heap_path) {
    if (size == 0) {
        throw std::invalid_argument("DmaBuf size cannot be 0");
    }

    int ret = dma_buf_alloc(heap_path, size, &fd_, &va_);
    if (ret < 0 || fd_ < 0 || va_ == nullptr) {
        throw std::runtime_error("Failed to allocate DMA buffer");
    }
    size_ = size;
}

DmaBuf::~DmaBuf() { free(); }

// 移动构造函数：接管另一个对象的资源
DmaBuf::DmaBuf(DmaBuf &&other) noexcept {
    fd_ = other.fd_;
    va_ = other.va_;
    size_ = other.size_;

    // 将原对象内部状态清空，防止其析构时释放资源
    other.fd_ = -1;
    other.va_ = nullptr;
    other.size_ = 0;
}

// 移动赋值运算符
DmaBuf &DmaBuf::operator=(DmaBuf &&other) noexcept {
    if (this != &other) {
        free(); // 先释放自己原本持有的资源

        fd_ = other.fd_;
        va_ = other.va_;
        size_ = other.size_;

        other.fd_ = -1;
        other.va_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void DmaBuf::free() {
    if (isValid()) {
        dma_buf_free(size_, fd_, va_);
        fd_ = -1;
        va_ = nullptr;
        size_ = 0;
    }
}

int DmaBuf::syncDeviceToCpu() const {
    if (!isValid())
        return -1;
    return dma_sync_device_to_cpu(fd_);
}

int DmaBuf::syncCpuToDevice() const {
    if (!isValid())
        return -1;
    return dma_sync_cpu_to_device(fd_);
}