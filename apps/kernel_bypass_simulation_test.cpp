#include "mdfh/simulator.hpp"
#include "mdfh/kernel_bypass.hpp"
#include "mdfh/ring_buffer.hpp"
#include "mdfh/ingestion.hpp"
#include "mdfh/timing.hpp"
#include <CLI/CLI.hpp>
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <signal.h>

using namespace mdfh;
using namespace boost::asio;
using tcp = ip::tcp;
using udp = ip::udp;

struct TestConfig {
    // Server settings
    std::uint16_t server_port = 9001;
    std::string server_interface = "127.0.0.1";
    TransportType transport = TransportType::TCP;
    
    // Simulation settings
    std::uint32_t rate = 50000;           // messages per second
    std::uint32_t batch_size = 100;       // messages per batch
    std::uint32_t max_seconds = 30;       // simulation duration
    std::uint64_t max_messages = 0;       // message limit (0 = infinite)
    
    // Client bypass settings
    std::string backend = "asio";         // asio, dpdk, solarflare
    std::uint32_t rx_ring_size = 2048;
    std::uint32_t client_batch_size = 32;
    std::uint32_t cpu_core = 0;
    bool enable_zero_copy = false;        // Start with false for basic test
    
    // Test settings
    bool run_server_only = false;
    bool run_client_only = false;
    bool verbose = false;
    std::uint32_t warmup_seconds = 5;
};

std::ostream& operator<<(std::ostream& os, const TestConfig& cfg) {
    os << "Kernel Bypass Simulation Test Configuration:\n";
    os << "  Transport: " << cfg.transport << "\n";
    os << "  Server: " << cfg.server_interface << ":" << cfg.server_port << "\n";
    os << "  Simulation Rate: " << cfg.rate << " msgs/sec\n";
    os << "  Simulation Batch: " << cfg.batch_size << " msgs\n";
    os << "  Backend: " << cfg.backend << "\n";
    os << "  RX Ring Size: " << cfg.rx_ring_size << "\n";
    os << "  Client Batch Size: " << cfg.client_batch_size << "\n";
    os << "  Zero-copy: " << (cfg.enable_zero_copy ? "enabled" : "disabled") << "\n";
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

// Global flag for clean shutdown
std::atomic<bool> should_stop{false};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    should_stop = true;
}

class TCPSimulationServer {
private:
    io_context& ctx_;
    tcp::acceptor acceptor_;
    MarketDataSimulator simulator_;
    std::vector<std::shared_ptr<tcp::socket>> clients_;
    std::thread server_thread_;
    
public:
    TCPSimulationServer(io_context& ctx, const TestConfig& config)
        : ctx_(ctx)
        , acceptor_(ctx, tcp::endpoint(ip::address::from_string(config.server_interface), config.server_port))
        , simulator_(create_simulator_config(config)) {
        
        // Set SO_REUSEADDR to allow quick restart
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
    }
    
    ~TCPSimulationServer() {
        stop();
    }
    
    void start() {
        std::cout << "Starting TCP simulation server on " 
                  << acceptor_.local_endpoint() << std::endl;
        
        // Start accepting connections
        start_accept();
        
        // Start simulator in a separate thread
        server_thread_ = std::thread([this]() {
            try {
                // Wait for at least one client
                while (clients_.empty() && !should_stop) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                if (!should_stop && !clients_.empty()) {
                    std::cout << "Client connected, starting simulation..." << std::endl;
                    simulator_.run();
                }
            } catch (const std::exception& e) {
                std::cerr << "Simulator error: " << e.what() << std::endl;
            }
        });
    }
    
    void stop() {
        should_stop = true;
        acceptor_.close();
        
        for (auto& client : clients_) {
            if (client->is_open()) {
                client->close();
            }
        }
        clients_.clear();
        
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
private:
    SimulatorConfig create_simulator_config(const TestConfig& config) {
        SimulatorConfig sim_config;
        sim_config.port = config.server_port;
        sim_config.transport = config.transport;
        sim_config.rate = config.rate;
        sim_config.batch_size = config.batch_size;
        sim_config.max_seconds = config.max_seconds;
        sim_config.max_messages = config.max_messages;
        return sim_config;
    }
    
    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(ctx_);
        
        acceptor_.async_accept(*socket,
            [this, socket](boost::system::error_code ec) {
                if (!ec) {
                    std::cout << "Client connected from " << socket->remote_endpoint() << std::endl;
                    clients_.push_back(socket);
                    
                    // Set up transport for simulator
                    if (clients_.size() == 1) {
                        auto transport = create_tcp_transport(std::move(*socket));
                        simulator_.set_transport(std::move(transport));
                    }
                    
                    start_accept(); // Continue accepting more connections
                } else if (ec != boost::asio::error::operation_aborted) {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                    start_accept();
                }
            });
    }
};

class UDPSimulationServer {
private:
    io_context& ctx_;
    MarketDataSimulator simulator_;
    std::thread server_thread_;
    
public:
    UDPSimulationServer(io_context& ctx, const TestConfig& config)
        : ctx_(ctx)
        , simulator_(create_simulator_config(config)) {
        
        // Set up UDP multicast transport
        auto transport = create_udp_transport(ctx_, create_simulator_config(config));
        simulator_.set_transport(std::move(transport));
    }
    
    ~UDPSimulationServer() {
        stop();
    }
    
    void start() {
        std::cout << "Starting UDP simulation server" << std::endl;
        
        server_thread_ = std::thread([this]() {
            try {
                // Small delay to allow client to connect
                std::this_thread::sleep_for(std::chrono::seconds(2));
                
                if (!should_stop) {
                    std::cout << "Starting UDP simulation..." << std::endl;
                    simulator_.run();
                }
            } catch (const std::exception& e) {
                std::cerr << "UDP Simulator error: " << e.what() << std::endl;
            }
        });
    }
    
    void stop() {
        should_stop = true;
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
private:
    SimulatorConfig create_simulator_config(const TestConfig& config) {
        SimulatorConfig sim_config;
        sim_config.port = config.server_port;
        sim_config.transport = TransportType::UDP_MULTICAST;
        sim_config.mcast_addr = "239.255.1.1";
        sim_config.interface = config.server_interface;
        sim_config.rate = config.rate;
        sim_config.batch_size = config.batch_size;
        sim_config.max_seconds = config.max_seconds;
        sim_config.max_messages = config.max_messages;
        return sim_config;
    }
};

class BypassTestClient {
private:
    TestConfig config_;
    BypassConfig bypass_config_;
    RingBuffer ring_;
    IngestionStats stats_;
    BypassIngestionClient client_;
    std::atomic<bool> running_{false};
    Timer test_timer_;
    
public:
    explicit BypassTestClient(TestConfig config)
        : config_(std::move(config))
        , ring_(65536)  // Large ring buffer for testing
        , client_(create_bypass_config()) {
    }
    
    bool run() {
        std::cout << "\n=== Starting Kernel Bypass Client Test ===" << std::endl;
        std::cout << "Backend: " << config_.backend << std::endl;
        
        // Initialize bypass client
        if (!client_.initialize()) {
            std::cerr << "Failed to initialize kernel bypass client" << std::endl;
            return false;
        }
        
        std::cout << "Using backend: " << client_.backend_info() << std::endl;
        
        // Connect to server
        std::cout << "Connecting to " << config_.server_interface 
                  << ":" << config_.server_port << std::endl;
        
        if (!client_.connect()) {
            std::cerr << "Failed to connect to server" << std::endl;
            return false;
        }
        
        std::cout << "Connected successfully!" << std::endl;
        
        // Warmup period
        if (config_.warmup_seconds > 0) {
            std::cout << "Warming up for " << config_.warmup_seconds << " seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(config_.warmup_seconds));
        }
        
        // Start ingestion
        std::cout << "Starting ingestion..." << std::endl;
        client_.start_ingestion(ring_, stats_);
        running_ = true;
        
        // Run consumer loop
        consumer_loop();
        
        // Stop and cleanup
        client_.stop_ingestion();
        client_.disconnect();
        
        // Print results
        print_results();
        
        return true;
    }
    
private:
    BypassConfig create_bypass_config() {
        BypassConfig bypass_cfg;
        bypass_cfg.backend = parse_backend(config_.backend);
        bypass_cfg.interface_name = "lo";  // loopback for local testing
        bypass_cfg.host = config_.server_interface;
        bypass_cfg.port = config_.server_port;
        bypass_cfg.rx_ring_size = config_.rx_ring_size;
        bypass_cfg.batch_size = config_.client_batch_size;
        bypass_cfg.cpu_core = config_.cpu_core;
        bypass_cfg.enable_zero_copy = config_.enable_zero_copy;
        bypass_cfg.poll_timeout_us = 100;
        return bypass_cfg;
    }
    
    void consumer_loop() {
        Slot slot;
        std::uint64_t messages_processed = 0;
        std::uint64_t last_report = 0;
        Timer report_timer;
        
        std::cout << "Consumer loop started..." << std::endl;
        
        while (should_continue() && running_) {
            if (ring_.try_pop(slot)) {
                stats_.record_message_processed(slot);
                messages_processed++;
                
                if (config_.verbose && messages_processed % 100000 == 0) {
                    std::cout << "Processed " << messages_processed << " messages" << std::endl;
                }
            }
            
            // Report progress every 5 seconds
            if (report_timer.elapsed_seconds() >= 5.0) {
                auto current_rate = (messages_processed - last_report) / 5.0;
                std::cout << "Current rate: " << static_cast<std::uint64_t>(current_rate) << " msgs/sec" << std::endl;
                last_report = messages_processed;
                report_timer.reset();
            }
            
            // Check periodic statistics
            stats_.check_periodic_flush();
        }
        
        std::cout << "Consumer loop finished, processed " << messages_processed << " messages" << std::endl;
    }
    
    bool should_continue() const {
        if (should_stop) return false;
        
        if (config_.max_seconds > 0 && test_timer_.elapsed_seconds() >= config_.max_seconds) {
            return false;
        }
        if (config_.max_messages > 0 && stats_.messages_processed() >= config_.max_messages) {
            return false;
        }
        return client_.is_connected();
    }
    
    void print_results() {
        auto elapsed = test_timer_.elapsed_seconds();
        auto msgs_recv = stats_.messages_received();
        auto msgs_proc = stats_.messages_processed();
        auto bytes_recv = stats_.bytes_received();
        auto msgs_drop = stats_.messages_dropped();
        
        // Get kernel bypass statistics
        auto packets_recv = client_.packets_received();
        auto packet_bytes = client_.bytes_received();
        auto packets_drop = client_.packets_dropped();
        auto cpu_util = client_.cpu_utilization();
        
        std::cout << "\n=== Kernel Bypass Test Results ===" << std::endl;
        std::cout << "Backend: " << client_.backend_info() << std::endl;
        std::cout << "Duration: " << elapsed << " seconds" << std::endl;
        
        std::cout << "\n--- Network Layer ---" << std::endl;
        std::cout << "Packets received: " << packets_recv << std::endl;
        std::cout << "Packet bytes: " << packet_bytes << " (" << (packet_bytes / 1024.0 / 1024.0) << " MB)" << std::endl;
        std::cout << "Packets dropped: " << packets_drop << std::endl;
        std::cout << "Packet rate: " << (packets_recv / elapsed) << " packets/s" << std::endl;
        
        std::cout << "\n--- Application Layer ---" << std::endl;
        std::cout << "Messages received: " << msgs_recv << std::endl;
        std::cout << "Messages processed: " << msgs_proc << std::endl;
        std::cout << "Messages dropped: " << msgs_drop << std::endl;
        std::cout << "Message rate: " << (msgs_proc / elapsed) << " msgs/s" << std::endl;
        std::cout << "Throughput: " << (bytes_recv / elapsed / 1024 / 1024) << " MB/s" << std::endl;
        
        if (cpu_util > 0) {
            std::cout << "CPU utilization: " << (cpu_util * 100) << "%" << std::endl;
        }
        
        // Validation
        double loss_rate = (msgs_drop > 0) ? (double(msgs_drop) / double(msgs_recv)) * 100.0 : 0.0;
        std::cout << "\n--- Validation ---" << std::endl;
        std::cout << "Message loss rate: " << loss_rate << "%" << std::endl;
        
        if (loss_rate < 0.1) {
            std::cout << "✓ PASS: Message loss within acceptable limits" << std::endl;
        } else {
            std::cout << "✗ FAIL: High message loss detected" << std::endl;
        }
        
        if (msgs_proc > 0) {
            std::cout << "✓ PASS: Messages successfully processed" << std::endl;
        } else {
            std::cout << "✗ FAIL: No messages processed" << std::endl;
        }
    }
};

void run_server(const TestConfig& config) {
    io_context ctx;
    
    std::unique_ptr<TCPSimulationServer> tcp_server;
    std::unique_ptr<UDPSimulationServer> udp_server;
    
    if (config.transport == TransportType::TCP) {
        tcp_server = std::make_unique<TCPSimulationServer>(ctx, config);
        tcp_server->start();
    } else {
        udp_server = std::make_unique<UDPSimulationServer>(ctx, config);
        udp_server->start();
    }
    
    // Run IO context in a separate thread
    std::thread io_thread([&ctx]() {
        try {
            ctx.run();
        } catch (const std::exception& e) {
            std::cerr << "IO context error: " << e.what() << std::endl;
        }
    });
    
    // Wait for stop signal
    while (!should_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Cleanup
    if (tcp_server) tcp_server->stop();
    if (udp_server) udp_server->stop();
    
    ctx.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
}

void run_client(const TestConfig& config) {
    // Small delay to ensure server is ready
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    BypassTestClient client(config);
    client.run();
}

void run_combined_test(const TestConfig& config) {
    std::cout << "=== Running Combined Kernel Bypass Test ===" << std::endl;
    std::cout << config << std::endl;
    
    // Start server in background thread
    std::thread server_thread([config]() {
        run_server(config);
    });
    
    // Small delay to let server start
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Run client in main thread
    run_client(config);
    
    // Signal server to stop
    should_stop = true;
    
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

int main(int argc, char* argv[]) {
    TestConfig config;
    
    CLI::App app{"Kernel Bypass Simulation Test - validates kernel bypass implementation"};
    
    // Server settings
    app.add_option("--port", config.server_port, "Server port");
    app.add_option("--host", config.server_interface, "Server interface");
    app.add_option("--transport", config.transport, "Transport type (TCP/UDP_MULTICAST)");
    
    // Simulation settings
    app.add_option("--rate", config.rate, "Message rate (msgs/sec)");
    app.add_option("--batch-size", config.batch_size, "Simulation batch size");
    app.add_option("--duration", config.max_seconds, "Test duration (seconds)");
    app.add_option("--max-messages", config.max_messages, "Maximum messages to process");
    
    // Client bypass settings
    app.add_option("--backend", config.backend, "Kernel bypass backend (asio/dpdk/solarflare)");
    app.add_option("--rx-ring-size", config.rx_ring_size, "RX ring buffer size");
    app.add_option("--client-batch", config.client_batch_size, "Client batch size");
    app.add_option("--cpu-core", config.cpu_core, "CPU core for affinity");
    app.add_flag("--zero-copy", config.enable_zero_copy, "Enable zero-copy optimization");
    
    // Test modes
    app.add_flag("--server-only", config.run_server_only, "Run server only");
    app.add_flag("--client-only", config.run_client_only, "Run client only");
    app.add_flag("-v,--verbose", config.verbose, "Verbose output");
    app.add_option("--warmup", config.warmup_seconds, "Warmup period (seconds)");
    
    CLI11_PARSE(app, argc, argv);
    
    // Setup signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        if (config.run_server_only) {
            std::cout << "Running server only mode..." << std::endl;
            run_server(config);
        } else if (config.run_client_only) {
            std::cout << "Running client only mode..." << std::endl;
            run_client(config);
        } else {
            run_combined_test(config);
        }
        
        std::cout << "Test completed successfully!" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
} 