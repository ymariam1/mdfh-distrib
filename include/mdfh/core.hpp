#pragma once

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace mdfh {

/**
 * @brief Logging levels for the MDFH system
 */
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    FATAL = 4
};

/**
 * @brief Simple thread-safe logger for MDFH
 * 
 * Provides structured logging with timestamps and log levels.
 * Thread-safe for concurrent use across multiple threads.
 */
class Logger {
private:
    static LogLevel current_level_;
    static std::ostream* output_stream_;
    
public:
    /**
     * @brief Sets the global log level
     * @param level Minimum log level to output
     */
    static void set_log_level(LogLevel level) { current_level_ = level; }
    
    /**
     * @brief Sets the output stream for logging
     * @param stream Output stream (default: std::cerr)
     */
    static void set_output_stream(std::ostream& stream) { output_stream_ = &stream; }
    
    /**
     * @brief Logs a message at the specified level
     * @param level Log level
     * @param component Component name (e.g., "RingBuffer", "KernelBypass")
     * @param message Log message
     */
    static void log(LogLevel level, const std::string& component, const std::string& message);
    
    /**
     * @brief Convenience methods for different log levels
     */
    static void debug(const std::string& component, const std::string& message) {
        log(LogLevel::DEBUG, component, message);
    }
    
    static void info(const std::string& component, const std::string& message) {
        log(LogLevel::INFO, component, message);
    }
    
    static void warn(const std::string& component, const std::string& message) {
        log(LogLevel::WARN, component, message);
    }
    
    static void error(const std::string& component, const std::string& message) {
        log(LogLevel::ERROR, component, message);
    }
    
    static void fatal(const std::string& component, const std::string& message) {
        log(LogLevel::FATAL, component, message);
    }

private:
    static std::string get_timestamp();
    static std::string level_to_string(LogLevel level);
};

// Convenience macros for logging
#define MDFH_LOG_DEBUG(component, message) mdfh::Logger::debug(component, message)
#define MDFH_LOG_INFO(component, message) mdfh::Logger::info(component, message)
#define MDFH_LOG_WARN(component, message) mdfh::Logger::warn(component, message)
#define MDFH_LOG_ERROR(component, message) mdfh::Logger::error(component, message)
#define MDFH_LOG_FATAL(component, message) mdfh::Logger::fatal(component, message)

/**
 * @brief Core message structure - the fundamental unit of market data
 * 
 * This structure represents a single market data message with sequence number,
 * price, and quantity. It's designed to be exactly 20 bytes for optimal
 * memory layout and network transmission.
 * 
 * @note The structure is packed to ensure consistent layout across platforms
 * @note All fields use little-endian byte order for consistency
 */
#pragma pack(push, 1)
struct Msg {
    std::uint64_t seq;          ///< Monotonically increasing sequence number
    double        px;           ///< Price (IEEE-754 double precision, little-endian)
    std::int32_t  qty;          ///< Quantity (positive = buy, negative = sell)
    
    /**
     * @brief Default constructor - initializes all fields to zero
     */
    Msg() : seq(0), px(0.0), qty(0) {}
    
    /**
     * @brief Parameterized constructor
     * @param sequence Sequence number
     * @param price Price value
     * @param quantity Quantity (positive for buy, negative for sell)
     */
    Msg(std::uint64_t sequence, double price, std::int32_t quantity)
        : seq(sequence), px(price), qty(quantity) {}
    
    /**
     * @brief Validates the message fields
     * @return true if the message is valid, false otherwise
     */
    bool is_valid() const {
        return seq > 0 && px > 0.0 && qty != 0;
    }
    
    /**
     * @brief Returns the side of the trade
     * @return 'B' for buy (positive qty), 'S' for sell (negative qty), 'U' for unknown
     */
    char side() const {
        if (qty > 0) return 'B';
        if (qty < 0) return 'S';
        return 'U';
    }
    
    /**
     * @brief Returns the absolute quantity
     * @return Absolute value of quantity
     */
    std::uint32_t abs_qty() const {
        return static_cast<std::uint32_t>(qty >= 0 ? qty : -qty);
    }
};
#pragma pack(pop)
static_assert(sizeof(Msg) == 20, "Msg struct size must be exactly 20 bytes");

/**
 * @brief Transport layer types for network communication
 */
enum class TransportType {
    TCP,            ///< TCP unicast connection
    UDP_MULTICAST   ///< UDP multicast for high-throughput feeds
};

/**
 * @brief Message encoding formats supported by the system
 */
enum class EncodingType {
    BINARY,     ///< Raw binary format (fastest)
    FIX,        ///< FIX protocol (Financial Information eXchange)
    ITCH        ///< ITCH protocol (Information Technology Communication Hub)
};

/**
 * @brief Exception class for MDFH-specific errors
 */
class MDFHException : public std::runtime_error {
public:
    explicit MDFHException(const std::string& message) 
        : std::runtime_error("MDFH Error: " + message) {}
};

/**
 * @brief Exception for configuration-related errors
 */
class ConfigurationError : public MDFHException {
public:
    explicit ConfigurationError(const std::string& message)
        : MDFHException("Configuration Error: " + message) {}
};

/**
 * @brief Exception for network-related errors
 */
class NetworkError : public MDFHException {
public:
    explicit NetworkError(const std::string& message)
        : MDFHException("Network Error: " + message) {}
};

/**
 * @brief Exception for performance-related errors
 */
class PerformanceError : public MDFHException {
public:
    explicit PerformanceError(const std::string& message)
        : MDFHException("Performance Error: " + message) {}
};

/**
 * @brief Validates a transport type
 * @param transport The transport type to validate
 * @return true if valid, false otherwise
 */
inline bool is_valid_transport(TransportType transport) {
    return transport == TransportType::TCP || transport == TransportType::UDP_MULTICAST;
}

/**
 * @brief Validates an encoding type
 * @param encoding The encoding type to validate
 * @return true if valid, false otherwise
 */
inline bool is_valid_encoding(EncodingType encoding) {
    return encoding == EncodingType::BINARY || 
           encoding == EncodingType::FIX || 
           encoding == EncodingType::ITCH;
}

/**
 * @brief Validates a port number
 * @param port The port number to validate
 * @return true if valid (1-65535), false otherwise
 */
inline bool is_valid_port(std::uint16_t port) {
    return port > 0 && port <= 65535;
}

/**
 * @brief Validates that a value is a power of 2
 * @param value The value to check
 * @return true if value is a power of 2, false otherwise
 */
inline bool is_power_of_two(std::uint64_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

// Output stream operators for better debugging/logging
inline std::ostream& operator<<(std::ostream& os, TransportType transport) {
    switch (transport) {
        case TransportType::TCP: return os << "TCP";
        case TransportType::UDP_MULTICAST: return os << "UDP_MULTICAST";
    }
    return os << "UNKNOWN_TRANSPORT";
}

inline std::ostream& operator<<(std::ostream& os, EncodingType encoding) {
    switch (encoding) {
        case EncodingType::BINARY: return os << "BINARY";
        case EncodingType::FIX: return os << "FIX";
        case EncodingType::ITCH: return os << "ITCH";
    }
    return os << "UNKNOWN_ENCODING";
}

inline std::ostream& operator<<(std::ostream& os, LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return os << "DEBUG";
        case LogLevel::INFO: return os << "INFO";
        case LogLevel::WARN: return os << "WARN";
        case LogLevel::ERROR: return os << "ERROR";
        case LogLevel::FATAL: return os << "FATAL";
    }
    return os << "UNKNOWN_LEVEL";
}

inline std::ostream& operator<<(std::ostream& os, const Msg& msg) {
    return os << "Msg{seq=" << msg.seq << ", px=" << msg.px 
              << ", qty=" << msg.qty << ", side=" << msg.side() << "}";
}

} // namespace mdfh 