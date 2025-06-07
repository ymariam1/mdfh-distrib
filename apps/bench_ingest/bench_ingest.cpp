#include "bench_ingest.hpp"
#include <iostream>
#include <thread>
#include <CLI/CLI.hpp>
#include <cstring>
#include <time.h>  // For clock_gettime

using namespace boost::asio;
using tcp = ip::tcp;

namespace mdfh {

// High-performance timestamp function using CLOCK_MONOTONIC_RAW when available
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
    return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

// RingBuffer implementation
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

// BenchIngest implementation
BenchIngest::BenchIngest(BenchCfg cfg) 
    : cfg_(cfg), ring_(cfg.buf_capacity) {}

void BenchIngest::run() {
    start_time_ = std::chrono::steady_clock::now();
    last_flush_ = start_time_;
    
    std::cout << "Starting TCP client connecting to " << cfg_.host << ":" << cfg_.port << std::endl;
    std::cout << "Ring buffer capacity: " << cfg_.buf_capacity << " slots" << std::endl;
    if (cfg_.max_seconds > 0) {
        std::cout << "Max seconds: " << cfg_.max_seconds << std::endl;
    }
    if (cfg_.max_messages > 0) {
        std::cout << "Max messages: " << cfg_.max_messages << std::endl;
    }
    std::cout << std::endl;
    
    // Start I/O thread
    std::thread io_thread(&BenchIngest::io_worker, this);
    
    // Run consumer in main thread
    consumer_loop();
    
    // Signal stop and wait for I/O thread
    should_stop_ = true;
    if (io_thread.joinable()) {
        io_thread.join();
    }
    
    // Print final stats
    print_final_stats();
}

void BenchIngest::io_worker() {
    try {
        boost::asio::io_context ctx;
        tcp::socket sock(ctx);
        
        // Connect to server
        tcp::endpoint endpoint(boost::asio::ip::address::from_string(cfg_.host), cfg_.port);
        sock.connect(endpoint);
        
        std::cout << "Connected to " << cfg_.host << ":" << cfg_.port << std::endl;
        
        std::array<std::uint8_t, 4096> buffer;
        std::vector<std::uint8_t> partial_msg;
        partial_msg.reserve(sizeof(Msg));  // Pre-allocate to avoid reallocations
        
        while (!should_stop_.load(std::memory_order_acquire)) {
            boost::system::error_code ec;
            std::size_t bytes_read = sock.read_some(boost::asio::buffer(buffer), ec);
            
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
            
            bytes_received_.fetch_add(bytes_read, std::memory_order_relaxed);
            process_bytes(buffer.data(), bytes_read, partial_msg);
        }
    }
    catch (std::exception& e) {
        std::cerr << "I/O worker error: " << e.what() << std::endl;
        should_stop_ = true;
    }
}

void BenchIngest::process_bytes(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& partial_msg) {
    std::size_t offset = 0;
    
    // If we have partial data from previous read, prepend it
    if (!partial_msg.empty()) {
        std::size_t old_size = partial_msg.size();
        partial_msg.resize(old_size + size);
        std::memcpy(partial_msg.data() + old_size, data, size);
        data = partial_msg.data();
        size = partial_msg.size();
        offset = 0;
    }
    
    // Process complete messages
    while (offset + sizeof(Msg) <= size) {
        // Get timestamp per message for better accuracy
        auto now_ns = get_timestamp_ns();
        
        Msg msg;
        std::memcpy(&msg, data + offset, sizeof(Msg));
        
        Slot slot{msg, now_ns};
        
        if (!ring_.try_push(slot)) {
            // Ring buffer full - track drops
            messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            messages_received_.fetch_add(1, std::memory_order_relaxed);
        }
        
        offset += sizeof(Msg);
    }
    
    // Handle partial message at end - use memcpy to avoid reallocation
    if (offset < size) {
        std::size_t remaining = size - offset;
        partial_msg.resize(remaining);
        std::memcpy(partial_msg.data(), data + offset, remaining);
    } else {
        partial_msg.clear();
    }
}

void BenchIngest::consumer_loop() {
    Slot slot;
    std::uint64_t empty_spins = 0;
    const std::uint64_t max_empty_spins = 1000;
    
    while (!should_stop_.load(std::memory_order_acquire)) {
        // Check exit criteria
        if (cfg_.max_seconds > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time_;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg_.max_seconds) {
                break;
            }
        }
        
        if (cfg_.max_messages > 0 && messages_received_.load(std::memory_order_acquire) >= cfg_.max_messages) {
            break;
        }
        
        if (ring_.try_pop(slot)) {
            empty_spins = 0;
            process_message(slot);
            check_periodic_flush();
        } else {
            // No messages available - busy-spin briefly, then sleep
            empty_spins++;
            if (empty_spins > max_empty_spins) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                empty_spins = 0;
            } else {
                // Use _mm_pause() equivalent for better spin behavior
                #if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
                #elif defined(__aarch64__)
                __asm__ __volatile__("yield" ::: "memory");
                #else
                std::this_thread::yield();
                #endif
            }
        }
    }
}

void BenchIngest::process_message(const Slot& slot) {
    const auto& msg = slot.raw;
    
    // Gap detection - bootstrap from first message
    if (!first_message_seen_) {
        expected_seq_ = msg.seq + 1;
        first_message_seen_ = true;
    } else {
        if (msg.seq != expected_seq_) {
            gap_count_ += std::abs(static_cast<int64_t>(msg.seq) - static_cast<int64_t>(expected_seq_));
            expected_seq_ = msg.seq;
        }
        expected_seq_++;
    }
    
    // Latency measurement
    auto now_ns = get_timestamp_ns();
    auto latency_ns = now_ns - slot.rx_ts;
    
    // Convert to microseconds and bucket appropriately
    auto latency_us = latency_ns / 1000;
    std::size_t bucket;
    if (latency_us >= 1000) {
        bucket = 1000;  // Overflow bucket for >=1000µs
    } else {
        bucket = static_cast<std::size_t>(latency_us);
    }
    latency_buckets_[bucket]++;
}

void BenchIngest::check_periodic_flush() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_flush_ >= std::chrono::seconds(1)) {
        print_periodic_stats();
        latency_buckets_.fill(0);  // Reset histogram
        last_flush_ = now;
    }
}

void BenchIngest::print_periodic_stats() {
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    
    // Calculate percentiles
    std::uint64_t total_samples = 0;
    for (auto count : latency_buckets_) {
        total_samples += count;
    }
    
    if (total_samples > 0) {
        auto p50 = calculate_percentile(50, total_samples);
        auto p95 = calculate_percentile(95, total_samples);
        auto p99 = calculate_percentile(99, total_samples);
        
        auto drops = messages_dropped_.load(std::memory_order_relaxed);
        
        std::cout << elapsed_sec << "s p50=" << p50 << "µs p95=" << p95 << "µs p99=" << p99 << "µs"
                  << " msgs=" << messages_received_.load()
                  << " gaps=" << gap_count_ 
                  << " drops=" << drops << std::endl;
    }
}

std::uint64_t BenchIngest::calculate_percentile(double percentile, std::uint64_t total_samples) {
    std::uint64_t target = static_cast<std::uint64_t>(total_samples * percentile / 100.0);
    std::uint64_t running_sum = 0;
    
    for (std::size_t i = 0; i < latency_buckets_.size() - 1; ++i) {  // Exclude overflow bucket from iteration
        running_sum += latency_buckets_[i];
        if (running_sum >= target) {
            return i;  // Return bucket index (microseconds)
        }
    }
    
    // If we reach here, the percentile is in the overflow bucket
    return 1000;  // Return 1000µs for overflow cases
}

void BenchIngest::print_final_stats() {
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    
    auto total_msgs = messages_received_.load();
    auto total_bytes = bytes_received_.load();
    auto total_drops = messages_dropped_.load();
    
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Runtime: " << elapsed_sec << "." << (elapsed_ms % 1000) << "s" << std::endl;
    std::cout << "Messages received: " << total_msgs << std::endl;
    std::cout << "Messages dropped: " << total_drops << std::endl;
    std::cout << "Bytes received: " << total_bytes << std::endl;
    std::cout << "Gap count: " << gap_count_ << std::endl;
    
    if (elapsed_sec > 0) {
        std::cout << "Average rate: " << (total_msgs / elapsed_sec) << " msgs/sec" << std::endl;
        std::cout << "Average throughput: " << (total_bytes / elapsed_sec / 1024 / 1024) << " MB/sec" << std::endl;
    }
    
    std::cout << "Ring buffer final size: " << ring_.size() << " slots" << std::endl;
}

} // namespace mdfh

int main(int argc, char** argv) {
    mdfh::BenchCfg cfg;
    
    CLI::App app{"Market data feed ingestion benchmark"};
    
    app.add_option("--host", cfg.host, "TCP server host")
       ->default_val(cfg.host);
    app.add_option("--port,-p", cfg.port, "TCP server port")
       ->default_val(cfg.port);
    app.add_option("--seconds", cfg.max_seconds, "Run for specified seconds (0 = infinite)")
       ->default_val(cfg.max_seconds);
    app.add_option("--max-msgs", cfg.max_messages, "Process max messages then exit (0 = infinite)")
       ->default_val(cfg.max_messages);
    app.add_option("--buf-cap", cfg.buf_capacity, "Ring buffer capacity (power of 2)")
       ->default_val(cfg.buf_capacity);
    
    CLI11_PARSE(app, argc, argv);
    
    // Validate buffer capacity is power of 2
    if ((cfg.buf_capacity & (cfg.buf_capacity - 1)) != 0) {
        std::cerr << "Error: --buf-cap must be a power of 2" << std::endl;
        return 1;
    }
    
    // Use the cleaner configuration output
    std::cout << cfg << std::endl;
    
    try {
        mdfh::BenchIngest bench(cfg);
        bench.run();
    }
    catch (std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 