#include "mdfh/simulator.hpp"
#include <iostream>

using namespace boost::asio;
using tcp = ip::tcp;
using udp = ip::udp;

namespace mdfh {

// SimulatorConfig output operator
std::ostream& operator<<(std::ostream& os, const SimulatorConfig& cfg) {
    os << "Market Data Simulator Configuration:\n";
    os << "  Transport: " << cfg.transport << "\n";
    os << "  Encoding: " << cfg.encoding << "\n";
    os << "  Port: " << cfg.port << "\n";
    if (cfg.transport == TransportType::UDP_MULTICAST) {
        os << "  Multicast Address: " << cfg.mcast_addr << "\n";
        os << "  Interface: " << cfg.interface << "\n";
    }
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

// MarketDataGenerator implementation
MarketDataGenerator::MarketDataGenerator(const SimulatorConfig& config) {
    reset(config);
}

void MarketDataGenerator::reset(const SimulatorConfig& config) {
    rng_.seed(config.seed);
    price_dist_ = std::uniform_real_distribution<double>(-config.price_jitter, config.price_jitter);
    qty_dist_ = std::uniform_int_distribution<std::int32_t>(1, config.max_quantity);
    current_price_ = config.base_price;
    sequence_ = 0;
}

void MarketDataGenerator::generate_batch(std::vector<Msg>& batch) {
    for (auto& msg : batch) {
        current_price_ += price_dist_(rng_);
        // Ensure price doesn't go negative
        if (current_price_ < 0.01) {
            current_price_ = 0.01;
        }
        
        std::int32_t qty = qty_dist_(rng_);
        // Randomly make it a sell order (negative quantity)
        if (rng_() % 2 == 0) {
            qty = -qty;
        }
        
        msg = Msg{++sequence_, current_price_, qty};
    }
}

// TCPTransport implementation
TCPTransport::TCPTransport(tcp::socket socket) : socket_(std::move(socket)) {}

void TCPTransport::send(const std::vector<std::uint8_t>& data) {
    boost::asio::write(socket_, boost::asio::buffer(data));
}

bool TCPTransport::is_connected() const {
    return socket_.is_open();
}

// UDPMulticastTransport implementation
UDPMulticastTransport::UDPMulticastTransport(boost::asio::io_context& ctx, const SimulatorConfig& config)
    : socket_(ctx, udp::endpoint(udp::v4(), 0))
    , endpoint_(ip::address::from_string(config.mcast_addr), config.port) {
    
    // Set up multicast interface if specified
    if (config.interface != "0.0.0.0") {
        auto interface_addr = ip::address::from_string(config.interface);
        if (interface_addr.is_v4()) {
            socket_.set_option(ip::multicast::outbound_interface(interface_addr.to_v4()));
        }
    }
}

void UDPMulticastTransport::send(const std::vector<std::uint8_t>& data) {
    socket_.send_to(boost::asio::buffer(data), endpoint_);
}

bool UDPMulticastTransport::is_connected() const {
    return socket_.is_open();
}

// MarketDataSimulator implementation
MarketDataSimulator::MarketDataSimulator(SimulatorConfig config)
    : config_(std::move(config))
    , generator_(config_)
    , encoder_(create_encoder(config_.encoding, config_.encoding_config))
    , rate_limiter_(config_.rate, config_.batch_size) {}

void MarketDataSimulator::set_transport(std::unique_ptr<Transport> transport) {
    transport_ = std::move(transport);
}

void MarketDataSimulator::run() {
    if (!transport_) {
        throw std::runtime_error("Transport not set");
    }
    
    std::cout << config_ << std::endl;
    
    std::vector<Msg> batch(config_.batch_size);
    std::vector<std::uint8_t> encoded_buffer;
    encoded_buffer.reserve(config_.batch_size * sizeof(Msg) * 2); // Extra space for protocol overhead
    
    timer_.reset();
    
    while (should_continue() && transport_->is_connected()) {
        send_batch();
    }
    
    std::cout << "\nSimulation completed:\n";
    std::cout << "  Messages sent: " << messages_sent_ << "\n";
    std::cout << "  Duration: " << timer_.elapsed_seconds() << " seconds\n";
    std::cout << "  Actual rate: " << (messages_sent_ / timer_.elapsed_seconds()) << " msgs/sec\n";
}

bool MarketDataSimulator::should_continue() const {
    if (config_.max_seconds > 0 && timer_.elapsed_s().count() >= config_.max_seconds) {
        return false;
    }
    if (config_.max_messages > 0 && messages_sent_ >= config_.max_messages) {
        return false;
    }
    return true;
}

void MarketDataSimulator::send_batch() {
    // Wait for the right time to send
    rate_limiter_.wait_for_next_tick();
    
    // Generate market data
    std::vector<Msg> batch(config_.batch_size);
    generator_.generate_batch(batch);
    
    // Encode messages
    std::vector<std::uint8_t> encoded_data;
    encoder_->encode_inplace(batch, encoded_data);
    
    // Send over transport
    transport_->send(encoded_data);
    
    messages_sent_ += config_.batch_size;
}

// Factory functions
std::unique_ptr<Transport> create_tcp_transport(tcp::socket socket) {
    return std::make_unique<TCPTransport>(std::move(socket));
}

std::unique_ptr<Transport> create_udp_transport(boost::asio::io_context& ctx, const SimulatorConfig& config) {
    return std::make_unique<UDPMulticastTransport>(ctx, config);
}

} // namespace mdfh 