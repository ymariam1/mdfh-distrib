#pragma once

#include "core.hpp"
#include <vector>
#include <atomic>
#include <stdexcept>

namespace mdfh {

// Slot structure for ring buffer entries
struct Slot {
    Msg raw;                    // The parsed message
    std::uint64_t rx_ts;        // Receive timestamp in nanoseconds
};

// High-performance single-producer/single-consumer lock-free ring buffer
// Optimized for low-latency market data ingestion
class RingBuffer {
private:
    std::vector<Slot> slots_;
    std::atomic<std::uint64_t> write_pos_{0};
    std::atomic<std::uint64_t> read_pos_{0};
    std::atomic<std::uint64_t> high_water_mark_{0};
    std::uint64_t capacity_;
    std::uint64_t mask_;

public:
    explicit RingBuffer(std::uint64_t capacity);
    
    // Thread-safe operations
    bool try_push(const Slot& slot);
    bool try_pop(Slot& slot);
    
    // Statistics and monitoring
    std::uint64_t size() const;
    std::uint64_t high_water_mark() const;
    std::uint64_t capacity() const { return capacity_; }
    
    // Zero-copy access for high-performance scenarios
    // WARNING: These methods are not thread-safe and should only be used
    // by the producer thread with careful synchronization
    Slot* slots() { return slots_.data(); }
    std::uint64_t write_pos() const { return write_pos_.load(std::memory_order_relaxed); }
    std::uint64_t read_pos() const { return read_pos_.load(std::memory_order_relaxed); }
    std::uint64_t mask() const { return mask_; }
    void advance_write_pos(std::uint64_t count);
};

} // namespace mdfh 