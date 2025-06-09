#include "mdfh/ring_buffer.hpp"
#include <stdexcept>
#include <chrono>
#include <thread>

// Platform-specific prefetch intrinsics
#ifdef __x86_64__
#include <xmmintrin.h>  // For _mm_prefetch
#define PREFETCH_READ(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#define PREFETCH_WRITE(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#elif defined(__aarch64__)
#define PREFETCH_READ(addr) __builtin_prefetch(addr, 0, 3)
#define PREFETCH_WRITE(addr) __builtin_prefetch(addr, 1, 3)
#else
#define PREFETCH_READ(addr) ((void)0)
#define PREFETCH_WRITE(addr) ((void)0)
#endif

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

bool RingBuffer::try_push_with_prefetch(const Slot& slot) {
    auto write = write_pos_.load(std::memory_order_relaxed);
    auto read = read_pos_.load(std::memory_order_acquire);
    
    if ((write - read) >= capacity_) {
        return false;  // Buffer full
    }
    
    // Prefetch the next slot we'll likely write to
    auto next_write_idx = (write + 1) & mask_;
    PREFETCH_WRITE(&slots_[next_write_idx]);
    
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

bool RingBuffer::try_pop_with_prefetch(Slot& slot) {
    auto read = read_pos_.load(std::memory_order_relaxed);
    auto write = write_pos_.load(std::memory_order_acquire);
    
    if (read == write) {
        return false;  // Buffer empty
    }
    
    // Prefetch the next slot we'll likely read
    auto next_read_idx = (read + 1) & mask_;
    PREFETCH_READ(&slots_[next_read_idx]);
    
    // Acquire fence to ensure we see producer's writes to slot before reading
    std::atomic_thread_fence(std::memory_order_acquire);
    slot = slots_[read & mask_];  // Single consumer, no need for atomic load
    read_pos_.store(read + 1, std::memory_order_release);
    return true;
}

bool RingBuffer::try_push_or_block(const Slot& slot, std::uint64_t timeout_ns, BackPressureMode mode) {
    if (mode == BackPressureMode::DROP) {
        return try_push_with_prefetch(slot);
    }
    
    // BLOCK mode - wait for space to become available
    auto start_time = std::chrono::high_resolution_clock::now();
    
    while (true) {
        if (try_push_with_prefetch(slot)) {
            return true;
        }
        
        // Check timeout
        if (timeout_ns > 0) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time).count();
            if (static_cast<std::uint64_t>(elapsed_ns) >= timeout_ns) {
                return false; // Timeout exceeded
            }
        }
        
        // Brief yield to avoid busy spinning
        std::this_thread::yield();
    }
}

} // namespace mdfh 