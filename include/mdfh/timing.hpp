#pragma once

#include <cstdint>
#include <chrono>

// Platform-specific high-resolution timing
#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

namespace mdfh {

// High-performance timestamp function using platform-specific optimizations
inline std::uint64_t get_timestamp_ns() {
#if defined(__linux__) || defined(__APPLE__)
    struct timespec ts;
    #ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    #else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    #endif
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + 
           static_cast<std::uint64_t>(ts.tv_nsec);
#else
    // Fallback to steady_clock on other platforms
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
#endif
}

// Rate limiter for controlling message throughput
class RateLimiter {
private:
    std::chrono::steady_clock::time_point next_tick_;
    std::chrono::nanoseconds interval_;
    
public:
    explicit RateLimiter(std::uint32_t rate_per_second, std::uint32_t batch_size = 1) 
        : next_tick_(std::chrono::steady_clock::now())
        , interval_(std::chrono::nanoseconds(static_cast<long long>(1e9 * batch_size / rate_per_second))) {}
    
    // Wait until it's time for the next batch
    void wait_for_next_tick() {
        auto now = std::chrono::steady_clock::now();
        
        // If we're behind, catch up by advancing to the next aligned slot
        while (next_tick_ <= now) {
            next_tick_ += interval_;
        }
        
        // Busy-spin until it's time (for maximum precision)
        while (std::chrono::steady_clock::now() < next_tick_) { 
            // Intentionally empty - busy wait for precision
        }
    }
};

// Simple timer for measuring elapsed time
class Timer {
private:
    std::chrono::steady_clock::time_point start_;
    
public:
    Timer() : start_(std::chrono::steady_clock::now()) {}
    
    void reset() { start_ = std::chrono::steady_clock::now(); }
    
    std::chrono::milliseconds elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_);
    }
    
    std::chrono::seconds elapsed_s() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_);
    }
    
    double elapsed_seconds() const {
        auto elapsed = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration<double>(elapsed).count();
    }
};

} // namespace mdfh 