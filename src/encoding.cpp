#include "mdfh/encoding.hpp"
#include "mdfh/timing.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <memory>
#include <ctime>

namespace mdfh {

// Fast integer to string conversion
static char* fast_uint_to_chars(std::uint64_t value, char* buffer) {
    if (value == 0) {
        *buffer++ = '0';
        return buffer;
    }
    
    char temp[20]; // Enough for 64-bit integer
    char* p = temp;
    
    while (value > 0) {
        *p++ = '0' + (value % 10);
        value /= 10;
    }
    
    // Reverse the digits
    while (p > temp) {
        *buffer++ = *--p;
    }
    
    return buffer;
}

// Fast double to string conversion for prices (4 decimal places)
static char* fast_price_to_chars(double price, char* buffer) {
    // Scale to 4 decimal places and convert to integer
    auto scaled = static_cast<std::uint64_t>(price * 10000 + 0.5);
    
    auto integer_part = scaled / 10000;
    auto fractional_part = scaled % 10000;
    
    // Write integer part
    buffer = fast_uint_to_chars(integer_part, buffer);
    
    // Write decimal point
    *buffer++ = '.';
    
    // Write fractional part with leading zeros
    *buffer++ = '0' + (fractional_part / 1000);
    *buffer++ = '0' + ((fractional_part / 100) % 10);
    *buffer++ = '0' + ((fractional_part / 10) % 10);
    *buffer++ = '0' + (fractional_part % 10);
    
    return buffer;
}

// Fast timestamp formatting (cached per second)
static thread_local char cached_timestamp[32];
static thread_local std::time_t cached_time = 0;

static const char* get_fast_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    
    if (time_t_now != cached_time) {
        cached_time = time_t_now;
        auto tm = *std::gmtime(&time_t_now);
        std::snprintf(cached_timestamp, sizeof(cached_timestamp), 
                     "%04d%02d%02d-%02d:%02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    
    return cached_timestamp;
}

// Binary encoder implementation
std::vector<std::uint8_t> BinaryEncoder::encode(const std::vector<Msg>& msgs) {
    std::vector<std::uint8_t> buffer;
    encode_inplace(msgs, buffer);
    return buffer;
}

void BinaryEncoder::encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) {
    buffer.resize(msgs.size() * sizeof(Msg));
    std::memcpy(buffer.data(), msgs.data(), buffer.size());
}

// FIX encoder implementation
std::uint8_t FIXEncoder::calculate_checksum(const std::string& msg) const {
    std::uint32_t sum = 0;
    for (char c : msg) {
        sum += static_cast<std::uint8_t>(c);
    }
    return static_cast<std::uint8_t>(sum % 256);
}

std::vector<std::uint8_t> FIXEncoder::encode(const std::vector<Msg>& msgs) {
    std::vector<std::uint8_t> buffer;
    encode_inplace(msgs, buffer);
    return buffer;
}

void FIXEncoder::encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) {
    buffer.clear();
    
    // Pre-allocate buffer to avoid reallocations
    buffer.reserve(msgs.size() * 150); // Estimate ~150 bytes per FIX message
    
    // Thread-local buffer for fast FIX message construction
    static thread_local char fix_buffer[512];
    
    for (const auto& msg : msgs) {
        char* p = fix_buffer;
        
        // FIX 4.4 Market Data Incremental Refresh
        std::memcpy(p, "8=FIX.4.4\x01", 10);
        p += 10;
        
        // Build body content first to calculate length
        char* body_start = p + 20; // Reserve space for body length field
        char* body_p = body_start;
        
        // MsgType
        std::memcpy(body_p, "35=X\x01", 5);
        body_p += 5;
        
        // SenderCompID
        std::memcpy(body_p, "49=", 3);
        body_p += 3;
        std::memcpy(body_p, config_.sender_comp_id.c_str(), config_.sender_comp_id.length());
        body_p += config_.sender_comp_id.length();
        *body_p++ = '\x01';
        
        // TargetCompID
        std::memcpy(body_p, "56=", 3);
        body_p += 3;
        std::memcpy(body_p, config_.target_comp_id.c_str(), config_.target_comp_id.length());
        body_p += config_.target_comp_id.length();
        *body_p++ = '\x01';
        
        // MsgSeqNum
        std::memcpy(body_p, "34=", 3);
        body_p += 3;
        body_p = fast_uint_to_chars(msg.seq, body_p);
        *body_p++ = '\x01';
        
        // Timestamp (cached per second)
        const char* timestamp = get_fast_timestamp();
        std::memcpy(body_p, "52=", 3);
        body_p += 3;
        auto ts_len = std::strlen(timestamp);
        std::memcpy(body_p, timestamp, ts_len);
        body_p += ts_len;
        *body_p++ = '\x01';
        
        // Market data entries
        std::memcpy(body_p, "268=1\x01", 6);
        body_p += 6;
        std::memcpy(body_p, "279=0\x01", 6);
        body_p += 6;
        
        // MDEntryType (Bid/Offer)
        std::memcpy(body_p, "269=", 4);
        body_p += 4;
        *body_p++ = (msg.qty > 0) ? '0' : '1';
        *body_p++ = '\x01';
        
        // MDEntryPx with fast price formatting
        std::memcpy(body_p, "270=", 4);
        body_p += 4;
        body_p = fast_price_to_chars(msg.px, body_p);
        *body_p++ = '\x01';
        
        // MDEntrySize
        std::memcpy(body_p, "271=", 4);
        body_p += 4;
        body_p = fast_uint_to_chars(std::abs(msg.qty), body_p);
        *body_p++ = '\x01';
        
        // Calculate body length and insert it
        auto body_length = body_p - body_start;
        std::memcpy(p, "9=", 2);
        p += 2;
        p = fast_uint_to_chars(body_length, p);
        *p++ = '\x01';
        
        // Move body to correct position if needed
        if (p != body_start) {
            std::memmove(p, body_start, body_length);
            body_p = p + body_length;
        }
        
        // Calculate checksum
        std::uint32_t checksum = 0;
        for (char* cp = fix_buffer; cp < body_p; ++cp) {
            checksum += static_cast<std::uint8_t>(*cp);
        }
        checksum %= 256;
        
        // Add checksum
        std::memcpy(body_p, "10=", 3);
        body_p += 3;
        if (checksum < 10) {
            *body_p++ = '0';
            *body_p++ = '0';
            *body_p++ = '0' + checksum;
        } else if (checksum < 100) {
            *body_p++ = '0';
            *body_p++ = '0' + (checksum / 10);
            *body_p++ = '0' + (checksum % 10);
        } else {
            *body_p++ = '0' + (checksum / 100);
            *body_p++ = '0' + ((checksum / 10) % 10);
            *body_p++ = '0' + (checksum % 10);
        }
        *body_p++ = '\x01';
        
        auto complete_msg_len = body_p - fix_buffer;
        
        // Add SOFH header
        SOFH sofh;
        sofh.message_length = htobe32(static_cast<std::uint32_t>(sizeof(SOFH) + complete_msg_len));
        sofh.encoding_type = htobe16(0x5000);  // FIX encoding type
        
        // Append to buffer
        const std::uint8_t* sofh_ptr = reinterpret_cast<const std::uint8_t*>(&sofh);
        buffer.insert(buffer.end(), sofh_ptr, sofh_ptr + sizeof(SOFH));
        buffer.insert(buffer.end(), reinterpret_cast<const std::uint8_t*>(fix_buffer), 
                     reinterpret_cast<const std::uint8_t*>(fix_buffer) + complete_msg_len);
    }
}

// ITCH encoder implementation
std::vector<std::uint8_t> ITCHEncoder::encode(const std::vector<Msg>& msgs) {
    std::vector<std::uint8_t> buffer;
    encode_inplace(msgs, buffer);
    return buffer;
}

void ITCHEncoder::encode_inplace(const std::vector<Msg>& msgs, std::vector<std::uint8_t>& buffer) {
    buffer.clear();
    
    std::uint64_t timestamp_ns = get_timestamp_ns();
    
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
}

// Factory function
std::unique_ptr<MessageEncoder> create_encoder(EncodingType type, const EncodingConfig& config) {
    switch (type) {
        case EncodingType::BINARY:
            return std::make_unique<BinaryEncoder>();
        case EncodingType::FIX:
            return std::make_unique<FIXEncoder>(config);
        case EncodingType::ITCH:
            return std::make_unique<ITCHEncoder>();
    }
    throw std::invalid_argument("Unknown encoding type");
}

} // namespace mdfh 