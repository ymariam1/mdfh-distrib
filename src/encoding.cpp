#include "mdfh/encoding.hpp"
#include "mdfh/timing.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <memory>

namespace mdfh {

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
    
    for (const auto& msg : msgs) {
        std::ostringstream fix_msg;
        
        // FIX 4.4 Market Data Incremental Refresh
        fix_msg << "8=FIX.4.4\x01";  // BeginString
        fix_msg << "35=X\x01";       // MsgType (Market Data Incremental Refresh)
        fix_msg << "49=" << config_.sender_comp_id << "\x01";  // SenderCompID
        fix_msg << "56=" << config_.target_comp_id << "\x01";  // TargetCompID
        fix_msg << "34=" << msg.seq << "\x01";                 // MsgSeqNum
        
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
        std::uint8_t checksum = calculate_checksum(fix_body);
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