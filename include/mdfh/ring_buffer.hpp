#pragma once

#include "core.hpp"
#include <vector>
#include <atomic>
#include <stdexcept>
#include <memory>

namespace mdfh {

/**
 * @brief Slot structure for ring buffer entries
 * 
 * Each slot is cache-line aligned to prevent false sharing between producer 
 * and consumer threads. This is critical for achieving optimal performance 
 * in high-frequency trading scenarios.
 * 
 * @note The padding ensures optimal cache alignment across platforms
 */
struct alignas(64) Slot {
    Msg raw;                    ///< The parsed message (20 bytes)
    std::uint64_t rx_ts;        ///< Receive timestamp in nanoseconds (8 bytes)
    
    // Padding to fill the rest of the cache line
    // On some platforms (Apple Silicon), alignas(64) may round up to 128 bytes
    // This is still cache-line aligned and provides good performance
    char padding[64 - sizeof(Msg) - sizeof(std::uint64_t)];
    
    /**
     * @brief Default constructor
     */
    Slot() : raw(), rx_ts(0) {}
    
    /**
     * @brief Constructor with message and timestamp
     * @param message The market data message
     * @param timestamp Receive timestamp in nanoseconds
     */
    Slot(const Msg& message, std::uint64_t timestamp) 
        : raw(message), rx_ts(timestamp) {}
    
    /**
     * @brief Validates the slot data
     * @return true if the slot contains valid data
     */
    bool is_valid() const {
        return raw.is_valid() && rx_ts > 0;
    }
};

// Ensure slot is properly aligned for cache performance
static_assert(alignof(Slot) == 64, "Slot must be 64-byte aligned");
static_assert(sizeof(Slot) >= 64, "Slot must be at least 64 bytes");
static_assert(sizeof(Slot) <= 128, "Slot should not exceed 128 bytes");

/**
 * @brief High-performance single-producer/single-consumer lock-free ring buffer
 * 
 * This ring buffer is optimized for ultra-low latency market data ingestion.
 * Key features:
 * - Lock-free operations using atomic operations
 * - Cache-line aligned slots to prevent false sharing
 * - Memory prefetching for improved performance
 * - Bulk operations for batch processing
 * - Back-pressure handling modes
 * 
 * @note This implementation assumes single producer and single consumer threads
 * @note Capacity must be a power of 2 for optimal performance
 */
class RingBuffer {
public:
    /**
     * @brief Back-pressure handling modes
     */
    enum class BackPressureMode {
        DROP,    ///< Drop messages when buffer is full (default)
        BLOCK    ///< Block until space is available
    };

private:
    std::vector<Slot> slots_;
    alignas(64) std::atomic<std::uint64_t> write_pos_{0};
    alignas(64) std::atomic<std::uint64_t> read_pos_{0};
    alignas(64) std::atomic<std::uint64_t> high_water_mark_{0};
    std::uint64_t capacity_;
    std::uint64_t mask_;

public:
    /**
     * @brief Constructs a ring buffer with the specified capacity
     * @param capacity Buffer capacity (must be a power of 2)
     * @throws std::invalid_argument if capacity is not a power of 2
     */
    explicit RingBuffer(std::uint64_t capacity);
    
    /**
     * @brief Destructor
     */
    ~RingBuffer() = default;
    
    // Non-copyable and non-movable for safety
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;
    
    /**
     * @brief Attempts to push a slot to the buffer (non-blocking)
     * @param slot The slot to push
     * @return true if successful, false if buffer is full
     * @note Thread-safe for single producer
     */
    bool try_push(const Slot& slot);
    
    /**
     * @brief Attempts to pop a slot from the buffer (non-blocking)
     * @param slot Reference to store the popped slot
     * @return true if successful, false if buffer is empty
     * @note Thread-safe for single consumer
     */
    bool try_pop(Slot& slot);
    
    /**
     * @brief High-performance push with memory prefetching
     * @param slot The slot to push
     * @return true if successful, false if buffer is full
     * @note Prefetches the next slot location for better cache performance
     */
    bool try_push_with_prefetch(const Slot& slot);
    
    /**
     * @brief High-performance pop with memory prefetching
     * @param slot Reference to store the popped slot
     * @return true if successful, false if buffer is empty
     * @note Prefetches the next slot location for better cache performance
     */
    bool try_pop_with_prefetch(Slot& slot);
    
    /**
     * @brief Bulk push operation for batch processing
     * @param slots Array of slots to push
     * @param count Number of slots to push
     * @return Number of slots actually pushed
     * @note More efficient than individual pushes for large batches
     */
    std::uint64_t try_push_bulk(const Slot* slots, std::uint64_t count);
    
    /**
     * @brief Bulk pop operation for batch processing
     * @param slots Array to store popped slots
     * @param max_count Maximum number of slots to pop
     * @return Number of slots actually popped
     * @note More efficient than individual pops for large batches
     */
    std::uint64_t try_pop_bulk(Slot* slots, std::uint64_t max_count);
    
    /**
     * @brief Push with back-pressure handling
     * @param slot The slot to push
     * @param timeout_ns Timeout in nanoseconds (0 = no timeout)
     * @param mode Back-pressure handling mode
     * @return true if successful, false if timeout or drop
     */
    bool try_push_or_block(const Slot& slot, std::uint64_t timeout_ns = 0, 
                          BackPressureMode mode = BackPressureMode::DROP);
    
    /**
     * @brief Gets the current number of items in the buffer
     * @return Current buffer size
     * @note This is an approximation due to concurrent access
     */
    std::uint64_t size() const;
    
    /**
     * @brief Gets the high water mark (maximum size reached)
     * @return High water mark value
     */
    std::uint64_t high_water_mark() const;
    
    /**
     * @brief Gets the buffer capacity
     * @return Buffer capacity
     */
    std::uint64_t capacity() const { return capacity_; }
    
    /**
     * @brief Checks if the buffer is empty
     * @return true if empty, false otherwise
     */
    bool empty() const { return size() == 0; }
    
    /**
     * @brief Checks if the buffer is full
     * @return true if full, false otherwise
     */
    bool full() const { return size() >= capacity_; }
    
    /**
     * @brief Gets the load factor (size / capacity)
     * @return Load factor as a percentage (0.0 to 1.0)
     */
    double load_factor() const { 
        return static_cast<double>(size()) / static_cast<double>(capacity_); 
    }
    
    // Zero-copy access for high-performance scenarios
    // WARNING: These methods are not thread-safe and should only be used
    // by the producer thread with careful synchronization
    
    /**
     * @brief Gets direct access to the slot array (UNSAFE)
     * @return Pointer to the slot array
     * @warning Only use this for zero-copy scenarios with proper synchronization
     */
    Slot* slots() { return slots_.data(); }
    
    /**
     * @brief Gets the current write position (UNSAFE)
     * @return Current write position
     * @warning Only use this for zero-copy scenarios with proper synchronization
     */
    std::uint64_t write_pos() const { return write_pos_.load(std::memory_order_relaxed); }
    
    /**
     * @brief Gets the current read position (UNSAFE)
     * @return Current read position
     * @warning Only use this for zero-copy scenarios with proper synchronization
     */
    std::uint64_t read_pos() const { return read_pos_.load(std::memory_order_relaxed); }
    
    /**
     * @brief Gets the index mask for wrapping
     * @return Index mask (capacity - 1)
     */
    std::uint64_t mask() const { return mask_; }
    
    /**
     * @brief Advances the write position by count (UNSAFE)
     * @param count Number of positions to advance
     * @warning Only use this for zero-copy scenarios with proper synchronization
     */
    void advance_write_pos(std::uint64_t count);

private:
    /**
     * @brief Validates the capacity parameter
     * @param capacity The capacity to validate
     * @throws std::invalid_argument if capacity is invalid
     */
    void validate_capacity(std::uint64_t capacity) const;
};

/**
 * @brief Creates a ring buffer with validated capacity
 * @param capacity Buffer capacity (must be power of 2)
 * @return Unique pointer to the ring buffer
 * @throws std::invalid_argument if capacity is invalid
 */
std::unique_ptr<RingBuffer> make_ring_buffer(std::uint64_t capacity);

} // namespace mdfh 