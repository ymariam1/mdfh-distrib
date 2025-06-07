#include "sim.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <CLI/CLI.hpp>
#include <cstring>

using namespace boost::asio;
using tcp = ip::tcp;
using udp = ip::udp;

namespace mdfh {

void run_tcp_session(tcp::socket sock, SimCfg cfg) {
    // -------- RNG setup --------
    std::mt19937_64                 rng{cfg.seed};
    std::uniform_real_distribution  dpx(-cfg.jitter, cfg.jitter);
    std::uniform_int_distribution<> dqty(1, cfg.qtymax);

    // -------- Timing --------
    const double  target_ns = 1e9 / cfg.rate;     // ns per msg
    auto          next_tick = std::chrono::steady_clock::now();

    // -------- State --------
    std::vector<Msg> batch(cfg.batch);
    double           price = cfg.basepx;
    std::uint64_t    seq   = 0;

    // -------- Pre-allocate encoding buffer to avoid heap churn --------
    std::vector<std::uint8_t> encoded_data;
    encoded_data.reserve(cfg.batch * sizeof(Msg) * 2); // Extra space for FIX/ITCH overhead

    // -------- Exit criteria --------
    std::uint64_t messages_sent = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        // Check exit criteria
        if (cfg.max_seconds > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.max_seconds) {
                break;
            }
        }
        if (cfg.max_messages > 0 && messages_sent >= cfg.max_messages) {
            break;
        }

        // 1) Fill batch
        for (auto& m : batch) {
            price += dpx(rng);
            m = Msg{++seq, price, static_cast<int32_t>(dqty(rng))};
        }
        
        // 2) Improved packet pacing - avoid drift
        auto now = std::chrono::steady_clock::now();
        auto target_interval = std::chrono::nanoseconds(static_cast<long long>(target_ns * cfg.batch));
        
        // If we're behind, catch up by advancing next_tick to the next aligned slot
        while (next_tick <= now) {
            next_tick += target_interval;
        }
        
        // Busy-spin until it's time
        while (std::chrono::steady_clock::now() < next_tick) { /* spin */ }

        // 3) Encode efficiently into pre-allocated buffer
        encoded_data.clear();
        switch (cfg.encoding) {
            case EncodingType::BINARY: {
                // Direct memcpy instead of push_back for binary encoding
                encoded_data.resize(batch.size() * sizeof(Msg));
                std::memcpy(encoded_data.data(), batch.data(), encoded_data.size());
                break;
            }
            case EncodingType::FIX:
                encoded_data = encode_fix(batch, cfg);
                break;
            case EncodingType::ITCH:
                encoded_data = encode_itch(batch, cfg);
                break;
        }
        
        std::size_t bytes = boost::asio::write(sock, boost::asio::buffer(encoded_data));
        if (bytes == 0) break;            // peer closed
        
        messages_sent += cfg.batch;
    }
}

void run_udp_multicast(SimCfg cfg) {
    try {
        std::cout << "Starting UDP multicast on " << cfg.mcast_addr << ":" << cfg.port 
                  << " at " << cfg.rate << " msgs/sec, batch size " << cfg.batch << std::endl;
        
        boost::asio::io_context ctx;
        udp::socket sock(ctx, udp::endpoint(udp::v4(), 0));
        
        // Set up multicast - wire up the interface option
        udp::endpoint multicast_endpoint(boost::asio::ip::address::from_string(cfg.mcast_addr), cfg.port);
        
        if (cfg.interface != "0.0.0.0") {
            auto interface_addr = boost::asio::ip::address::from_string(cfg.interface);
            if (interface_addr.is_v4()) {
                sock.set_option(ip::multicast::outbound_interface(interface_addr.to_v4()));
            }
        }
        
        // -------- RNG setup --------
        std::mt19937_64                 rng{cfg.seed};
        std::uniform_real_distribution  dpx(-cfg.jitter, cfg.jitter);
        std::uniform_int_distribution<> dqty(1, cfg.qtymax);

        // -------- Timing --------
        const double  target_ns = 1e9 / cfg.rate;     // ns per msg
        auto          next_tick = std::chrono::steady_clock::now();

        // -------- State --------
        std::vector<Msg> batch(cfg.batch);
        double           price = cfg.basepx;
        std::uint64_t    seq   = 0;

        // -------- Pre-allocate encoding buffer to avoid heap churn --------
        std::vector<std::uint8_t> encoded_data;
        encoded_data.reserve(cfg.batch * sizeof(Msg) * 2); // Extra space for FIX/ITCH overhead

        // -------- Exit criteria --------
        std::uint64_t messages_sent = 0;
        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            // Check exit criteria
            if (cfg.max_seconds > 0) {
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.max_seconds) {
                    break;
                }
            }
            if (cfg.max_messages > 0 && messages_sent >= cfg.max_messages) {
                break;
            }

            // 1) Fill batch
            for (auto& m : batch) {
                price += dpx(rng);
                m = Msg{++seq, price, static_cast<int32_t>(dqty(rng))};
            }
            
            // 2) Improved packet pacing - avoid drift
            auto now = std::chrono::steady_clock::now();
            auto target_interval = std::chrono::nanoseconds(static_cast<long long>(target_ns * cfg.batch));
            
            // If we're behind, catch up by advancing next_tick to the next aligned slot
            while (next_tick <= now) {
                next_tick += target_interval;
            }
            
            // Busy-spin until it's time
            while (std::chrono::steady_clock::now() < next_tick) { /* spin */ }

            // 3) Encode efficiently into pre-allocated buffer
            encoded_data.clear();
            switch (cfg.encoding) {
                case EncodingType::BINARY: {
                    // Direct memcpy instead of push_back for binary encoding
                    encoded_data.resize(batch.size() * sizeof(Msg));
                    std::memcpy(encoded_data.data(), batch.data(), encoded_data.size());
                    break;
                }
                case EncodingType::FIX:
                    encoded_data = encode_fix(batch, cfg);
                    break;
                case EncodingType::ITCH:
                    encoded_data = encode_itch(batch, cfg);
                    break;
            }
            
            sock.send_to(boost::asio::buffer(encoded_data), multicast_endpoint);
            messages_sent += cfg.batch;
        }
    }
    catch (std::exception& e) {
        std::cerr << "UDP multicast error: " << e.what() << std::endl;
        throw;
    }
}

void run_simulator(SimCfg cfg) {
    if (cfg.transport == TransportType::UDP_MULTICAST) {
        run_udp_multicast(cfg);
        return;
    }
    
    // TCP mode
    try {
        std::cout << "Starting TCP market feed simulator on port " << cfg.port 
                  << " at " << cfg.rate << " msgs/sec, batch size " << cfg.batch << std::endl;
        
        boost::asio::io_context ctx;
        tcp::acceptor    acc(ctx, tcp::endpoint(tcp::v4(), cfg.port));

        std::vector<std::thread> workers;
        auto start_time = std::chrono::steady_clock::now();
        
        // Use timeout on accept to allow checking exit criteria
        acc.non_blocking(true);
        
        for (;;) {
            // Check exit criteria for server
            if (cfg.max_seconds > 0) {
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cfg.max_seconds) {
                    break;
                }
            }
            
            boost::system::error_code ec;
            tcp::socket sock = acc.accept(ec);
            
            if (!ec) {
                std::cout << "New client connected" << std::endl;
                workers.emplace_back([s=std::move(sock), cfg] mutable {
                    try { run_tcp_session(std::move(s), cfg); }
                    catch (std::exception& e) { std::cerr << "Session error: " << e.what() << '\n'; }
                });
            } else if (ec == boost::asio::error::would_block) {
                // No incoming connection, sleep briefly and continue
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                // Some other error
                std::cerr << "Accept error: " << ec.message() << std::endl;
                break;
            }
        }
        
        // Clean thread cleanup - join all worker threads
        std::cout << "Shutting down, waiting for " << workers.size() << " worker threads..." << std::endl;
        for (auto& t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }
        std::cout << "All worker threads joined." << std::endl;
        
    }
    catch (std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        throw;
    }
}

} // namespace mdfh

int main(int argc, char** argv) {
    mdfh::SimCfg cfg;
    
    CLI::App app{"Market data feed simulator"};
    
    app.add_option("--port,-p", cfg.port, "TCP listen port / UDP port")
       ->default_val(cfg.port);
    app.add_option("--rate,-r", cfg.rate, "Messages per second")
       ->default_val(cfg.rate);
    app.add_option("--batch,-b", cfg.batch, "Messages per batch")
       ->default_val(cfg.batch);
    app.add_option("--seed,-s", cfg.seed, "RNG seed for reproducibility")
       ->default_val(cfg.seed);
    app.add_option("--basepx", cfg.basepx, "Starting base price")
       ->default_val(cfg.basepx);
    app.add_option("--jitter,-j", cfg.jitter, "Price jitter range (±)")
       ->default_val(cfg.jitter);
    app.add_option("--qtymax,-q", cfg.qtymax, "Maximum quantity per message")
       ->default_val(cfg.qtymax);
    app.add_option("--mcast-addr", cfg.mcast_addr, "Multicast address")
       ->default_val(cfg.mcast_addr);
    app.add_option("--interface", cfg.interface, "Network interface for multicast")
       ->default_val(cfg.interface);
    app.add_option("--sender-id", cfg.sender_comp_id, "FIX SenderCompID")
       ->default_val(cfg.sender_comp_id);
    app.add_option("--target-id", cfg.target_comp_id, "FIX TargetCompID")
       ->default_val(cfg.target_comp_id);
    
    // Exit criteria options
    app.add_option("--seconds", cfg.max_seconds, "Run for specified seconds (0 = infinite)")
       ->default_val(cfg.max_seconds);
    app.add_option("--max-msgs", cfg.max_messages, "Send max messages then exit (0 = infinite)")
       ->default_val(cfg.max_messages);
    
    // Transport and encoding options
    std::string transport_str = "tcp";
    std::string encoding_str = "binary";
    
    app.add_option("--transport,-t", transport_str, "Transport type: tcp, udp")
       ->default_val(transport_str);
    app.add_option("--encoding,-e", encoding_str, "Encoding type: binary, fix, itch")
       ->default_val(encoding_str);
    
    CLI11_PARSE(app, argc, argv);
    
    // Parse transport type
    if (transport_str == "tcp") {
        cfg.transport = mdfh::TransportType::TCP;
    } else if (transport_str == "udp") {
        cfg.transport = mdfh::TransportType::UDP_MULTICAST;
    } else {
        std::cerr << "Invalid transport type: " << transport_str << std::endl;
        return 1;
    }
    
    // Parse encoding type
    if (encoding_str == "binary") {
        cfg.encoding = mdfh::EncodingType::BINARY;
    } else if (encoding_str == "fix") {
        cfg.encoding = mdfh::EncodingType::FIX;
    } else if (encoding_str == "itch") {
        cfg.encoding = mdfh::EncodingType::ITCH;
    } else {
        std::cerr << "Invalid encoding type: " << encoding_str << std::endl;
        return 1;
    }
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Transport: " << transport_str << std::endl;
    std::cout << "  Encoding: " << encoding_str << std::endl;
    std::cout << "  Port: " << cfg.port << std::endl;
    if (cfg.transport == mdfh::TransportType::UDP_MULTICAST) {
        std::cout << "  Multicast address: " << cfg.mcast_addr << std::endl;
        std::cout << "  Interface: " << cfg.interface << std::endl;
    }
    std::cout << "  Rate: " << cfg.rate << " msgs/sec" << std::endl;
    std::cout << "  Batch: " << cfg.batch << " msgs/batch" << std::endl;
    std::cout << "  Seed: " << cfg.seed << std::endl;
    std::cout << "  Base price: " << cfg.basepx << std::endl;
    std::cout << "  Jitter: ±" << cfg.jitter << std::endl;
    std::cout << "  Max quantity: " << cfg.qtymax << std::endl;
    if (cfg.encoding == mdfh::EncodingType::FIX) {
        std::cout << "  FIX Sender ID: " << cfg.sender_comp_id << std::endl;
        std::cout << "  FIX Target ID: " << cfg.target_comp_id << std::endl;
    }
    if (cfg.max_seconds > 0) {
        std::cout << "  Max seconds: " << cfg.max_seconds << std::endl;
    }
    if (cfg.max_messages > 0) {
        std::cout << "  Max messages: " << cfg.max_messages << std::endl;
    }
    std::cout << std::endl;
    
    try {
        mdfh::run_simulator(cfg);
    }
    catch (std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}