#pragma once

#include "core.hpp"
#include "ring_buffer.hpp"
#include "timing.hpp"
#include <boost/asio.hpp>
#include <string>
#include <atomic>
#include <array>
#include <thread>
#include <memory>

namespace mdfh {

// Configuration for the benchmark ingestion client
struct IngestionConfig {
    // Network settings
    std::string host = "127.0.0.1";
    std::uint16_t port = 9001;
    
    // Performance settings
    std::uint32_t buffer_capacity = 65536;  // Ring buffer capacity (power of 2)
    
    // Exit criteria
    std::uint32_t max_seconds = 0;          // Run duration (0 = infinite)
    std::uint64_t max_messages = 0;         // Message limit (0 = infinite)
};

// Output stream operator for configuration display
std::ostream& operator<<(std::ostream& os, const IngestionConfig& cfg);

// Statistics collector for ingestion performance
class IngestionStats {
private:
    // Core counters
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> messages_processed_{0};
    std::atomic<std::uint64_t> messages_dropped_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    
    // Sequence tracking
    std::atomic<bool> first_message_seen_{false};
    std::uint64_t expected_seq_{0};
    std::uint64_t gap_count_{0};
    
    // Timing
    Timer timer_;
    std::chrono::steady_clock::time_point last_flush_;
    
    // Latency histogram (microseconds)
    std::array<std::uint64_t, 1001> latency_buckets_{};  // 0-999µs + overflow
    
    // For rate calculation - track deltas
    std::uint64_t last_messages_received_{0};
    std::uint64_t last_messages_processed_{0};
    std::uint64_t last_bytes_received_{0};
    double last_elapsed_seconds_{0.0};
    
public:
    IngestionStats();
    virtual ~IngestionStats() = default;
    
    // Record events
    virtual void record_bytes_received(std::uint64_t bytes);
    virtual void record_message_received();
    virtual void record_message_processed(const Slot& slot);
    virtual void record_message_dropped();
    
    // Periodic reporting
    virtual void check_periodic_flush();
    virtual void print_final_stats();
    
    // Getters
    std::uint64_t messages_received() const { return messages_received_.load(); }
    std::uint64_t messages_processed() const { return messages_processed_.load(); }
    std::uint64_t messages_dropped() const { return messages_dropped_.load(); }
    std::uint64_t bytes_received() const { return bytes_received_.load(); }
    std::uint64_t gap_count() const { return gap_count_; }
    double elapsed_seconds() const { return timer_.elapsed_seconds(); }
    
private:
    void print_periodic_stats();
    std::uint64_t calculate_percentile(double percentile, std::uint64_t total_samples);
};

// Message parser - handles parsing of incoming byte streams
class MessageParser {
private:
    // Pre-allocated buffer for partial messages (ZERO ALLOCATION IN HOT PATH)
    static constexpr std::size_t MAX_PARTIAL_SIZE = 65536;  // 64KB max partial message
    std::array<std::uint8_t, MAX_PARTIAL_SIZE> partial_buffer_;
    std::size_t partial_size_{0};
    
public:
    MessageParser();
    
    // Parse incoming bytes and push complete messages to ring buffer
    void parse_bytes(const std::uint8_t* data, std::size_t size, RingBuffer& ring, IngestionStats& stats);
    
    // Zero-copy parsing for high performance scenarios
    void parse_bytes_zero_copy(const std::uint8_t* data, std::size_t size, RingBuffer& ring, IngestionStats& stats);
    
private:
    void process_complete_message(const Msg& msg, std::uint64_t timestamp, RingBuffer& ring, IngestionStats& stats);
};

// Network client for receiving market data
class NetworkClient {
private:
    IngestionConfig config_;
    boost::asio::io_context ctx_;
    boost::asio::ip::tcp::socket socket_;
    std::atomic<bool> should_stop_{false};
    
public:
    explicit NetworkClient(IngestionConfig config);
    
    // Connect to server
    void connect();
    
    // Main I/O loop (runs in separate thread)
    void run_io_loop(RingBuffer& ring, IngestionStats& stats, MessageParser& parser);
    
    // Signal stop
    void stop() { should_stop_ = true; }
    
    bool is_connected() const { return socket_.is_open(); }
};

// Main ingestion benchmark class
class IngestionBenchmark {
private:
    IngestionConfig config_;
    RingBuffer ring_;
    IngestionStats stats_;
    MessageParser parser_;
    NetworkClient client_;
    
    std::atomic<bool> should_stop_{false};
    
public:
    explicit IngestionBenchmark(IngestionConfig config);
    
    // Main entry point
    void run();
    
    // Statistics
    const IngestionStats& stats() const { return stats_; }
    
private:
    void consumer_loop();
    bool should_continue() const;
};

} // namespace mdfh 