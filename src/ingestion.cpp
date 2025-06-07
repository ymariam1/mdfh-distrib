#include "mdfh/ingestion.hpp"
#include <iostream>
#include <iomanip>
#include <cstring>

using namespace boost::asio;
using tcp = ip::tcp;

namespace mdfh {

// IngestionConfig output operator
std::ostream& operator<<(std::ostream& os, const IngestionConfig& cfg) {
    os << "Ingestion Benchmark Configuration:\n";
    os << "  Host: " << cfg.host << "\n";
    os << "  Port: " << cfg.port << "\n";
    os << "  Buffer Capacity: " << cfg.buffer_capacity << " slots\n";
    if (cfg.max_seconds > 0) {
        os << "  Max Duration: " << cfg.max_seconds << " seconds\n";
    }
    if (cfg.max_messages > 0) {
        os << "  Max Messages: " << cfg.max_messages << "\n";
    }
    return os;
}

// IngestionStats implementation
IngestionStats::IngestionStats() 
    : last_flush_(std::chrono::steady_clock::now()) {}

void IngestionStats::record_bytes_received(std::uint64_t bytes) {
    bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
}

void IngestionStats::record_message_received() {
    messages_received_.fetch_add(1, std::memory_order_relaxed);
}

void IngestionStats::record_message_processed(const Slot& slot) {
    messages_processed_.fetch_add(1, std::memory_order_relaxed);
    
    // Track sequence gaps
    if (!first_message_seen_) {
        expected_seq_ = slot.raw.seq;
        first_message_seen_ = true;
    } else {
        if (slot.raw.seq != expected_seq_) {
            gap_count_++;
        }
        expected_seq_ = slot.raw.seq + 1;
    }
    
    // Calculate latency and update histogram
    auto now_ns = get_timestamp_ns();
    auto latency_ns = now_ns - slot.rx_ts;
    auto latency_us = latency_ns / 1000;
    
    if (latency_us < 1000) {
        latency_buckets_[latency_us]++;
    } else {
        latency_buckets_[1000]++;  // Overflow bucket
    }
}

void IngestionStats::record_message_dropped() {
    messages_dropped_.fetch_add(1, std::memory_order_relaxed);
}

void IngestionStats::check_periodic_flush() {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_flush_).count() >= 1) {
        print_periodic_stats();
        last_flush_ = now;
    }
}

void IngestionStats::print_periodic_stats() {
    auto elapsed = timer_.elapsed_seconds();
    auto msgs_recv = messages_received_.load();
    auto msgs_proc = messages_processed_.load();
    auto bytes_recv = bytes_received_.load();
    auto msgs_drop = messages_dropped_.load();
    
    std::cout << std::fixed << std::setprecision(1)
              << "T+" << std::setw(6) << elapsed << "s | "
              << "Recv: " << std::setw(8) << msgs_recv << " msgs | "
              << "Proc: " << std::setw(8) << msgs_proc << " msgs | "
              << "Drop: " << std::setw(6) << msgs_drop << " | "
              << "Rate: " << std::setw(8) << (msgs_recv / elapsed) << " msg/s | "
              << "BW: " << std::setw(6) << (bytes_recv / elapsed / 1024 / 1024) << " MB/s"
              << std::endl;
}

void IngestionStats::print_final_stats() {
    auto elapsed = timer_.elapsed_seconds();
    auto msgs_recv = messages_received_.load();
    auto msgs_proc = messages_processed_.load();
    auto bytes_recv = bytes_received_.load();
    auto msgs_drop = messages_dropped_.load();
    
    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Duration: " << elapsed << " seconds\n";
    std::cout << "Messages received: " << msgs_recv << "\n";
    std::cout << "Messages processed: " << msgs_proc << "\n";
    std::cout << "Messages dropped: " << msgs_drop << "\n";
    std::cout << "Sequence gaps: " << gap_count_ << "\n";
    std::cout << "Bytes received: " << bytes_recv << " (" << (bytes_recv / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "Average rate: " << (msgs_recv / elapsed) << " msg/s\n";
    std::cout << "Average bandwidth: " << (bytes_recv / elapsed / 1024 / 1024) << " MB/s\n";
    
    // Latency percentiles
    std::uint64_t total_samples = msgs_proc;
    if (total_samples > 0) {
        std::cout << "\nLatency percentiles (microseconds):\n";
        std::cout << "  50th: " << calculate_percentile(0.50, total_samples) << "µs\n";
        std::cout << "  90th: " << calculate_percentile(0.90, total_samples) << "µs\n";
        std::cout << "  95th: " << calculate_percentile(0.95, total_samples) << "µs\n";
        std::cout << "  99th: " << calculate_percentile(0.99, total_samples) << "µs\n";
        std::cout << "  99.9th: " << calculate_percentile(0.999, total_samples) << "µs\n";
    }
}

std::uint64_t IngestionStats::calculate_percentile(double percentile, std::uint64_t total_samples) {
    std::uint64_t target = static_cast<std::uint64_t>(total_samples * percentile);
    std::uint64_t count = 0;
    
    for (std::size_t i = 0; i < latency_buckets_.size(); ++i) {
        count += latency_buckets_[i];
        if (count >= target) {
            return i >= 1000 ? 1000 : i;  // Return 1000+ for overflow bucket
        }
    }
    return 0;
}

// MessageParser implementation
MessageParser::MessageParser() {
    partial_buffer_.reserve(sizeof(Msg));
}

void MessageParser::process_complete_message(const Msg& msg, std::uint64_t timestamp, RingBuffer& ring, IngestionStats& stats) {
    Slot slot{msg, timestamp};
    
    if (!ring.try_push(slot)) {
        stats.record_message_dropped();
    } else {
        stats.record_message_received();
    }
}

void MessageParser::parse_bytes(const std::uint8_t* data, std::size_t size, RingBuffer& ring, IngestionStats& stats) {
    std::size_t offset = 0;
    
    // If we have partial data from previous read, prepend it
    if (!partial_buffer_.empty()) {
        std::size_t old_size = partial_buffer_.size();
        partial_buffer_.resize(old_size + size);
        std::memcpy(partial_buffer_.data() + old_size, data, size);
        data = partial_buffer_.data();
        size = partial_buffer_.size();
        offset = 0;
    }
    
    // Process complete messages
    while (offset + sizeof(Msg) <= size) {
        auto now_ns = get_timestamp_ns();
        
        Msg msg;
        std::memcpy(&msg, data + offset, sizeof(Msg));
        
        process_complete_message(msg, now_ns, ring, stats);
        offset += sizeof(Msg);
    }
    
    // Save any remaining partial data
    if (offset < size) {
        std::size_t remaining = size - offset;
        partial_buffer_.assign(data + offset, data + offset + remaining);
    } else {
        partial_buffer_.clear();
    }
}

void MessageParser::parse_bytes_zero_copy(const std::uint8_t* data, std::size_t size, RingBuffer& ring, IngestionStats& stats) {
    // For zero-copy, we need to handle partial messages differently
    // This is a simplified version - in practice, you'd want more sophisticated buffering
    parse_bytes(data, size, ring, stats);
}

// NetworkClient implementation
NetworkClient::NetworkClient(IngestionConfig config) 
    : config_(std::move(config)), socket_(ctx_) {}

void NetworkClient::connect() {
    tcp::endpoint endpoint(ip::address::from_string(config_.host), config_.port);
    socket_.connect(endpoint);
    std::cout << "Connected to " << config_.host << ":" << config_.port << std::endl;
}

void NetworkClient::run_io_loop(RingBuffer& ring, IngestionStats& stats, MessageParser& parser) {
    std::array<std::uint8_t, 4096> buffer;
    
    while (!should_stop_.load(std::memory_order_acquire) && socket_.is_open()) {
        try {
            boost::system::error_code ec;
            std::size_t bytes_read = socket_.read_some(boost::asio::buffer(buffer), ec);
            
            if (ec) {
                if (ec == boost::asio::error::eof) {
                    std::cout << "Server closed connection" << std::endl;
                } else {
                    std::cerr << "Read error: " << ec.message() << std::endl;
                }
                break;
            }
            
            if (bytes_read == 0) {
                break;
            }
            
            stats.record_bytes_received(bytes_read);
            parser.parse_bytes_zero_copy(buffer.data(), bytes_read, ring, stats);
        }
        catch (std::exception& e) {
            std::cerr << "I/O error: " << e.what() << std::endl;
            break;
        }
    }
}

// IngestionBenchmark implementation
IngestionBenchmark::IngestionBenchmark(IngestionConfig config)
    : config_(std::move(config))
    , ring_(config_.buffer_capacity)
    , client_(config_) {}

void IngestionBenchmark::run() {
    std::cout << config_ << std::endl;
    
    // Connect to server
    client_.connect();
    
    // Start I/O thread
    std::thread io_thread([this]() {
        client_.run_io_loop(ring_, stats_, parser_);
    });
    
    // Run consumer in main thread
    consumer_loop();
    
    // Signal stop and wait for I/O thread
    client_.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    
    // Drain any remaining messages
    Slot slot;
    while (ring_.try_pop(slot)) {
        stats_.record_message_processed(slot);
    }
    
    // Print final statistics
    stats_.print_final_stats();
}

void IngestionBenchmark::consumer_loop() {
    Slot slot;
    
    while (should_continue()) {
        if (ring_.try_pop(slot)) {
            stats_.record_message_processed(slot);
        }
        
        // Periodic statistics reporting
        stats_.check_periodic_flush();
    }
}

bool IngestionBenchmark::should_continue() const {
    if (config_.max_seconds > 0 && stats_.elapsed_seconds() >= config_.max_seconds) {
        return false;
    }
    if (config_.max_messages > 0 && stats_.messages_processed() >= config_.max_messages) {
        return false;
    }
    return client_.is_connected();
}

} // namespace mdfh 