#pragma once

#include "../feed_sim/sim.hpp"  // For Msg struct
#include <boost/asio.hpp>
#include <chrono>
#include <vector>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <ostream>

namespace mdfh {

// Configuration structure for the benchmark client
struct BenchCfg {
    // --- Network ---
    std::string   host = "127.0.0.1";   // TCP server host
    std::uint16_t port = 9001;           // TCP server port
    
    // --- Exit criteria ---
    std::uint32_t max_seconds = 0;       // Run for specified seconds (0 = infinite)
    std::uint64_t max_messages = 0;      // Process max messages then exit (0 = infinite)
    
    // --- Ring buffer ---
    std::uint32_t buf_capacity = 65536;  // Ring buffer capacity (power of 2)
};

// Output stream operator for cleaner configuration display
inline std::ostream& operator<<(std::ostream& os, const BenchCfg& cfg) {
    os << "Configuration:\n";
    os << "  Host: " << cfg.host << "\n";
    os << "  Port: " << cfg.port << "\n";
    os << "  Buffer capacity: " << cfg.buf_capacity << "\n";
    if (cfg.max_seconds > 0) {
        os << "  Max seconds: " << cfg.max_seconds << "\n";
    }
    if (cfg.max_messages > 0) {
        os << "  Max messages: " << cfg.max_messages << "\n";
    }
    return os;
}

// Compile-time validation of Msg struct size
static_assert(sizeof(Msg) == 20, "Msg struct size must be exactly 20 bytes");

// Slot structure for ring buffer entries
struct Slot {
    Msg raw;                    // The parsed message
    std::uint64_t rx_ts;        // Receive timestamp in nanoseconds
};

// Simple single-producer/single-consumer lock-free ring buffer
class RingBuffer {
private:
    std::vector<Slot> slots_;
    std::atomic<std::uint64_t> write_pos_{0};
    std::atomic<std::uint64_t> read_pos_{0};
    std::uint64_t capacity_;
    std::uint64_t mask_;

public:
    explicit RingBuffer(std::uint64_t capacity);
    
    bool try_push(const Slot& slot);
    bool try_pop(Slot& slot);
    std::uint64_t size() const;
};

// Main benchmark ingestion class
class BenchIngest {
private:
    BenchCfg cfg_;
    RingBuffer ring_;
    std::atomic<bool> should_stop_{false};
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> messages_dropped_{0};
    
    // Gap tracking
    std::uint64_t expected_seq_ = 0;  // Will be set from first message
    std::uint64_t gap_count_ = 0;
    bool first_message_seen_ = false;
    
    // Latency histogram (0-999 microseconds, 1µs buckets + overflow bucket)
    std::array<std::uint64_t, 1001> latency_buckets_{};  // [0-999] + [1000+]
    
    // Timing
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_flush_;

public:
    explicit BenchIngest(BenchCfg cfg);
    
    // Main entry point
    void run();

private:
    // Thread workers
    void io_worker();
    void consumer_loop();
    
    // Message processing
    void process_bytes(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& partial_msg);
    void process_message(const Slot& slot);
    
    // Statistics and reporting
    void check_periodic_flush();
    void print_periodic_stats();
    void print_final_stats();
    std::uint64_t calculate_percentile(double percentile, std::uint64_t total_samples);
};

} // namespace mdfh 