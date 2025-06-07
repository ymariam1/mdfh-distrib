#pragma once

#include <cstdint>
#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>

// Platform-specific endianness includes
#include <arpa/inet.h>  // For htonl, htons, etc.
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#elif defined(__linux__)
#include <endian.h>
#elif defined(_WIN32)
#include <winsock2.h>
#define htobe16(x) htons(x)
#define htobe32(x) htonl(x)
#define htobe64(x) (((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#endif

namespace mdfh {

enum class TransportType {
    TCP,
    UDP_MULTICAST
};

enum class EncodingType {
    BINARY,     // Original binary format
    FIX,        // FIX protocol
    ITCH        // ITCH protocol
};

struct SimCfg {
    // --- Network ---
    std::uint16_t port   = 9001;        // TCP listen port / UDP port
    std::string   mcast_addr = "239.255.1.1";  // Multicast address
    std::string   interface = "0.0.0.0";       // Network interface for multicast
    TransportType transport = TransportType::TCP;
    EncodingType  encoding = EncodingType::BINARY;

    // --- Throughput / batching ---
    std::uint32_t rate   = 100'000;     // messages per second (sustained target)
    std::uint32_t batch  = 100;         // msgs per send() call

    // --- RNG & price process ---
    std::uint64_t seed   = 42;          // RNG seed (deterministic runs)
    double        basepx = 100.0;       // starting price
    double        jitter = 0.05;        // ± max tick size
    std::int32_t  qtymax = 100;         // |quantity| upper bound
    
    // --- FIX/ITCH specific ---
    std::string   sender_comp_id = "MDFH_SIM";   // FIX SenderCompID
    std::string   target_comp_id = "CLIENT";     // FIX TargetCompID
    
    // --- Exit criteria ---
    std::uint32_t max_seconds = 0;      // Run for specified seconds (0 = infinite)
    std::uint64_t max_messages = 0;     // Send max messages then exit (0 = infinite)
};

#pragma pack(push, 1)
struct Msg {
    std::uint64_t seq;          // monotonically increasing
    double        px;           // IEEE-754, little-endian
    std::int32_t  qty;          // positive => buy, negative => sell
};
#pragma pack(pop)
static_assert(sizeof(Msg) == 20);

// SOFH (Simple Open Framing Header) for FIX/ITCH
#pragma pack(push, 1)
struct SOFH {
    std::uint32_t message_length;   // Big-endian, includes SOFH
    std::uint16_t encoding_type;    // Big-endian, 0x5000 for FIX, 0x4954 for ITCH
};
#pragma pack(pop)
static_assert(sizeof(SOFH) == 6);

// ITCH message format
#pragma pack(push, 1)
struct ITCHMsg {
    char          msg_type;     // 'Q' for quote
    std::uint64_t timestamp;    // Big-endian nanoseconds
    std::uint64_t seq;          // Big-endian sequence number
    std::uint32_t price;        // Big-endian, scaled by 10000
    std::uint32_t qty;          // Big-endian absolute quantity
    char          side;         // 'B' for buy, 'S' for sell
};
#pragma pack(pop)
static_assert(sizeof(ITCHMsg) == 26);

// Utility functions
inline std::uint8_t calculate_fix_checksum(const std::string& msg) {
    std::uint32_t sum = 0;
    for (char c : msg) {
        sum += static_cast<std::uint8_t>(c);
    }
    return static_cast<std::uint8_t>(sum % 256);
}

// Encoding functions
inline std::vector<std::uint8_t> encode_binary(const std::vector<Msg>& msgs) {
    std::vector<std::uint8_t> buffer;
    buffer.resize(msgs.size() * sizeof(Msg));
    std::memcpy(buffer.data(), msgs.data(), buffer.size());
    return buffer;
}

// More efficient binary encoding that reuses a pre-allocated buffer
inline void encode_binary_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) {
    buffer.resize(msgs.size() * sizeof(Msg));
    std::memcpy(buffer.data(), msgs.data(), buffer.size());
}

inline std::vector<std::uint8_t> encode_fix(const std::vector<Msg>& msgs, const SimCfg& cfg) {
    std::vector<std::uint8_t> buffer;
    
    for (const auto& msg : msgs) {
        std::ostringstream fix_msg;
        
        // FIX 4.4 Market Data Incremental Refresh
        fix_msg << "8=FIX.4.4\x01";  // BeginString
        fix_msg << "35=X\x01";       // MsgType (Market Data Incremental Refresh)
        fix_msg << "49=" << cfg.sender_comp_id << "\x01";  // SenderCompID
        fix_msg << "56=" << cfg.target_comp_id << "\x01";  // TargetCompID
        fix_msg << "34=" << msg.seq << "\x01";             // MsgSeqNum
        
        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::gmtime(&time_t);
        fix_msg << "52=" << std::put_time(&tm, "%Y%m%d-%H:%M:%S") << "\x01";
        
        // Market data entries
        fix_msg << "268=1\x01";      // NoMDEntries
        fix_msg << "279=0\x01";      // MDUpdateAction (New)
        fix_msg << "269=" << (msg.qty > 0 ? "0" : "1") << "\x01";  // MDEntryType (Bid/Offer)
        fix_msg << "270=" << std::fixed << std::setprecision(4) << msg.px << "\x01";  // MDEntryPx
        fix_msg << "271=" << std::abs(msg.qty) << "\x01";  // MDEntrySize
        
        std::string fix_body = fix_msg.str();
        
        // Calculate body length (excluding BeginString and BodyLength fields)
        std::size_t body_start = fix_body.find("35=");
        std::string body_length_str = "9=" + std::to_string(fix_body.length() - body_start) + "\x01";
        
        // Insert body length after BeginString
        std::size_t begin_end = fix_body.find("\x01") + 1;
        fix_body.insert(begin_end, body_length_str);
        
        // Calculate and append checksum
        std::uint8_t checksum = calculate_fix_checksum(fix_body);
        fix_body += "10=" + std::to_string(checksum).substr(0, 3) + "\x01";
        
        // Add SOFH header
        SOFH sofh;
        sofh.message_length = htobe32(static_cast<std::uint32_t>(sizeof(SOFH) + fix_body.length()));
        sofh.encoding_type = htobe16(0x5000);  // FIX encoding type
        
        // Append to buffer
        const std::uint8_t* sofh_ptr = reinterpret_cast<const std::uint8_t*>(&sofh);
        buffer.insert(buffer.end(), sofh_ptr, sofh_ptr + sizeof(SOFH));
        buffer.insert(buffer.end(), fix_body.begin(), fix_body.end());
    }
    
    return buffer;
}

inline std::vector<std::uint8_t> encode_itch(const std::vector<Msg>& msgs, const SimCfg& cfg) {
    std::vector<std::uint8_t> buffer;
    
    auto now = std::chrono::steady_clock::now();
    auto epoch = now.time_since_epoch();
    std::uint64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count();
    
    for (const auto& msg : msgs) {
        // SOFH header
        SOFH sofh;
        sofh.message_length = htobe32(static_cast<std::uint32_t>(sizeof(SOFH) + sizeof(ITCHMsg)));
        sofh.encoding_type = htobe16(0x4954);  // 'IT' for ITCH
        
        // ITCH message
        ITCHMsg itch_msg;
        itch_msg.msg_type = 'Q';  // Quote message
        itch_msg.timestamp = htobe64(timestamp_ns);
        itch_msg.seq = htobe64(msg.seq);
        itch_msg.price = htobe32(static_cast<std::uint32_t>(msg.px * 10000));  // Scale to 4 decimal places
        itch_msg.qty = htobe32(static_cast<std::uint32_t>(std::abs(msg.qty)));
        itch_msg.side = (msg.qty > 0) ? 'B' : 'S';
        
        // Append to buffer
        const std::uint8_t* sofh_ptr = reinterpret_cast<const std::uint8_t*>(&sofh);
        buffer.insert(buffer.end(), sofh_ptr, sofh_ptr + sizeof(SOFH));
        
        const std::uint8_t* itch_ptr = reinterpret_cast<const std::uint8_t*>(&itch_msg);
        buffer.insert(buffer.end(), itch_ptr, itch_ptr + sizeof(ITCHMsg));
    }
    
    return buffer;
}

// Function declarations
void run_tcp_session(boost::asio::ip::tcp::socket sock, SimCfg cfg);
void run_udp_multicast(SimCfg cfg);
void run_simulator(SimCfg cfg);

} // namespace mdfh