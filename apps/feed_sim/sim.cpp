#include "sim.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <CLI/CLI.hpp>

using namespace boost::asio;
using tcp = ip::tcp;

namespace mdfh {

void run_session(tcp::socket sock, SimCfg cfg) {
    // -------- 3.1 RNG setup --------
    std::mt19937_64                 rng{cfg.seed};
    std::uniform_real_distribution  dpx(-cfg.jitter, cfg.jitter);
    std::uniform_int_distribution<> dqty(1, cfg.qtymax);

    // -------- 3.2 Timing --------
    const double  target_ns = 1e9 / cfg.rate;     // ns per msg
    auto          next_tick = std::chrono::steady_clock::now();

    // -------- 3.3 State --------
    std::vector<Msg> batch(cfg.batch);
    double           price = cfg.basepx;
    std::uint64_t    seq   = 0;

    while (true) {
        // 1) Fill batch
        for (auto& m : batch) {
            price += dpx(rng);
            m = Msg{++seq, price, static_cast<int32_t>(dqty(rng))};
        }
        // 2) Busy-sleep until it's time
        next_tick += std::chrono::nanoseconds(
            static_cast<long long>(target_ns * cfg.batch));
        while (std::chrono::steady_clock::now() < next_tick) { /* spin */ }

        // 3) Send
        std::size_t bytes = boost::asio::write(sock,
                         boost::asio::buffer(batch.data(), batch.size()*sizeof(Msg)));
        if (bytes == 0) break;            // peer closed
    }
}

void run_simulator(SimCfg cfg) {
    try {
        std::cout << "Starting market feed simulator on port " << cfg.port 
                  << " at " << cfg.rate << " msgs/sec, batch size " << cfg.batch << std::endl;
        
        boost::asio::io_context ctx;
        tcp::acceptor    acc(ctx, tcp::endpoint(tcp::v4(), cfg.port));

        std::vector<std::thread> workers;     // Use std::thread for compatibility
        for (;;) {
            tcp::socket sock = acc.accept();   // blocking accept
            std::cout << "New client connected" << std::endl;
            workers.emplace_back([s=std::move(sock), cfg] mutable {
                try { run_session(std::move(s), cfg); }
                catch (std::exception& e) { std::cerr << "Session error: " << e.what() << '\n'; }
            });
        }
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
    
    app.add_option("--port,-p", cfg.port, "TCP listen port")
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
    
    CLI11_PARSE(app, argc, argv);
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Port: " << cfg.port << std::endl;
    std::cout << "  Rate: " << cfg.rate << " msgs/sec" << std::endl;
    std::cout << "  Batch: " << cfg.batch << " msgs/batch" << std::endl;
    std::cout << "  Seed: " << cfg.seed << std::endl;
    std::cout << "  Base price: " << cfg.basepx << std::endl;
    std::cout << "  Jitter: ±" << cfg.jitter << std::endl;
    std::cout << "  Max quantity: " << cfg.qtymax << std::endl;
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