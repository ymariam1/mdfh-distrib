#pragma once

#include "core.hpp"
#include "ring_buffer.hpp"
#include "timing.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>

namespace mdfh {

// Forward declarations
class IngestionStats;
class MessageParser;

// Kernel bypass networking backend types
enum class BypassBackend {
    BOOST_ASIO,     // Standard kernel networking (fallback)
    DPDK,           // Intel DPDK for high performance
    SOLARFLARE_VI   // Solarflare ef_vi for ultra-low latency
};

// Configuration for kernel bypass networking
struct BypassConfig {
    BypassBackend backend = BypassBackend::BOOST_ASIO;
    
    // Network settings
    std::string interface_name = "eth0";       // Network interface
    std::string host = "127.0.0.1";           // Host address
    std::uint16_t port = 9001;                // Port number
    
    // Performance settings
    std::uint32_t rx_ring_size = 2048;        // RX ring buffer size (power of 2)
    std::uint32_t batch_size = 32;            // Packet batch processing size
    std::uint32_t cpu_core = 0;               // CPU core to pin threads to
    bool enable_numa_awareness = true;        // NUMA-aware allocations
    
    // Zero-copy settings
    bool enable_zero_copy = true;             // Enable zero-copy reception
    std::uint32_t zero_copy_threshold = 64;   // Minimum packet size for zero-copy
    
    // Timeout settings
    std::uint32_t poll_timeout_us = 100;      // Polling timeout in microseconds
    
    // Validation
    bool is_valid() const;
};

// Packet descriptor for zero-copy reception
struct PacketDesc {
    const std::uint8_t* data;                 // Packet data pointer
    std::size_t length;                       // Packet length
    std::uint64_t timestamp_ns;               // Hardware timestamp (if available)
    void* context;                            // Backend-specific context for packet release
    
    PacketDesc() : data(nullptr), length(0), timestamp_ns(0), context(nullptr) {}
    PacketDesc(const std::uint8_t* d, std::size_t len, std::uint64_t ts = 0, void* ctx = nullptr)
        : data(d), length(len), timestamp_ns(ts), context(ctx) {}
};

// Callback for packet reception
using PacketHandler = std::function<void(const PacketDesc& packet)>;

// Abstract base class for kernel bypass networking
class KernelBypassClient {
public:
    virtual ~KernelBypassClient() = default;
    
    // Configuration and lifecycle
    virtual bool initialize(const BypassConfig& config) = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    
    // Packet reception
    virtual void start_reception(PacketHandler handler) = 0;
    virtual void stop_reception() = 0;
    
    // Zero-copy packet management
    virtual void release_packet(void* context) = 0;
    
    // Statistics and monitoring
    virtual std::uint64_t packets_received() const = 0;
    virtual std::uint64_t bytes_received() const = 0;
    virtual std::uint64_t packets_dropped() const = 0;
    virtual double cpu_utilization() const = 0;
    
    // Backend information
    virtual BypassBackend backend_type() const = 0;
    virtual std::string backend_info() const = 0;
};

// Factory function for creating kernel bypass clients
std::unique_ptr<KernelBypassClient> create_bypass_client(BypassBackend backend);

// Boost.Asio fallback implementation
class BoostAsioBypassClient : public KernelBypassClient {
private:
    BypassConfig config_;
    std::unique_ptr<class NetworkClient> asio_client_;
    std::atomic<bool> running_{false};
    std::thread reception_thread_;
    PacketHandler packet_handler_;
    
    // Statistics
    std::atomic<std::uint64_t> packets_received_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> packets_dropped_{0};
    
public:
    BoostAsioBypassClient();
    ~BoostAsioBypassClient() override;
    
    // KernelBypassClient interface
    bool initialize(const BypassConfig& config) override;
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;
    
    void start_reception(PacketHandler handler) override;
    void stop_reception() override;
    
    void release_packet(void* context) override;
    
    std::uint64_t packets_received() const override { return packets_received_.load(); }
    std::uint64_t bytes_received() const override { return bytes_received_.load(); }
    std::uint64_t packets_dropped() const override { return packets_dropped_.load(); }
    double cpu_utilization() const override { return 0.0; }
    
    BypassBackend backend_type() const override { return BypassBackend::BOOST_ASIO; }
    std::string backend_info() const override { return "Boost.Asio (kernel networking)"; }
    
private:
    void reception_loop();
};

#ifdef MDFH_ENABLE_DPDK
// DPDK implementation for Intel NICs
class DPDKBypassClient : public KernelBypassClient {
private:
    BypassConfig config_;
    std::uint16_t port_id_{0};
    std::uint16_t queue_id_{0};
    bool initialized_{false};
    std::atomic<bool> running_{false};
    std::thread reception_thread_;
    PacketHandler packet_handler_;
    
    // Statistics
    std::atomic<std::uint64_t> packets_received_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> packets_dropped_{0};
    
    // Memory pools
    struct rte_mempool* mbuf_pool_{nullptr};
    
public:
    DPDKBypassClient();
    ~DPDKBypassClient() override;
    
    // KernelBypassClient interface
    bool initialize(const BypassConfig& config) override;
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override { return initialized_ && !running_.load(); }
    
    void start_reception(PacketHandler handler) override;
    void stop_reception() override;
    
    void release_packet(void* context) override;
    
    std::uint64_t packets_received() const override { return packets_received_.load(); }
    std::uint64_t bytes_received() const override { return bytes_received_.load(); }
    std::uint64_t packets_dropped() const override { return packets_dropped_.load(); }
    double cpu_utilization() const override;
    
    BypassBackend backend_type() const override { return BypassBackend::DPDK; }
    std::string backend_info() const override;
    
private:
    bool setup_dpdk_eal();
    bool setup_ethernet_port();
    bool setup_memory_pools();
    void reception_loop();
    void process_packet_batch(struct rte_mbuf** packets, std::uint16_t count);
    
    static bool dpdk_initialized_;
    static std::mutex dpdk_init_mutex_;
};
#endif // MDFH_ENABLE_DPDK

#ifdef MDFH_ENABLE_SOLARFLARE
// Solarflare ef_vi implementation for ultra-low latency
class SolarflareBypassClient : public KernelBypassClient {
private:
    BypassConfig config_;
    struct ef_vi* vi_{nullptr};
    struct ef_pd* pd_{nullptr};
    struct ef_memreg* memreg_{nullptr};
    void* packet_buffer_{nullptr};
    std::size_t buffer_size_{0};
    bool initialized_{false};
    std::atomic<bool> running_{false};
    std::thread reception_thread_;
    PacketHandler packet_handler_;
    
    // Statistics
    std::atomic<std::uint64_t> packets_received_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::atomic<std::uint64_t> packets_dropped_{0};
    
public:
    SolarflareBypassClient();
    ~SolarflareBypassClient() override;
    
    // KernelBypassClient interface
    bool initialize(const BypassConfig& config) override;
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override { return initialized_; }
    
    void start_reception(PacketHandler handler) override;
    void stop_reception() override;
    
    void release_packet(void* context) override;
    
    std::uint64_t packets_received() const override { return packets_received_.load(); }
    std::uint64_t bytes_received() const override { return bytes_received_.load(); }
    std::uint64_t packets_dropped() const override { return packets_dropped_.load(); }
    double cpu_utilization() const override;
    
    BypassBackend backend_type() const override { return BypassBackend::SOLARFLARE_VI; }
    std::string backend_info() const override;
    
private:
    bool setup_virtual_interface();
    bool setup_packet_buffers();
    void reception_loop();
    void process_events();
};
#endif // MDFH_ENABLE_SOLARFLARE

// High-level kernel bypass ingestion client
class BypassIngestionClient {
private:
    BypassConfig config_;
    std::unique_ptr<KernelBypassClient> bypass_client_;
    std::unique_ptr<MessageParser> parser_;
    RingBuffer* ring_buffer_{nullptr};
    IngestionStats* stats_{nullptr};
    
    // Zero-copy packet management
    std::vector<void*> pending_packets_;
    std::mutex packet_mutex_;
    
public:
    explicit BypassIngestionClient(BypassConfig config);
    ~BypassIngestionClient();
    
    // Configuration and lifecycle
    bool initialize();
    bool connect();
    void disconnect();
    
    // Start ingestion with ring buffer and statistics
    void start_ingestion(RingBuffer& ring, IngestionStats& stats);
    void stop_ingestion();
    
    // Status and information
    bool is_connected() const;
    const BypassConfig& config() const { return config_; }
    std::string backend_info() const;
    
    // Performance statistics
    std::uint64_t packets_received() const;
    std::uint64_t bytes_received() const;
    std::uint64_t packets_dropped() const;
    double cpu_utilization() const;
    
private:
    void packet_handler(const PacketDesc& packet);
    void cleanup_processed_packets();
};

} // namespace mdfh 