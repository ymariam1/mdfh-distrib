#pragma once

#include <cstdint>
#include <ostream>

namespace mdfh {

// Core message structure - the fundamental unit of market data
#pragma pack(push, 1)
struct Msg {
    std::uint64_t seq;          // monotonically increasing sequence number
    double        px;           // price (IEEE-754, little-endian)
    std::int32_t  qty;          // quantity (positive = buy, negative = sell)
};
#pragma pack(pop)
static_assert(sizeof(Msg) == 20, "Msg struct size must be exactly 20 bytes");

// Transport layer types
enum class TransportType {
    TCP,
    UDP_MULTICAST
};

// Message encoding formats
enum class EncodingType {
    BINARY,     // Raw binary format
    FIX,        // FIX protocol
    ITCH        // ITCH protocol
};

// Output stream operators for better debugging/logging
inline std::ostream& operator<<(std::ostream& os, TransportType transport) {
    switch (transport) {
        case TransportType::TCP: return os << "TCP";
        case TransportType::UDP_MULTICAST: return os << "UDP_MULTICAST";
    }
    return os << "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, EncodingType encoding) {
    switch (encoding) {
        case EncodingType::BINARY: return os << "BINARY";
        case EncodingType::FIX: return os << "FIX";
        case EncodingType::ITCH: return os << "ITCH";
    }
    return os << "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, const Msg& msg) {
    return os << "Msg{seq=" << msg.seq << ", px=" << msg.px << ", qty=" << msg.qty << "}";
}

} // namespace mdfh 