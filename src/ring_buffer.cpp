#include "mdfh/ring_buffer.hpp"
#include <stdexcept>

namespace mdfh {

RingBuffer::RingBuffer(std::uint64_t capacity) 
    : capacity_(capacity), mask_(capacity - 1) {
    // Ensure capacity is power of 2
    if ((capacity & (capacity - 1)) != 0) {
        throw std::invalid_argument("Ring buffer capacity must be power of 2");
    }
    slots_.resize(capacity);
}

bool RingBuffer::try_push(const Slot& slot) {
    auto write = write_pos_.load(std::memory_order_relaxed);
    auto read = read_pos_.load(std::memory_order_acquire);
    
    if ((write - read) >= capacity_) {
        return false;  // Buffer full
    }
    
    slots_[write & mask_] = slot;  // Single producer, no need for atomic store
    std::atomic_thread_fence(std::memory_order_release);
    write_pos_.store(write + 1, std::memory_order_release);
    
    // Update high water mark
    auto current_size = (write + 1) - read;
    auto current_hwm = high_water_mark_.load(std::memory_order_relaxed);
    while (current_size > current_hwm && 
           !high_water_mark_.compare_exchange_weak(current_hwm, current_size, std::memory_order_relaxed)) {
        // Retry if another thread updated it
    }
    
    return true;
}

bool RingBuffer::try_pop(Slot& slot) {
    auto read = read_pos_.load(std::memory_order_relaxed);
    auto write = write_pos_.load(std::memory_order_acquire);
    
    if (read == write) {
        return false;  // Buffer empty
    }
    
    // Acquire fence to ensure we see producer's writes to slot before reading
    std::atomic_thread_fence(std::memory_order_acquire);
    slot = slots_[read & mask_];  // Single consumer, no need for atomic load
    read_pos_.store(read + 1, std::memory_order_release);
    return true;
}

std::uint64_t RingBuffer::size() const {
    auto write = write_pos_.load(std::memory_order_acquire);
    auto read = read_pos_.load(std::memory_order_acquire);
    return write - read;
}

std::uint64_t RingBuffer::high_water_mark() const {
    return high_water_mark_.load(std::memory_order_acquire);
}

void RingBuffer::advance_write_pos(std::uint64_t count) {
    auto old_write = write_pos_.load(std::memory_order_relaxed);
    auto new_write = old_write + count;
    write_pos_.store(new_write, std::memory_order_release);
    
    // Update high water mark
    auto read = read_pos_.load(std::memory_order_acquire);
    auto current_size = new_write - read;
    auto current_hwm = high_water_mark_.load(std::memory_order_relaxed);
    while (current_size > current_hwm && 
           !high_water_mark_.compare_exchange_weak(current_hwm, current_size, std::memory_order_relaxed)) {
        // Retry if another thread updated it
    }
}

} // namespace mdfh 