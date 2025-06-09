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
    
    for (const auto& msg : msgs) {
        thread_local char fix_buffer[512]; // Thread-local buffer for building messages
        char* p = fix_buffer;
        
        // FIX 4.4 Market Data Incremental Refresh
        const char* begin_string = "8=FIX.4.4\x01";
        std::memcpy(p, begin_string, 10);
        p += 10;
        
        // Reserve space for body length (we'll fill this in later)
        char* body_length_pos = p;
        const char* body_length_prefix = "9=";
        std::memcpy(p, body_length_prefix, 2);
        p += 2;
        char* body_length_value_pos = p;
        p += 10; // Reserve space for body length value
        *p++ = '\x01';
        
        char* body_start = p;
        
        // MsgType (Market Data Incremental Refresh)
        const char* msg_type = "35=X\x01";
        std::memcpy(p, msg_type, 5);
        p += 5;
        
        // SenderCompID
        const char* sender_prefix = "49=";
        std::memcpy(p, sender_prefix, 3);
        p += 3;
        std::memcpy(p, config_.sender_comp_id.c_str(), config_.sender_comp_id.length());
        p += config_.sender_comp_id.length();
        *p++ = '\x01';
        
        // TargetCompID
        const char* target_prefix = "56=";
        std::memcpy(p, target_prefix, 3);
        p += 3;
        std::memcpy(p, config_.target_comp_id.c_str(), config_.target_comp_id.length());
        p += config_.target_comp_id.length();
        *p++ = '\x01';
        
        // MsgSeqNum
        const char* seq_prefix = "34=";
        std::memcpy(p, seq_prefix, 3);
        p += 3;
        p = fast_uint_to_chars(msg.seq, p);
        *p++ = '\x01';
        
        // Timestamp (cached per second)
        const char* time_prefix = "52=";
        std::memcpy(p, time_prefix, 3);
        p += 3;
        const char* timestamp = get_fast_timestamp();
        std::memcpy(p, timestamp, 17); // YYYYMMDD-HH:MM:SS
        p += 17;
        *p++ = '\x01';
        
        // Market data entries
        const char* md_entries = "268=1\x01";
        std::memcpy(p, md_entries, 7);
        p += 7;
        
        const char* md_action = "279=0\x01";
        std::memcpy(p, md_action, 7);
        p += 7;
        
        // MDEntryType (Bid/Offer)
        const char* entry_type_prefix = "269=";
        std::memcpy(p, entry_type_prefix, 4);
        p += 4;
        *p++ = (msg.qty > 0) ? '0' : '1';
        *p++ = '\x01';
        
        // MDEntryPx
        const char* price_prefix = "270=";
        std::memcpy(p, price_prefix, 4);
        p += 4;
        p = fast_price_to_chars(msg.px, p);
        *p++ = '\x01';
        
        // MDEntrySize
        const char* size_prefix = "271=";
        std::memcpy(p, size_prefix, 4);
        p += 4;
        p = fast_uint_to_chars(std::abs(msg.qty), p);
        *p++ = '\x01';
        
        // Calculate body length
        std::size_t body_length = p - body_start;
        
        // Fill in body length
        char* body_length_end = fast_uint_to_chars(body_length, body_length_value_pos);
        
        // Shift the body if needed to accommodate the actual body length
        std::size_t body_length_digits = body_length_end - body_length_value_pos;
        std::size_t reserved_digits = 10;
        if (body_length_digits < reserved_digits) {
            std::size_t shift = reserved_digits - body_length_digits;
            std::memmove(body_start - shift, body_start, body_length);
            p -= shift;
            body_length_end = body_length_value_pos + body_length_digits;
            *body_length_end = '\x01';
        }
        
        // Calculate and append checksum
        std::uint32_t checksum = 0;
        for (char* c = fix_buffer; c < p; ++c) {
            checksum += static_cast<std::uint8_t>(*c);
        }
        checksum %= 256;
        
        const char* checksum_prefix = "10=";
        std::memcpy(p, checksum_prefix, 3);
        p += 3;
        
        // Format checksum with leading zeros
        if (checksum < 100) *p++ = '0';
        if (checksum < 10) *p++ = '0';
        p = fast_uint_to_chars(checksum, p);
        *p++ = '\x01';
        
        // Add SOFH header
        SOFH sofh;
        std::size_t fix_message_length = p - fix_buffer;
        sofh.message_length = htobe32(static_cast<std::uint32_t>(sizeof(SOFH) + fix_message_length));
        sofh.encoding_type = htobe16(0x5000);  // FIX encoding type
        
        // Append to buffer
        const std::uint8_t* sofh_ptr = reinterpret_cast<const std::uint8_t*>(&sofh);
        buffer.insert(buffer.end(), sofh_ptr, sofh_ptr + sizeof(SOFH));
        buffer.insert(buffer.end(), reinterpret_cast<std::uint8_t*>(fix_buffer), 
                     reinterpret_cast<std::uint8_t*>(p));
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