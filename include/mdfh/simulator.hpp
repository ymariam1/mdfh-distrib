#pragma once

#include "core.hpp"
#include "encoding.hpp"
#include "timing.hpp"
#include <boost/asio.hpp>
#include <string>
#include <random>
#include <memory>

namespace mdfh {

// Configuration for the market data simulator
struct SimulatorConfig {
    // Network settings
    std::uint16_t port = 9001;
    std::string mcast_addr = "239.255.1.1";
    std::string interface = "0.0.0.0";
    TransportType transport = TransportType::TCP;
    EncodingType encoding = EncodingType::BINARY;
    
    // Performance settings
    std::uint32_t rate = 100'000;       // messages per second
    std::uint32_t batch_size = 100;     // messages per batch
    
    // Market data generation
    std::uint64_t seed = 42;            // RNG seed for reproducible runs
    double base_price = 100.0;          // starting price
    double price_jitter = 0.05;         // ± max price movement per tick
    std::int32_t max_quantity = 100;    // maximum order quantity
    
    // Encoding configuration
    EncodingConfig encoding_config;
    
    // Exit criteria
    std::uint32_t max_seconds = 0;      // run duration (0 = infinite)
    std::uint64_t max_messages = 0;     // message limit (0 = infinite)
};

// Output stream operator for configuration display
std::ostream& operator<<(std::ostream& os, const SimulatorConfig& cfg);

// Market data generator - creates realistic market data sequences
class MarketDataGenerator {
private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> price_dist_;
    std::uniform_int_distribution<std::int32_t> qty_dist_;
    
    double current_price_;
    std::uint64_t sequence_;
    
public:
    explicit MarketDataGenerator(const SimulatorConfig& config);
    
    // Generate a batch of market data messages
    void generate_batch(std::vector<Msg>& batch);
    
    // Reset generator state
    void reset(const SimulatorConfig& config);
};

// Abstract base class for transport implementations
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const std::vector<std::uint8_t>& data) = 0;
    virtual bool is_connected() const = 0;
};

// TCP transport implementation
class TCPTransport : public Transport {
private:
    boost::asio::ip::tcp::socket socket_;
    
public:
    explicit TCPTransport(boost::asio::ip::tcp::socket socket);
    void send(const std::vector<std::uint8_t>& data) override;
    bool is_connected() const override;
};

// UDP multicast transport implementation
class UDPMulticastTransport : public Transport {
private:
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint endpoint_;
    
public:
    UDPMulticastTransport(boost::asio::io_context& ctx, const SimulatorConfig& config);
    void send(const std::vector<std::uint8_t>& data) override;
    bool is_connected() const override;
};

// Main simulator class - orchestrates market data generation and transmission
class MarketDataSimulator {
private:
    SimulatorConfig config_;
    MarketDataGenerator generator_;
    std::unique_ptr<MessageEncoder> encoder_;
    std::unique_ptr<Transport> transport_;
    RateLimiter rate_limiter_;
    Timer timer_;
    
    // Statistics
    std::uint64_t messages_sent_ = 0;
    
public:
    explicit MarketDataSimulator(SimulatorConfig config);
    
    // Set transport (for dependency injection)
    void set_transport(std::unique_ptr<Transport> transport);
    
    // Main simulation loop
    void run();
    
    // Statistics
    std::uint64_t messages_sent() const { return messages_sent_; }
    double elapsed_seconds() const { return timer_.elapsed_seconds(); }
    
private:
    bool should_continue() const;
    void send_batch();
};

// Factory functions for creating transports
std::unique_ptr<Transport> create_tcp_transport(boost::asio::ip::tcp::socket socket);
std::unique_ptr<Transport> create_udp_transport(boost::asio::io_context& ctx, const SimulatorConfig& config);

} // namespace mdfh 