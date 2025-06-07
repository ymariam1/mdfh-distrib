#pragma once

#include "core.hpp"
#include <vector>
#include <cstring>
#include <string>

// Platform-specific endianness handling
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

// SOFH (Simple Open Framing Header) for FIX/ITCH protocols
#pragma pack(push, 1)
struct SOFH {
    std::uint32_t message_length;   // Big-endian, includes SOFH
    std::uint16_t encoding_type;    // Big-endian, 0x5000 for FIX, 0x4954 for ITCH
};
#pragma pack(pop)
static_assert(sizeof(SOFH) == 6, "SOFH must be exactly 6 bytes");

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
static_assert(sizeof(ITCHMsg) == 26, "ITCHMsg must be exactly 26 bytes");

// Configuration for encoding operations
struct EncodingConfig {
    std::string sender_comp_id = "MDFH_SIM";   // FIX SenderCompID
    std::string target_comp_id = "CLIENT";     // FIX TargetCompID
};

// Message encoder interface - provides a clean abstraction for different encoding formats
class MessageEncoder {
public:
    virtual ~MessageEncoder() = default;
    virtual std::vector<std::uint8_t> encode(const std::vector<Msg>& msgs) = 0;
    virtual void encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) = 0;
};

// Binary encoder - fastest, direct memory copy
class BinaryEncoder : public MessageEncoder {
public:
    std::vector<std::uint8_t> encode(const std::vector<Msg>& msgs) override;
    void encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) override;
};

// FIX encoder - Financial Information eXchange protocol
class FIXEncoder : public MessageEncoder {
private:
    EncodingConfig config_;
    
public:
    explicit FIXEncoder(EncodingConfig config = {}) : config_(std::move(config)) {}
    std::vector<std::uint8_t> encode(const std::vector<Msg>& msgs) override;
    void encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) override;
    
private:
    std::uint8_t calculate_checksum(const std::string& msg) const;
};

// ITCH encoder - Information Technology Communication Hub protocol
class ITCHEncoder : public MessageEncoder {
public:
    std::vector<std::uint8_t> encode(const std::vector<Msg>& msgs) override;
    void encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) override;
};

// Factory function to create appropriate encoder
std::unique_ptr<MessageEncoder> create_encoder(EncodingType type, const EncodingConfig& config = {});

} // namespace mdfh 