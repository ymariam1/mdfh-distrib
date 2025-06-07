#pragma once

#include <cstdint>
#include <boost/asio.hpp>

namespace mdfh {

struct SimCfg {
    // --- Network ---
    std::uint16_t port   = 9001;        // TCP listen port

    // --- Throughput / batching ---
    std::uint32_t rate   = 100'000;     // messages per second (sustained target)
    std::uint32_t batch  = 100;         // msgs per send() call

    // --- RNG & price process ---
    std::uint64_t seed   = 42;          // RNG seed (deterministic runs)
    double        basepx = 100.0;       // starting price
    double        jitter = 0.05;        // ± max tick size
    std::int32_t  qtymax = 100;         // |quantity| upper bound
};

#pragma pack(push, 1)
struct Msg {
    std::uint64_t seq;          // monotonically increasing
    double        px;           // IEEE-754, little-endian
    std::int32_t  qty;          // positive => buy, negative => sell
};
#pragma pack(pop)
static_assert(sizeof(Msg) == 20);

// Function declarations
void run_session(boost::asio::ip::tcp::socket sock, SimCfg cfg);
void run_simulator(SimCfg cfg);

} // namespace mdfh