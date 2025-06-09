#include "mdfh/kernel_bypass.hpp"
#include "mdfh/ring_buffer.hpp"
#include "mdfh/ingestion.hpp"
#include "mdfh/timing.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace mdfh;

struct BenchmarkConfig {
    // Network settings
    std::string host = "127.0.0.1";
    std::uint16_t port = 9001;
    std::string interface = "eth0";
    
    // Kernel bypass settings
    std::string backend = "asio";           // asio, dpdk, solarflare
    std::uint32_t rx_ring_size = 2048;
    std::uint32_t batch_size = 32;
    std::uint32_t cpu_core = 0;
    bool enable_zero_copy = true;
    bool enable_numa_awareness = true;
    
    // Performance settings
    std::uint32_t buffer_capacity = 65536;
    std::uint32_t poll_timeout_us = 100;
    std::uint32_t zero_copy_threshold = 64;
    
    // Test duration
    std::uint32_t max_seconds = 60;
    std::uint64_t max_messages = 0;
    
    // Output settings
    bool verbose = false;
    bool show_latency_histogram = false;
};

std::ostream& operator<<(std::ostream& os, const BenchmarkConfig& cfg) {
    os << "Kernel Bypass Ingestion Benchmark Configuration:\n";
    os << "  Network: " << cfg.host << ":" << cfg.port << "\n";
    os << "  Interface: " << cfg.interface << "\n";
    os << "  Backend: " << cfg.backend << "\n";
    os << "  RX Ring Size: " << cfg.rx_ring_size << "\n";
    os << "  Batch Size: " << cfg.batch_size << "\n";
    os << "  CPU Core: " << cfg.cpu_core << "\n";
    os << "  Zero-copy: " << (cfg.enable_zero_copy ? "enabled" : "disabled") << "\n";
    os << "  NUMA Awareness: " << (cfg.enable_numa_awareness ? "enabled" : "disabled") << "\n";
    os << "  Buffer Capacity: " << cfg.buffer_capacity << " slots\n";
    if (cfg.max_seconds > 0) {
        os << "  Max Duration: " << cfg.max_seconds << " seconds\n";
    }
    if (cfg.max_messages > 0) {
        os << "  Max Messages: " << cfg.max_messages << "\n";
    }
    return os;
}

BypassBackend parse_backend(const std::string& backend_str) {
    if (backend_str == "asio" || backend_str == "boost") {
        return BypassBackend::BOOST_ASIO;
    } else if (backend_str == "dpdk") {
        return BypassBackend::DPDK;
    } else if (backend_str == "solarflare" || backend_str == "ef_vi") {
        return BypassBackend::SOLARFLARE_VI;
    } else {
        std::cerr << "Unknown backend: " << backend_str << ", using Boost.Asio" << std::endl;
        return BypassBackend::BOOST_ASIO;
    }
}

class BypassBenchmark {
private:
    BenchmarkConfig config_;
    BypassConfig bypass_config_;
    RingBuffer ring_;
    IngestionStats stats_;
    BypassIngestionClient client_;
    std::atomic<bool> should_stop_{false};
    Timer benchmark_timer_;
    
public:
    explicit BypassBenchmark(BenchmarkConfig config) 
        : config_(std::move(config))
        , ring_(config_.buffer_capacity)
        , client_(create_bypass_config()) {
    }
    
    void run() {
        std::cout << config_ << std::endl;
        
        // Initialize bypass client
        if (!client_.initialize()) {
            std::cerr << "Failed to initialize kernel bypass client" << std::endl;
            return;
        }
        
        std::cout << "Using backend: " << client_.backend_info() << std::endl;
        
        // Connect to data source
        if (!client_.connect()) {
            std::cerr << "Failed to connect to data source" << std::endl;
            return;
        }
        
        std::cout << "Connected successfully, starting ingestion..." << std::endl;
        
        // Start ingestion
        client_.start_ingestion(ring_, stats_);
        
        // Run consumer loop in main thread
        consumer_loop();
        
        // Stop ingestion
        client_.stop_ingestion();
        client_.disconnect();
        
        // Print final statistics
        print_final_stats();
    }
    
private:
    BypassConfig create_bypass_config() {
        BypassConfig bypass_cfg;
        bypass_cfg.backend = parse_backend(config_.backend);
        bypass_cfg.interface_name = config_.interface;
        bypass_cfg.host = config_.host;
        bypass_cfg.port = config_.port;
        bypass_cfg.rx_ring_size = config_.rx_ring_size;
        bypass_cfg.batch_size = config_.batch_size;
        bypass_cfg.cpu_core = config_.cpu_core;
        bypass_cfg.enable_numa_awareness = config_.enable_numa_awareness;
        bypass_cfg.enable_zero_copy = config_.enable_zero_copy;
        bypass_cfg.zero_copy_threshold = config_.zero_copy_threshold;
        bypass_cfg.poll_timeout_us = config_.poll_timeout_us;
        return bypass_cfg;
    }
    
    void consumer_loop() {
        Slot slot;
        std::uint64_t messages_processed = 0;
        
        while (should_continue()) {
            if (ring_.try_pop(slot)) {
                stats_.record_message_processed(slot);
                messages_processed++;
                
                if (config_.verbose && messages_processed % 1000000 == 0) {
                    std::cout << "Processed " << messages_processed << " messages" << std::endl;
                }
            }
            
            // Periodic statistics reporting
            stats_.check_periodic_flush();
        }
        
        std::cout << "Consumer loop finished, processed " << messages_processed << " messages" << std::endl;
    }
    
    bool should_continue() const {
        if (config_.max_seconds > 0 && benchmark_timer_.elapsed_seconds() >= config_.max_seconds) {
            return false;
        }
        if (config_.max_messages > 0 && stats_.messages_processed() >= config_.max_messages) {
            return false;
        }
        return client_.is_connected();
    }
    
    void print_final_stats() {
        auto elapsed = benchmark_timer_.elapsed_seconds();
        auto msgs_recv = stats_.messages_received();
        auto msgs_proc = stats_.messages_processed();
        auto bytes_recv = stats_.bytes_received();
        auto msgs_drop = stats_.messages_dropped();
        auto gaps = stats_.gap_count();
        
        // Get kernel bypass statistics
        auto packets_recv = client_.packets_received();
        auto packet_bytes = client_.bytes_received();
        auto packets_drop = client_.packets_dropped();
        auto cpu_util = client_.cpu_utilization();
        
        std::cout << "\n=== Kernel Bypass Ingestion Benchmark Results ===" << std::endl;
        std::cout << "Backend: " << client_.backend_info() << std::endl;
        std::cout << "Duration: " << elapsed << " seconds" << std::endl;
        
        std::cout << "\n--- Network Layer Statistics ---" << std::endl;
        std::cout << "Packets received: " << packets_recv << std::endl;
        std::cout << "Packet bytes: " << packet_bytes << " (" << (packet_bytes / 1024.0 / 1024.0) << " MB)" << std::endl;
        std::cout << "Packets dropped: " << packets_drop << std::endl;
        std::cout << "Packet rate: " << (packets_recv / elapsed) << " packets/s" << std::endl;
        std::cout << "Network bandwidth: " << (packet_bytes / elapsed / 1024 / 1024) << " MB/s" << std::endl;
        if (cpu_util > 0) {
            std::cout << "CPU utilization: " << (cpu_util * 100) << "%" << std::endl;
        }
        
        std::cout << "\n--- Application Layer Statistics ---" << std::endl;
        std::cout << "Messages received: " << msgs_recv << std::endl;
        std::cout << "Messages processed: " << msgs_proc << std::endl;
        std::cout << "Messages dropped: " << msgs_drop << std::endl;
        std::cout << "Sequence gaps: " << gaps << std::endl;
        std::cout << "Message bytes: " << bytes_recv << " (" << (bytes_recv / 1024.0 / 1024.0) << " MB)" << std::endl;
        std::cout << "Message rate: " << (msgs_recv / elapsed) << " msg/s" << std::endl;
        std::cout << "Processing rate: " << (msgs_proc / elapsed) << " msg/s" << std::endl;
        
        std::cout << "\n--- Performance Analysis ---" << std::endl;
        if (packets_recv > 0) {
            double msgs_per_packet = static_cast<double>(msgs_recv) / packets_recv;
            std::cout << "Messages per packet: " << msgs_per_packet << std::endl;
        }
        
        if (msgs_recv > 0) {
            double processing_efficiency = static_cast<double>(msgs_proc) / msgs_recv * 100;
            std::cout << "Processing efficiency: " << processing_efficiency << "%" << std::endl;
        }
        
        if (packets_drop > 0 || msgs_drop > 0) {
            std::cout << "WARNING: Packets or messages were dropped!" << std::endl;
            std::cout << "  Consider increasing ring buffer sizes or CPU affinity optimization" << std::endl;
        }
        
        // Print latency histogram if requested
        if (config_.show_latency_histogram) {
            stats_.print_final_stats();
        }
    }
};

int main(int argc, char* argv[]) {
    BenchmarkConfig config;
    
    CLI::App app{"Kernel Bypass Ingestion Benchmark"};
    
    // Network settings
    app.add_option("--host", config.host, "Host address")
        ->default_val(config.host);
    app.add_option("--port,-p", config.port, "Port number")
        ->default_val(config.port);
    app.add_option("--interface,-i", config.interface, "Network interface name")
        ->default_val(config.interface);
    
    // Kernel bypass settings
    app.add_option("--backend,-b", config.backend, "Kernel bypass backend (asio, dpdk, solarflare)")
        ->default_val(config.backend);
    app.add_option("--rx-ring-size", config.rx_ring_size, "RX ring buffer size (power of 2)")
        ->default_val(config.rx_ring_size);
    app.add_option("--batch-size", config.batch_size, "Packet batch processing size")
        ->default_val(config.batch_size);
    app.add_option("--cpu-core", config.cpu_core, "CPU core for networking thread")
        ->default_val(config.cpu_core);
    app.add_flag("--no-zero-copy", "Disable zero-copy packet processing")
        ->default_val(false);
    app.add_flag("--no-numa", "Disable NUMA-aware memory allocation")
        ->default_val(false);
    
    // Performance settings
    app.add_option("--buffer-capacity", config.buffer_capacity, "Ring buffer capacity")
        ->default_val(config.buffer_capacity);
    app.add_option("--poll-timeout", config.poll_timeout_us, "Polling timeout (microseconds)")
        ->default_val(config.poll_timeout_us);
    app.add_option("--zero-copy-threshold", config.zero_copy_threshold, "Min packet size for zero-copy")
        ->default_val(config.zero_copy_threshold);
    
    // Test duration
    app.add_option("--max-seconds,-t", config.max_seconds, "Max test duration (0 = infinite)")
        ->default_val(config.max_seconds);
    app.add_option("--max-messages,-m", config.max_messages, "Max messages to process (0 = infinite)")
        ->default_val(config.max_messages);
    
    // Output settings
    app.add_flag("--verbose,-v", config.verbose, "Enable verbose output");
    app.add_flag("--latency-histogram", config.show_latency_histogram, "Show latency histogram");
    
    CLI11_PARSE(app, argc, argv);
    
    // Process flags
    if (app.count("--no-zero-copy")) {
        config.enable_zero_copy = false;
    }
    if (app.count("--no-numa")) {
        config.enable_numa_awareness = false;
    }
    
    // Validate configuration
    if (config.rx_ring_size == 0 || (config.rx_ring_size & (config.rx_ring_size - 1)) != 0) {
        std::cerr << "Error: rx_ring_size must be a power of 2" << std::endl;
        return 1;
    }
    
    if (config.batch_size == 0 || config.batch_size > config.rx_ring_size) {
        std::cerr << "Error: batch_size must be > 0 and <= rx_ring_size" << std::endl;
        return 1;
    }
    
    try {
        BypassBenchmark benchmark(config);
        benchmark.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Benchmark failed: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 