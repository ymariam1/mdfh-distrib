#include "mdfh/core.hpp"
#include <boost/asio.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <random>
#include <mutex>

using namespace mdfh;
using namespace boost::asio;
using tcp = ip::tcp;

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9001;
    std::uint32_t rate = 50000;        // messages per second
    std::uint32_t batch_size = 100;    // messages per batch
    std::uint32_t max_seconds = 0;     // 0 = infinite
    std::uint64_t max_messages = 0;    // 0 = infinite
    bool verbose = false;
    double base_price = 100.0;
    double price_jitter = 0.05;
    std::uint32_t max_quantity = 1000;
};

std::ostream& operator<<(std::ostream& os, const ServerConfig& cfg) {
    os << "Market Data Server Configuration:\n";
    os << "  Listen: " << cfg.host << ":" << cfg.port << "\n";
    os << "  Rate: " << cfg.rate << " msgs/sec\n";
    os << "  Batch Size: " << cfg.batch_size << " msgs\n";
    os << "  Base Price: $" << cfg.base_price << "\n";
    os << "  Price Jitter: ±$" << cfg.price_jitter << "\n";
    os << "  Max Quantity: " << cfg.max_quantity << "\n";
    if (cfg.max_seconds > 0) {
        os << "  Max Duration: " << cfg.max_seconds << " seconds\n";
    }
    if (cfg.max_messages > 0) {
        os << "  Max Messages: " << cfg.max_messages << "\n";
    }
    return os;
}

class MarketDataServer {
private:
    ServerConfig config_;
    io_context ctx_;
    tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<tcp::socket>> clients_;
    std::mutex clients_mutex_;  // Protect clients_ vector
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    
    // Message generation
    std::mt19937 rng_;
    std::uniform_real_distribution<double> price_dist_;
    std::uniform_int_distribution<std::uint32_t> qty_dist_;
    std::uniform_int_distribution<int> side_dist_;
    
public:
    explicit MarketDataServer(ServerConfig config)
        : config_(std::move(config))
        , acceptor_(ctx_, tcp::endpoint(ip::address::from_string(config_.host), config_.port))
        , rng_(std::random_device{}())
        , price_dist_(-config_.price_jitter, config_.price_jitter)
        , qty_dist_(1, config_.max_quantity)
        , side_dist_(0, 1) {
        
        // Set SO_REUSEADDR to allow quick restart
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
    }
    
    ~MarketDataServer() {
        stop();
    }
    
    void start() {
        std::cout << config_ << std::endl;
        std::cout << "Starting market data server on " << acceptor_.local_endpoint() << std::endl;
        
        running_ = true;
        
        // Start accepting connections
        start_accept();
        
        // Start message generation in separate thread
        server_thread_ = std::thread([this]() {
            message_generation_loop();
        });
        
        // Run IO context
        ctx_.run();
    }
    
    void stop() {
        running_ = false;
        acceptor_.close();
        
        for (auto& client : clients_) {
            if (client && client->is_open()) {
                client->close();
            }
        }
        clients_.clear();
        
        ctx_.stop();
        
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
private:
    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(ctx_);
        
        acceptor_.async_accept(*socket,
            [this, socket](boost::system::error_code ec) {
                if (!ec) {
                    std::cout << "Client connected from " << socket->remote_endpoint() << std::endl;
                    
                    // Thread-safe client addition
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex_);
                        clients_.push_back(socket);
                        std::cout << "Total clients: " << clients_.size() << std::endl;
                    }
                    
                    start_accept(); // Continue accepting more connections
                } else if (ec != boost::asio::error::operation_aborted) {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                    start_accept();
                }
            });
    }
    
    void message_generation_loop() {
        std::cout << "Starting message generation..." << std::endl;
        
        auto start_time = std::chrono::steady_clock::now();
        std::uint64_t messages_sent = 0;
        std::uint64_t sequence = 1;
        
        // Calculate timing for rate limiting with higher precision
        auto messages_per_batch = config_.batch_size;
        auto batch_interval_ns = std::chrono::nanoseconds(
            (1000000000ULL * messages_per_batch) / config_.rate);
        
        if (config_.verbose) {
            std::cout << "Batch interval: " << batch_interval_ns.count() << " nanoseconds" << std::endl;
        }
        
        while (running_) {
            auto batch_start = std::chrono::steady_clock::now();
            
            // Check exit conditions
            if (config_.max_seconds > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    batch_start - start_time).count();
                if (elapsed >= config_.max_seconds) {
                    break;
                }
            }
            
            if (config_.max_messages > 0 && messages_sent >= config_.max_messages) {
                break;
            }
            
            // Wait for at least one client (thread-safe check)
            bool has_clients = false;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                has_clients = !clients_.empty();
            }
            
            if (!has_clients) {
                if (config_.verbose) {
                    std::cout << "Waiting for clients to connect..." << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            if (config_.verbose && messages_sent == 0) {
                std::size_t client_count = 0;
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    client_count = clients_.size();
                }
                std::cout << "Starting to send messages to " << client_count << " clients..." << std::endl;
            }
            
            // Generate and send a batch of messages
            std::vector<Msg> batch;
            batch.reserve(messages_per_batch);
            
            for (std::uint32_t i = 0; i < messages_per_batch && running_; ++i) {
                Msg msg = generate_message(sequence++);
                batch.push_back(msg);
            }
            
            // Send batch to all connected clients
            send_batch_to_clients(batch);
            messages_sent += batch.size();
            
            // Adaptive verbose output based on rate
            std::uint64_t report_interval = std::max(1000ULL, static_cast<std::uint64_t>(config_.rate) / 10); // Report 10 times per second max
            if (config_.verbose && (messages_sent % report_interval == 0 || messages_sent <= 1000)) {
                std::size_t client_count = 0;
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    client_count = clients_.size();
                }
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                auto current_rate = elapsed > 0 ? messages_sent / elapsed : 0;
                std::cout << "Sent " << messages_sent << " messages to " 
                          << client_count << " clients (rate: " << current_rate << " msg/s)" << std::endl;
            }
            
            // Rate limiting - sleep until next batch time
            auto batch_end = std::chrono::steady_clock::now();
            auto batch_duration = batch_end - batch_start;
            
            if (batch_duration < batch_interval_ns) {
                auto sleep_time = batch_interval_ns - batch_duration;
                if (sleep_time > std::chrono::microseconds(1)) {
                    std::this_thread::sleep_for(sleep_time);
                }
            }
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
        
        std::cout << "\nMessage generation completed:" << std::endl;
        std::cout << "  Total messages sent: " << messages_sent << std::endl;
        std::cout << "  Duration: " << elapsed << " seconds" << std::endl;
        std::cout << "  Average rate: " << (messages_sent / std::max(1LL, elapsed)) << " msgs/sec" << std::endl;
        
        // Close all client connections
        for (auto& client : clients_) {
            if (client && client->is_open()) {
                client->close();
            }
        }
    }
    
    Msg generate_message(std::uint64_t sequence) {
        Msg msg;
        msg.seq = sequence;
        msg.px = config_.base_price + price_dist_(rng_);
        msg.qty = (side_dist_(rng_) == 0 ? 1 : -1) * static_cast<std::int32_t>(qty_dist_(rng_));
        return msg;
    }
    
    void send_batch_to_clients(const std::vector<Msg>& batch) {
        if (batch.empty()) return;
        
        std::lock_guard<std::mutex> lock(clients_mutex_);
        
        // Remove disconnected clients
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [](const std::shared_ptr<tcp::socket>& client) {
                    return !client || !client->is_open();
                }),
            clients_.end());
        
        // Send batch to all connected clients
        for (auto& client : clients_) {
            if (client && client->is_open()) {
                boost::system::error_code ec;
                write(*client, buffer(batch.data(), batch.size() * sizeof(Msg)), ec);
                
                if (ec) {
                    std::cout << "Send error to client: " << ec.message() << std::endl;
                    client->close();
                }
            }
        }
    }
};

// Global server instance for signal handling
std::unique_ptr<MarketDataServer> g_server;

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down server..." << std::endl;
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    ServerConfig config;
    
    CLI::App app{"Market Data Server - generates synthetic market data for benchmarking"};
    
    // Network settings
    app.add_option("--host", config.host, "Host address to bind to")
        ->default_val(config.host);
    app.add_option("--port,-p", config.port, "Port to listen on")
        ->default_val(config.port);
    
    // Message generation settings
    app.add_option("--rate,-r", config.rate, "Message rate (msgs/sec)")
        ->default_val(config.rate);
    app.add_option("--batch-size,-b", config.batch_size, "Messages per batch")
        ->default_val(config.batch_size);
    app.add_option("--max-seconds,-t", config.max_seconds, "Max duration (0 = infinite)")
        ->default_val(config.max_seconds);
    app.add_option("--max-messages,-m", config.max_messages, "Max messages (0 = infinite)")
        ->default_val(config.max_messages);
    
    // Market data settings
    app.add_option("--base-price", config.base_price, "Base price for generated messages")
        ->default_val(config.base_price);
    app.add_option("--price-jitter", config.price_jitter, "Price jitter range (±)")
        ->default_val(config.price_jitter);
    app.add_option("--max-quantity", config.max_quantity, "Maximum quantity per message")
        ->default_val(config.max_quantity);
    
    // Output settings
    app.add_flag("--verbose,-v", config.verbose, "Enable verbose output");
    
    CLI11_PARSE(app, argc, argv);
    
    // Validate configuration
    if (config.rate == 0) {
        std::cerr << "Error: Message rate must be > 0" << std::endl;
        return 1;
    }
    
    if (config.batch_size == 0) {
        std::cerr << "Error: Batch size must be > 0" << std::endl;
        return 1;
    }
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        g_server = std::make_unique<MarketDataServer>(config);
        g_server->start();
    }
    catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 