#include "mdfh/ring_buffer.hpp"
#include <stdexcept>
#include <chrono>
#include <thread>
#include <mutex>

// Platform-specific prefetch intrinsics
#if defined(__x86_64__) || defined(__AVX2__)
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

// Unlikely macro for branch prediction
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

namespace mdfh {

// Logger implementation
LogLevel Logger::current_level_ = LogLevel::INFO;
std::ostream* Logger::output_stream_ = &std::cerr;

void Logger::log(LogLevel level, const std::string& component, const std::string& message) {
    if (level < current_level_) {
        return;  // Skip logging if below current level
    }
    
    static std::mutex log_mutex;  // Thread-safe logging
    std::lock_guard<std::mutex> lock(log_mutex);
    
    *output_stream_ << get_timestamp() << " [" << level_to_string(level) << "] " 
                   << component << ": " << message << std::endl;
}

std::string Logger::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

RingBuffer::RingBuffer(std::uint64_t capacity) 
    : capacity_(capacity), mask_(capacity - 1) {
    validate_capacity(capacity);
    slots_.resize(capacity);
    
    MDFH_LOG_DEBUG("RingBuffer", "Created ring buffer with capacity " + std::to_string(capacity));
}

void RingBuffer::validate_capacity(std::uint64_t capacity) const {
    if (capacity == 0) {
        throw std::invalid_argument("Ring buffer capacity cannot be zero");
    }
    if (!is_power_of_two(capacity)) {
        throw std::invalid_argument("Ring buffer capacity must be power of 2, got: " + std::to_string(capacity));
    }
    if (capacity > (1ULL << 32)) {
        throw std::invalid_argument("Ring buffer capacity too large (max 2^32), got: " + std::to_string(capacity));
    }
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
    
    // Update high water mark - gated to reduce contention
    auto current_size = (write + 1) - read;
    auto current_hwm = high_water_mark_.load(std::memory_order_relaxed);
    if (UNLIKELY(current_size > current_hwm)) {
        high_water_mark_.store(current_size, std::memory_order_relaxed);
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
    if (count == 0) return;
    
    auto old_write = write_pos_.load(std::memory_order_relaxed);
    auto new_write = old_write + count;
    
    // Validate that we're not advancing beyond capacity
    auto read = read_pos_.load(std::memory_order_acquire);
    if ((new_write - read) > capacity_) {
        MDFH_LOG_ERROR("RingBuffer", "Cannot advance write position beyond capacity");
        throw std::runtime_error("Cannot advance write position beyond capacity");
    }
    
    write_pos_.store(new_write, std::memory_order_release);
    
    // Update high water mark - gated to reduce contention
    auto current_size = new_write - read;
    auto current_hwm = high_water_mark_.load(std::memory_order_relaxed);
    if (UNLIKELY(current_size > current_hwm)) {
        high_water_mark_.store(current_size, std::memory_order_relaxed);
    }
}

std::uint64_t RingBuffer::try_push_bulk(const Slot* slots, std::uint64_t count) {
    if (count == 0 || slots == nullptr) return 0;
    
    auto write = write_pos_.load(std::memory_order_relaxed);
    auto read = read_pos_.load(std::memory_order_acquire);
    
    auto available_space = capacity_ - (write - read);
    auto to_push = std::min(count, available_space);
    
    if (to_push == 0) {
        return 0;  // Buffer full
    }
    
    // Copy slots in bulk
    for (std::uint64_t i = 0; i < to_push; ++i) {
        slots_[(write + i) & mask_] = slots[i];
    }
    
    std::atomic_thread_fence(std::memory_order_release);
    write_pos_.store(write + to_push, std::memory_order_release);
    
    // Update high water mark - gated to reduce contention
    auto current_size = (write + to_push) - read;
    auto current_hwm = high_water_mark_.load(std::memory_order_relaxed);
    if (UNLIKELY(current_size > current_hwm)) {
        high_water_mark_.store(current_size, std::memory_order_relaxed);
    }
    
    return to_push;
}

std::uint64_t RingBuffer::try_pop_bulk(Slot* slots, std::uint64_t max_count) {
    if (max_count == 0 || slots == nullptr) return 0;
    
    auto read = read_pos_.load(std::memory_order_relaxed);
    auto write = write_pos_.load(std::memory_order_acquire);
    
    auto available_items = write - read;
    auto to_pop = std::min(max_count, available_items);
    
    if (to_pop == 0) {
        return 0;  // Buffer empty
    }
    
    // Acquire fence to ensure we see producer's writes before reading
    std::atomic_thread_fence(std::memory_order_acquire);
    
    // Copy slots in bulk
    for (std::uint64_t i = 0; i < to_pop; ++i) {
        slots[i] = slots_[(read + i) & mask_];
    }
    
    read_pos_.store(read + to_pop, std::memory_order_release);
    return to_pop;
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
    
    // Update high water mark - gated to reduce contention
    auto current_size = (write + 1) - read;
    auto current_hwm = high_water_mark_.load(std::memory_order_relaxed);
    if (UNLIKELY(current_size > current_hwm)) {
        high_water_mark_.store(current_size, std::memory_order_relaxed);
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

std::unique_ptr<RingBuffer> make_ring_buffer(std::uint64_t capacity) {
    return std::make_unique<RingBuffer>(capacity);
}

} // namespace mdfh 