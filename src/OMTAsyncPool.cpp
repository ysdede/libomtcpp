/**
 * OMTAsyncPool.cpp - Async Send Buffer Pool Implementation
 * Port of libomtnet/src/OMTSocketAsyncPool.cs
 */

#include "OMTAsyncPool.h"

#include "OMTLogging.h"
namespace omt {

AsyncPool::AsyncPool(int count, size_t bufferSize) {
    pool_.reserve(count);
    for (int i = 0; i < count; i++) {
        pool_.push_back(new AsyncBuffer(bufferSize));
    }
    LOGI("AsyncPool: created %d buffers of %zu bytes", count, bufferSize);
}

AsyncPool::~AsyncPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* buf : pool_) {
        delete buf;
    }
    pool_.clear();
}

AsyncBuffer* AsyncPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* buf : pool_) {
        if (!buf->inUse) {
            buf->inUse = true;
            buf->length = 0;
            return buf;
        }
    }
    return nullptr;  // All buffers in use
}

void AsyncPool::release(AsyncBuffer* buf) {
    if (buf) {
        std::lock_guard<std::mutex> lock(mutex_);
        buf->inUse = false;
        buf->length = 0;
    }
}

int AsyncPool::availableCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto* buf : pool_) {
        if (!buf->inUse) count++;
    }
    return count;
}

} // namespace omt
