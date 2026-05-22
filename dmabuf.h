#pragma once 
#include "dma_alloc.h"


class DmaBuf {
public:
    // 1. 默认构造（构造一个空的、无效的全局对象）
    DmaBuf();

    // 2. 核心构造：根据路径和大小自动分配 DMA 内存
    DmaBuf(size_t size, const char* heap_path = "/dev/dma_heap/cma");

    // 3. 析构函数：RAII 核心，自动释放内存
    ~DmaBuf();

    // 4. 禁止拷贝（防止多个对象管理同一个 fd 导致二次释放）
    DmaBuf(const DmaBuf&) = delete;
    DmaBuf& operator=(const DmaBuf&) = delete;

    // 5. 允许移动语义（支持在容器中传递或作为函数返回值）
    DmaBuf(DmaBuf&& other) noexcept;
    DmaBuf& operator=(DmaBuf&& other) noexcept;

    // 6. 核心功能接口
    bool isValid() const { return fd_ >= 0 && va_ != nullptr; }
    int getFd() const { return fd_; }
    void* getVa() const { return va_; }
    size_t getSize() const { return size_; }

    // 7. 缓存同步接口（封装 C 接口）
    int syncDeviceToCpu() const;
    int syncCpuToDevice() const;

    // 8. 手动释放接口
    void free();

private:
    int fd_ = -1;
    void* va_ = nullptr;
    size_t size_ = 0;
};