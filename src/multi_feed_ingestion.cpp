#include "mdfh/multi_feed_ingestion.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>

namespace mdfh {

// FeedConfig implementation
bool FeedConfig::is_valid() const {
    return !name.empty() && !host.empty() && port > 0 && 
           heartbeat_interval_ms > 0 && timeout_multiplier > 0 &&
           buffer_capacity > 0 && (buffer_capacity & (buffer_capacity - 1)) == 0; // power of 2
}

// MultiFeedConfig implementation
MultiFeedConfig MultiFeedConfig::from_yaml(const std::string& filename) {
    MultiFeedConfig config;
    
    try {
        YAML::Node yaml = YAML::LoadFile(filename);
        
        // Global settings
        if (yaml["global"]) {
            auto global = yaml["global"];
            if (global["buffer_capacity"]) {
                config.global_buffer_capacity = global["buffer_capacity"].as<std::uint32_t>();
            }
            if (global["dispatcher_threads"]) {
                config.dispatcher_threads = global["dispatcher_threads"].as<std::uint32_t>();
            }
            if (global["max_seconds"]) {
                config.max_seconds = global["max_seconds"].as<std::uint32_t>();
            }
            if (global["max_messages"]) {
                config.max_messages = global["max_messages"].as<std::uint64_t>();
            }
            if (global["health_check_interval_ms"]) {
                config.health_check_interval_ms = global["health_check_interval_ms"].as<std::uint32_t>();
            }
        }
        
        // Feed configurations
        if (yaml["feeds"]) {
            std::uint32_t origin_id = 0;
            for (const auto& feed_node : yaml["feeds"]) {
                FeedConfig feed;
                feed.origin_id = origin_id++;
                
                if (feed_node["name"]) {
                    feed.name = feed_node["name"].as<std::string>();
                } else {
                    feed.name = "feed_" + std::to_string(feed.origin_id);
                }
                
                if (feed_node["host"]) {
                    feed.host = feed_node["host"].as<std::string>();
                }
                if (feed_node["port"]) {
                    feed.port = feed_node["port"].as<std::uint16_t>();
                }
                if (feed_node["is_primary"]) {
                    feed.is_primary = feed_node["is_primary"].as<bool>();
                }
                if (feed_node["heartbeat_interval_ms"]) {
                    feed.heartbeat_interval_ms = feed_node["heartbeat_interval_ms"].as<std::uint32_t>();
                }
                if (feed_node["timeout_multiplier"]) {
                    feed.timeout_multiplier = feed_node["timeout_multiplier"].as<std::uint32_t>();
                }
                if (feed_node["buffer_capacity"]) {
                    feed.buffer_capacity = feed_node["buffer_capacity"].as<std::uint32_t>();
                }
                
                if (feed.is_valid()) {
                    config.feeds.push_back(std::move(feed));
                } else {
                    std::cerr << "Warning: Invalid feed configuration for " << feed.name << std::endl;
                }
            }
        }
    }
    catch (const YAML::Exception& e) {
        std::cerr << "YAML parsing error: " << e.what() << std::endl;
        throw std::runtime_error("Failed to parse YAML configuration");
    }
    
    return config;
}

MultiFeedConfig MultiFeedConfig::from_cli_feeds(const std::vector<std::string>& feed_specs) {
    MultiFeedConfig config;
    
    std::uint32_t origin_id = 0;
    std::regex feed_regex(R"(^([^:]+):(\d+)$)");
    
    for (const auto& spec : feed_specs) {
        std::smatch match;
        if (std::regex_match(spec, match, feed_regex)) {
            FeedConfig feed;
            feed.name = "feed_" + std::to_string(origin_id);
            feed.origin_id = origin_id++;
            feed.host = match[1].str();
            feed.port = static_cast<std::uint16_t>(std::stoul(match[2].str()));
            feed.is_primary = (origin_id == 1); // First feed is primary
            
            if (feed.is_valid()) {
                config.feeds.push_back(std::move(feed));
            } else {
                std::cerr << "Warning: Invalid feed specification: " << spec << std::endl;
            }
        } else {
            std::cerr << "Warning: Invalid feed format: " << spec << " (expected host:port)" << std::endl;
        }
    }
    
    return config;
}

bool MultiFeedConfig::is_valid() const {
    if (feeds.empty()) {
        return false;
    }
    
    // Check for duplicate origin IDs
    std::set<std::uint32_t> origin_ids;
    for (const auto& feed : feeds) {
        if (!feed.is_valid()) {
            return false;
        }
        if (origin_ids.count(feed.origin_id)) {
            return false; // Duplicate origin ID
        }
        origin_ids.insert(feed.origin_id);
    }
    
    // Ensure power of 2 buffer capacity
    return global_buffer_capacity > 0 && 
           (global_buffer_capacity & (global_buffer_capacity - 1)) == 0 &&
           dispatcher_threads > 0 && health_check_interval_ms > 0;
}

// MPSCRingBuffer implementation
MPSCRingBuffer::MPSCRingBuffer(std::uint64_t capacity) 
    : capacity_(capacity), mask_(capacity - 1) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        throw std::invalid_argument("Capacity must be a power of 2");
    }
    slots_.resize(capacity);
}

bool MPSCRingBuffer::try_push(const MultiFeedSlot& slot) {
    std::uint64_t write_pos = write_pos_.load(std::memory_order_relaxed);
    std::uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
    
    if (write_pos - read_pos >= capacity_) {
        return false; // Buffer full
    }
    
    std::uint64_t expected = write_pos;
    if (!write_pos_.compare_exchange_weak(expected, write_pos + 1, std::memory_order_acq_rel)) {
        return false; // Another producer got there first
    }
    
    slots_[write_pos & mask_] = slot;
    return true;
}

bool MPSCRingBuffer::try_pop(MultiFeedSlot& slot) {
    std::uint64_t read_pos = read_pos_.load(std::memory_order_relaxed);
    std::uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
    
    if (read_pos >= write_pos) {
        return false; // Buffer empty
    }
    
    slot = slots_[read_pos & mask_];
    read_pos_.store(read_pos + 1, std::memory_order_release);
    return true;
}

std::uint64_t MPSCRingBuffer::size() const {
    std::uint64_t write_pos = write_pos_.load(std::memory_order_acquire);
    std::uint64_t read_pos = read_pos_.load(std::memory_order_acquire);
    return write_pos - read_pos;
}

// FeedMonitor implementation
FeedMonitor::FeedMonitor(FeedConfig config) : config_(std::move(config)) {
    last_message_time_.store(std::chrono::steady_clock::now());
}

void FeedMonitor::record_message(const Msg& msg, std::uint64_t bytes) {
    messages_received_.fetch_add(1, std::memory_order_relaxed);
    bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
    last_message_time_.store(std::chrono::steady_clock::now(), std::memory_order_release);
    
    // Track sequence gaps
    std::lock_guard<std::mutex> lock(gap_mutex_);
    if (!first_message_seen_) {
        expected_sequence_ = msg.seq;
        first_message_seen_ = true;
    } else {
        if (msg.seq != expected_sequence_) {
            sequence_gaps_.fetch_add(1, std::memory_order_relaxed);
        }
        expected_sequence_ = msg.seq + 1;
    }
    
    last_sequence_.store(msg.seq, std::memory_order_release);
    
    // Update status to healthy if we were connecting
    FeedStatus current = status_.load();
    if (current == FeedStatus::CONNECTING) {
        status_.store(FeedStatus::HEALTHY, std::memory_order_release);
    }
}

void FeedMonitor::record_connection_established() {
    status_.store(FeedStatus::HEALTHY, std::memory_order_release);
}

void FeedMonitor::record_connection_failed() {
    status_.store(FeedStatus::FAILED, std::memory_order_release);
}

void FeedMonitor::check_health() {
    auto now = std::chrono::steady_clock::now();
    auto last_msg = last_message_time_.load(std::memory_order_acquire);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_msg);
    
    std::uint32_t timeout_ms = config_.heartbeat_interval_ms * config_.timeout_multiplier;
    
    FeedStatus current = status_.load();
    if (current == FeedStatus::HEALTHY || current == FeedStatus::DEGRADED) {
        if (elapsed.count() > timeout_ms) {
            status_.store(FeedStatus::DEAD, std::memory_order_release);
        } else if (elapsed.count() > config_.heartbeat_interval_ms * 2) {
            status_.store(FeedStatus::DEGRADED, std::memory_order_release);
        }
    }
}

bool FeedMonitor::is_healthy() const {
    FeedStatus status = status_.load();
    return status == FeedStatus::HEALTHY || status == FeedStatus::DEGRADED;
}

bool FeedMonitor::is_dead() const {
    FeedStatus status = status_.load();
    return status == FeedStatus::DEAD || status == FeedStatus::FAILED;
}

void FeedMonitor::print_stats() const {
    const char* status_str = "UNKNOWN";
    switch (status_.load()) {
        case FeedStatus::CONNECTING: status_str = "CONNECTING"; break;
        case FeedStatus::HEALTHY: status_str = "HEALTHY"; break;
        case FeedStatus::DEGRADED: status_str = "DEGRADED"; break;
        case FeedStatus::DEAD: status_str = "DEAD"; break;
        case FeedStatus::FAILED: status_str = "FAILED"; break;
    }
    
    std::cout << "Feed " << config_.name << " [" << config_.host << ":" << config_.port << "] "
              << "Status: " << status_str << " | "
              << "Messages: " << messages_received_.load() << " | "
              << "Gaps: " << sequence_gaps_.load() << " | "
              << "Last Seq: " << last_sequence_.load() << std::endl;
}

// FeedWorker implementation
FeedWorker::FeedWorker(FeedConfig config) : config_(std::move(config)) {
    monitor_ = std::make_unique<FeedMonitor>(config_);
    
    // Create ingestion config for this feed
    IngestionConfig ing_config;
    ing_config.host = config_.host;
    ing_config.port = config_.port;
    ing_config.buffer_capacity = config_.buffer_capacity;
    
    client_ = std::make_unique<NetworkClient>(ing_config);
    parser_ = std::make_unique<MessageParser>();
    local_buffer_ = std::make_unique<RingBuffer>(config_.buffer_capacity);
}

FeedWorker::~FeedWorker() {
    stop();
}

void FeedWorker::start(MPSCRingBuffer& global_buffer) {
    should_stop_.store(false);
    worker_thread_ = std::thread([this, &global_buffer]() {
        worker_loop(global_buffer);
    });
}

void FeedWorker::stop() {
    should_stop_.store(true);
    if (client_) {
        client_->stop();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool FeedWorker::is_running() const {
    return worker_thread_.joinable() && !should_stop_.load();
}

void FeedWorker::worker_loop(MPSCRingBuffer& global_buffer) {
    try {
        // Connect to feed
        client_->connect();
        monitor_->record_connection_established();
        
        // Create a custom stats adapter that inherits from IngestionStats
        class StatsAdapter : public IngestionStats {
        private:
            FeedMonitor* monitor_;
        public:
            explicit StatsAdapter(FeedMonitor* monitor) : monitor_(monitor) {}
            
            void record_message_processed(const Slot& slot) override {
                IngestionStats::record_message_processed(slot);
                monitor_->record_message(slot.raw, sizeof(Msg));
            }
        };
        
        StatsAdapter stats_adapter(monitor_.get());
        
        // Start I/O thread for this feed
        std::thread io_thread([this, &stats_adapter]() {
            client_->run_io_loop(*local_buffer_, stats_adapter, *parser_);
        });
        
        // Process messages from local buffer to global buffer
        while (!should_stop_.load()) {
            process_local_messages(global_buffer);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        // Clean up I/O thread
        client_->stop();
        if (io_thread.joinable()) {
            io_thread.join();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Feed worker error for " << config_.name << ": " << e.what() << std::endl;
        monitor_->record_connection_failed();
    }
}

void FeedWorker::process_local_messages(MPSCRingBuffer& global_buffer) {
    Slot local_slot;
    while (local_buffer_->try_pop(local_slot)) {
        // Record message in monitor
        monitor_->record_message(local_slot.raw, sizeof(Msg));
        
        // Create multi-feed slot and push to global buffer
        MultiFeedSlot global_slot(local_slot, config_.origin_id, local_slot.raw.seq);
        
        if (!global_buffer.try_push(global_slot)) {
            // Global buffer full - could implement backpressure here
            std::cerr << "Warning: Global buffer full, dropping message from " << config_.name << std::endl;
        }
    }
}

// FanInDispatcher implementation
FanInDispatcher::FanInDispatcher(MultiFeedConfig config) : config_(std::move(config)) {
    global_buffer_ = std::make_unique<MPSCRingBuffer>(config_.global_buffer_capacity);
    
    // Create workers for each feed
    for (const auto& feed_config : config_.feeds) {
        workers_.push_back(std::make_unique<FeedWorker>(feed_config));
    }
}

FanInDispatcher::~FanInDispatcher() {
    stop();
}

void FanInDispatcher::start() {
    should_stop_.store(false);
    
    // Start all feed workers
    for (auto& worker : workers_) {
        worker->start(*global_buffer_);
    }
    
    // Start health monitoring thread
    health_monitor_thread_ = std::thread([this]() {
        health_monitor_loop();
    });
    
    std::cout << "Started " << workers_.size() << " feed workers" << std::endl;
}

void FanInDispatcher::stop() {
    should_stop_.store(true);
    
    // Stop all workers
    for (auto& worker : workers_) {
        worker->stop();
    }
    
    // Stop health monitor
    if (health_monitor_thread_.joinable()) {
        health_monitor_thread_.join();
    }
}

bool FanInDispatcher::try_consume_message(MultiFeedSlot& slot) {
    return global_buffer_->try_pop(slot);
}

void FanInDispatcher::print_health_summary() const {
    std::cout << "\n=== Feed Health Summary ===" << std::endl;
    for (const auto& worker : workers_) {
        worker->monitor().print_stats();
    }
    std::cout << "Global buffer size: " << global_buffer_->size() << "/" << global_buffer_->capacity() << std::endl;
}

std::uint64_t FanInDispatcher::total_messages_received() const {
    std::uint64_t total = 0;
    for (const auto& worker : workers_) {
        total += worker->monitor().messages_received();
    }
    return total;
}

void FanInDispatcher::health_monitor_loop() {
    while (!should_stop_.load()) {
        // Check health of all feeds
        for (auto& worker : workers_) {
            const_cast<FeedMonitor&>(worker->monitor()).check_health();
        }
        
        // Promote backup feeds if needed
        promote_backup_feeds();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.health_check_interval_ms));
    }
}

void FanInDispatcher::promote_backup_feeds() {
    // Simple promotion logic: if primary feed is dead, mark first healthy backup as primary
    bool primary_healthy = false;
    
    for (const auto& worker : workers_) {
        if (worker->monitor().config().is_primary && worker->monitor().is_healthy()) {
            primary_healthy = true;
            break;
        }
    }
    
    if (!primary_healthy) {
        for (auto& worker : workers_) {
            if (!worker->monitor().config().is_primary && worker->monitor().is_healthy()) {
                // In a real implementation, you'd update the config to mark this as primary
                std::cout << "Promoting backup feed " << worker->monitor().config().name << " to primary" << std::endl;
                break;
            }
        }
    }
}

// MultiFeedIngestionBenchmark implementation
MultiFeedIngestionBenchmark::MultiFeedIngestionBenchmark(MultiFeedConfig config) 
    : config_(std::move(config)) {
    dispatcher_ = std::make_unique<FanInDispatcher>(config_);
}

MultiFeedIngestionBenchmark::~MultiFeedIngestionBenchmark() = default;

void MultiFeedIngestionBenchmark::run() {
    std::cout << "Starting multi-feed ingestion benchmark with " << config_.feeds.size() << " feeds" << std::endl;
    
    // Start dispatcher
    dispatcher_->start();
    
    // Run consumer loop
    consumer_loop();
    
    // Stop dispatcher
    dispatcher_->stop();
    
    // Print final statistics
    print_final_stats();
}

void MultiFeedIngestionBenchmark::consumer_loop() {
    MultiFeedSlot slot;
    auto last_health_print = std::chrono::steady_clock::now();
    
    while (should_continue()) {
        if (dispatcher_->try_consume_message(slot)) {
            messages_processed_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Print health summary periodically
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_health_print).count() >= 5) {
            dispatcher_->print_health_summary();
            last_health_print = now;
        }
    }
}

bool MultiFeedIngestionBenchmark::should_continue() const {
    if (config_.max_seconds > 0 && elapsed_seconds() >= config_.max_seconds) {
        return false;
    }
    if (config_.max_messages > 0 && messages_processed() >= config_.max_messages) {
        return false;
    }
    return true;
}

void MultiFeedIngestionBenchmark::print_final_stats() {
    auto elapsed = elapsed_seconds();
    auto total_processed = messages_processed();
    auto total_received = dispatcher_->total_messages_received();
    
    std::cout << "\n=== Final Multi-Feed Statistics ===" << std::endl;
    std::cout << "Duration: " << elapsed << " seconds" << std::endl;
    std::cout << "Total messages received: " << total_received << std::endl;
    std::cout << "Total messages processed: " << total_processed << std::endl;
    std::cout << "Average processing rate: " << (total_processed / elapsed) << " msg/s" << std::endl;
    std::cout << "Average ingestion rate: " << (total_received / elapsed) << " msg/s" << std::endl;
    
    dispatcher_->print_health_summary();
}

} // namespace mdfh 