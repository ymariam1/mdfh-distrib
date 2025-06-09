#pragma once

#include "core.hpp"
#include "ring_buffer.hpp"
#include "timing.hpp"
#include "ingestion.hpp"
#include <boost/asio.hpp>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace mdfh {

// Configuration for a single feed in multi-feed setup
struct FeedConfig {
    std::string name;                    // Feed identifier
    std::string host = "127.0.0.1";     // Host address
    std::uint16_t port = 9001;           // Port number
    std::uint32_t origin_id = 0;         // Unique feed identifier for sequence tracking
    bool is_primary = true;              // Primary vs backup feed
    std::uint32_t heartbeat_interval_ms = 1000;  // Expected heartbeat interval
    std::uint32_t timeout_multiplier = 3;        // Timeout = heartbeat_interval * multiplier
    
    // Performance settings
    std::uint32_t buffer_capacity = 65536;  // Per-feed ring buffer capacity
    
    // Validation
    bool is_valid() const;
};

// Multi-feed ingestion configuration
struct MultiFeedConfig {
    std::vector<FeedConfig> feeds;
    
    // Global settings
    std::uint32_t global_buffer_capacity = 262144;  // Main MPSC ring buffer capacity
    std::uint32_t dispatcher_threads = 1;           // Number of dispatcher threads
    std::uint32_t max_seconds = 0;                  // Run duration (0 = infinite)
    std::uint64_t max_messages = 0;                 // Message limit (0 = infinite)
    
    // Health monitoring
    std::uint32_t health_check_interval_ms = 100;   // Health check frequency
    
    // Load from YAML file
    static MultiFeedConfig from_yaml(const std::string& filename);
    
    // Load from CLI arguments (--feed host:port format)
    static MultiFeedConfig from_cli_feeds(const std::vector<std::string>& feed_specs);
    
    // Validation
    bool is_valid() const;
};

// Enhanced slot with feed origin information
struct MultiFeedSlot {
    Slot base_slot;                      // Original slot data
    std::uint32_t origin_id;             // Feed origin identifier
    std::uint64_t feed_sequence;         // Per-feed sequence number
    std::chrono::steady_clock::time_point arrival_time;  // When message arrived
    
    MultiFeedSlot() = default;
    MultiFeedSlot(const Slot& slot, std::uint32_t origin, std::uint64_t seq)
        : base_slot(slot), origin_id(origin), feed_sequence(seq)
        , arrival_time(std::chrono::steady_clock::now()) {}
};

// Multi-producer, single-consumer ring buffer for fan-in
class MPSCRingBuffer {
private:
    std::vector<MultiFeedSlot> slots_;
    std::atomic<std::uint64_t> write_pos_{0};
    std::atomic<std::uint64_t> read_pos_{0};
    std::uint64_t capacity_;
    std::uint64_t mask_;
    
public:
    explicit MPSCRingBuffer(std::uint64_t capacity);
    
    // Thread-safe multi-producer operations
    bool try_push(const MultiFeedSlot& slot);
    bool try_pop(MultiFeedSlot& slot);
    
    // Statistics
    std::uint64_t size() const;
    std::uint64_t capacity() const { return capacity_; }
};

// Feed health status
enum class FeedStatus {
    CONNECTING,
    HEALTHY,
    DEGRADED,    // Receiving data but with gaps/delays
    DEAD,        // No data received within timeout
    FAILED       // Connection failed
};

// Per-feed statistics and health monitoring
class FeedMonitor {
private:
    FeedConfig config_;
    std::atomic<FeedStatus> status_{FeedStatus::CONNECTING};
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> sequence_gaps_{0};
    std::atomic<std::uint64_t> last_sequence_{0};
    std::atomic<std::chrono::steady_clock::time_point> last_message_time_;
    
    // Gap tracking
    std::uint64_t expected_sequence_ = 0;
    bool first_message_seen_ = false;
    mutable std::mutex gap_mutex_;
    
public:
    explicit FeedMonitor(FeedConfig config);
    
    // Update statistics
    void record_message(const Msg& msg, std::uint64_t bytes);
    void record_connection_established();
    void record_connection_failed();
    
    // Health checking
    void check_health();
    bool is_healthy() const;
    bool is_dead() const;
    
    // Accessors
    FeedStatus status() const { return status_.load(); }
    std::uint64_t messages_received() const { return messages_received_.load(); }
    std::uint64_t bytes_received() const { return bytes_received_.load(); }
    std::uint64_t sequence_gaps() const { return sequence_gaps_.load(); }
    const FeedConfig& config() const { return config_; }
    
    // Statistics reporting
    void print_stats() const;
};

// Single feed ingestion worker
class FeedWorker {
private:
    FeedConfig config_;
    std::unique_ptr<FeedMonitor> monitor_;
    std::unique_ptr<NetworkClient> client_;
    std::unique_ptr<MessageParser> parser_;
    std::unique_ptr<RingBuffer> local_buffer_;
    std::atomic<bool> should_stop_{false};
    std::thread worker_thread_;
    
public:
    explicit FeedWorker(FeedConfig config);
    ~FeedWorker();
    
    // Lifecycle
    void start(MPSCRingBuffer& global_buffer);
    void stop();
    
    // Status
    bool is_running() const;
    const FeedMonitor& monitor() const { return *monitor_; }
    
private:
    void worker_loop(MPSCRingBuffer& global_buffer);
    void process_local_messages(MPSCRingBuffer& global_buffer);
};

// Fan-in dispatcher - coordinates multiple feed workers
class FanInDispatcher {
private:
    MultiFeedConfig config_;
    std::unique_ptr<MPSCRingBuffer> global_buffer_;
    std::vector<std::unique_ptr<FeedWorker>> workers_;
    std::atomic<bool> should_stop_{false};
    std::thread health_monitor_thread_;
    
    // Health monitoring
    Timer health_timer_;
    
public:
    explicit FanInDispatcher(MultiFeedConfig config);
    ~FanInDispatcher();
    
    // Lifecycle
    void start();
    void stop();
    
    // Consumer interface
    bool try_consume_message(MultiFeedSlot& slot);
    
    // Statistics and monitoring
    void print_health_summary() const;
    std::uint64_t total_messages_received() const;
    
private:
    void health_monitor_loop();
    void promote_backup_feeds();
};

// Main multi-feed ingestion benchmark
class MultiFeedIngestionBenchmark {
private:
    MultiFeedConfig config_;
    std::unique_ptr<FanInDispatcher> dispatcher_;
    std::atomic<bool> should_stop_{false};
    
    // Global statistics
    std::atomic<std::uint64_t> messages_processed_{0};
    Timer benchmark_timer_;
    
public:
    explicit MultiFeedIngestionBenchmark(MultiFeedConfig config);
    ~MultiFeedIngestionBenchmark();
    
    // Main entry point
    void run();
    
    // Statistics
    std::uint64_t messages_processed() const { return messages_processed_.load(); }
    double elapsed_seconds() const { return benchmark_timer_.elapsed_seconds(); }
    
private:
    void consumer_loop();
    bool should_continue() const;
    void print_final_stats();
};

} // namespace mdfh 