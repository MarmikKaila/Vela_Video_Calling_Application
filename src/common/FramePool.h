#pragma once

// FramePool — fixed-capacity, pre-allocated buffer pool.
//
// Solves the "no malloc in the media hot path" requirement. At startup the pool
// allocates `count` buffers of `bufferSize` bytes. acquire() hands out a buffer
// wrapped in a shared_ptr whose deleter returns it to the free list instead of
// freeing it — so the steady-state capture→encode→send loop performs zero heap
// allocation and frees are O(1) list pushes.
//
// acquire() returns nullptr when the pool is exhausted; callers MUST treat that
// as back-pressure (drop the frame), never as a fatal error. Dropping under
// load is the correct real-time behavior — blocking would grow latency.
//
// Thread-safe: producer (capture thread) and consumers (encode/send threads)
// may acquire/release concurrently.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace vc {

class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    // A pooled buffer. Memory is owned by the pool for its whole lifetime; this
    // is just a view that gets recycled. Obtain via FramePool::acquire().
    class Buffer {
    public:
        [[nodiscard]] uint8_t* data() noexcept { return data_; }
        [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    private:
        friend class FramePool;
        Buffer(uint8_t* d, std::size_t cap, std::size_t slot)
            : data_(d), capacity_(cap), slot_(slot) {}
        uint8_t* data_;
        std::size_t capacity_;
        std::size_t slot_;
    };

    static std::shared_ptr<FramePool> create(std::size_t bufferSize, std::size_t count) {
        return std::shared_ptr<FramePool>(new FramePool(bufferSize, count));
    }

    // Returns a recycled buffer, or nullptr if all `count` buffers are in use.
    [[nodiscard]] std::shared_ptr<Buffer> acquire() {
        std::size_t slot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (freeSlots_.empty()) return nullptr;
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        }
        auto self = shared_from_this();
        Buffer* b = new Buffer(storage_.data() + slot * bufferSize_, bufferSize_, slot);
        // Custom deleter recycles the slot rather than freeing pool memory.
        return std::shared_ptr<Buffer>(b, [self](Buffer* buf) {
            self->release(buf->slot_);
            delete buf;
        });
    }

    [[nodiscard]] std::size_t bufferSize() const noexcept { return bufferSize_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return count_; }
    [[nodiscard]] std::size_t available() const {
        std::lock_guard<std::mutex> lk(mu_);
        return freeSlots_.size();
    }

private:
    FramePool(std::size_t bufferSize, std::size_t count)
        : bufferSize_(bufferSize), count_(count), storage_(bufferSize * count) {
        freeSlots_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) freeSlots_.push_back(i);
    }

    void release(std::size_t slot) {
        std::lock_guard<std::mutex> lk(mu_);
        freeSlots_.push_back(slot);
    }

    const std::size_t bufferSize_;
    const std::size_t count_;
    std::vector<uint8_t> storage_;       // single contiguous pre-allocation
    mutable std::mutex mu_;
    std::vector<std::size_t> freeSlots_; // LIFO free list of slot indices
};

} // namespace vc
