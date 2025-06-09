#include "mdfh/kernel_bypass.hpp"
#include "mdfh/ingestion.hpp"
#include "mdfh/timing.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#ifdef __linux__
#include <sched.h>
#endif
#include <unistd.h>

#ifdef MDFH_ENABLE_DPDK
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_lcore.h>
#include <rte_cycles.h>
#endif

#ifdef MDFH_ENABLE_SOLARFLARE
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <etherfabric/ef_vi.h>
#endif

namespace mdfh {

// BypassConfig validation
bool BypassConfig::is_valid() const {
    if (rx_ring_size == 0 || (rx_ring_size & (rx_ring_size - 1)) != 0) {
        return false; // Must be power of 2
    }
    if (batch_size == 0 || batch_size > rx_ring_size) {
        return false;
    }
    if (interface_name.empty()) {
        return false;
    }
    return true;
}

// Factory function implementation
std::unique_ptr<KernelBypassClient> create_bypass_client(BypassBackend backend) {
    switch (backend) {
        case BypassBackend::BOOST_ASIO:
            return std::make_unique<BoostAsioBypassClient>();
            
#ifdef MDFH_ENABLE_DPDK
        case BypassBackend::DPDK:
            return std::make_unique<DPDKBypassClient>();
#endif

#ifdef MDFH_ENABLE_SOLARFLARE
        case BypassBackend::SOLARFLARE_VI:
            return std::make_unique<SolarflareBypassClient>();
#endif

        default:
            std::cerr << "Unsupported bypass backend, falling back to Boost.Asio" << std::endl;
            return std::make_unique<BoostAsioBypassClient>();
    }
}

// Utility function to set CPU affinity
bool set_cpu_affinity(std::uint32_t cpu_core) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_core, &cpuset);
    
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
#else
    // CPU affinity not supported on this platform
    (void)cpu_core;
    return true;
#endif
}

// BoostAsioBypassClient implementation
BoostAsioBypassClient::BoostAsioBypassClient() = default;

BoostAsioBypassClient::~BoostAsioBypassClient() {
    disconnect();
}

bool BoostAsioBypassClient::initialize(const BypassConfig& config) {
    if (!config.is_valid()) {
        std::cerr << "Invalid bypass configuration" << std::endl;
        return false;
    }
    
    config_ = config;
    
    // Create underlying NetworkClient with converted config
    IngestionConfig ing_config;
    ing_config.host = config.host;
    ing_config.port = config.port;
    ing_config.buffer_capacity = config.rx_ring_size;
    
    asio_client_ = std::make_unique<NetworkClient>(ing_config);
    return true;
}

bool BoostAsioBypassClient::connect() {
    if (!asio_client_) {
        return false;
    }
    
    try {
        asio_client_->connect();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Boost.Asio connect failed: " << e.what() << std::endl;
        return false;
    }
}

void BoostAsioBypassClient::disconnect() {
    stop_reception();
    if (asio_client_) {
        asio_client_->stop();
    }
}

bool BoostAsioBypassClient::is_connected() const {
    return asio_client_ && asio_client_->is_connected();
}

void BoostAsioBypassClient::start_reception(PacketHandler handler) {
    if (!handler || running_.load()) {
        return;
    }
    
    packet_handler_ = std::move(handler);
    running_ = true;
    
    reception_thread_ = std::thread([this]() {
        reception_loop();
    });
    
    // Set CPU affinity if requested
    if (config_.cpu_core > 0) {
        set_cpu_affinity(config_.cpu_core);
    }
}

void BoostAsioBypassClient::stop_reception() {
    running_ = false;
    if (reception_thread_.joinable()) {
        reception_thread_.join();
    }
}

void BoostAsioBypassClient::release_packet(void* context) {
    // For Boost.Asio, packets are copied, so no release needed
    (void)context;
}

void BoostAsioBypassClient::reception_loop() {
    std::cout << "Boost.Asio bypass client reception loop started" << std::endl;
    
    // Create our own socket for kernel bypass simulation
    boost::asio::io_context ctx;
    boost::asio::ip::tcp::socket socket(ctx);
    boost::system::error_code ec;
    
    // Connect to the server
    boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::address::from_string(config_.host), 
        config_.port
    );
    
    socket.connect(endpoint, ec);
    if (ec) {
        std::cerr << "Boost.Asio bypass socket connection failed: " << ec.message() << std::endl;
        return;
    }
    
    std::cout << "Bypass socket connected to " << config_.host << ":" << config_.port << std::endl;
    
    // Buffer for receiving data
    std::vector<std::uint8_t> buffer(65536);  // 64KB buffer
    
    while (running_.load()) {
        try {
            // Use async_read_some with timeout
            std::size_t bytes_received = 0;
            
            // Set up async read with timeout
            auto deadline = std::chrono::steady_clock::now() + 
                           std::chrono::microseconds(config_.poll_timeout_us * 10);
            
            // Try to receive data with non-blocking read
            bytes_received = socket.read_some(boost::asio::buffer(buffer), ec);
            
            if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                // No data available, continue polling
                std::this_thread::sleep_for(std::chrono::microseconds(config_.poll_timeout_us));
                continue;
            } else if (ec) {
                std::cerr << "Boost.Asio reception error: " << ec.message() << std::endl;
                break;
            }
            
            if (bytes_received > 0) {
                // Update statistics
                packets_received_.fetch_add(1, std::memory_order_relaxed);
                bytes_received_.fetch_add(bytes_received, std::memory_order_relaxed);
                
                // Get timestamp
                std::uint64_t timestamp_ns = get_timestamp_ns();
                
                // Create packet descriptor
                PacketDesc packet(buffer.data(), bytes_received, timestamp_ns, nullptr);
                
                // Call packet handler
                if (packet_handler_) {
                    packet_handler_(packet);
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Boost.Asio reception error: " << e.what() << std::endl;
            break;
        }
    }
    
    socket.close();
    std::cout << "Boost.Asio bypass client reception loop stopped" << std::endl;
}

#ifdef MDFH_ENABLE_DPDK
// DPDK static members
bool DPDKBypassClient::dpdk_initialized_ = false;
std::mutex DPDKBypassClient::dpdk_init_mutex_;

DPDKBypassClient::DPDKBypassClient() = default;

DPDKBypassClient::~DPDKBypassClient() {
    disconnect();
}

bool DPDKBypassClient::initialize(const BypassConfig& config) {
    if (!config.is_valid()) {
        std::cerr << "Invalid bypass configuration for DPDK" << std::endl;
        return false;
    }
    
    config_ = config;
    
    // Initialize DPDK EAL (only once per process)
    {
        std::lock_guard<std::mutex> lock(dpdk_init_mutex_);
        if (!dpdk_initialized_) {
            if (!setup_dpdk_eal()) {
                return false;
            }
            dpdk_initialized_ = true;
        }
    }
    
    // Setup memory pools
    if (!setup_memory_pools()) {
        std::cerr << "Failed to setup DPDK memory pools" << std::endl;
        return false;
    }
    
    // Setup ethernet port
    if (!setup_ethernet_port()) {
        std::cerr << "Failed to setup DPDK ethernet port" << std::endl;
        return false;
    }
    
    initialized_ = true;
    return true;
}

bool DPDKBypassClient::setup_dpdk_eal() {
    // Basic DPDK EAL initialization
    const char* argv[] = {
        "mdfh",
        "-l", "0-1",          // Use cores 0-1
        "--proc-type=primary",
        "--file-prefix=mdfh",
        nullptr
    };
    int argc = 5;
    
    int ret = rte_eal_init(argc, const_cast<char**>(argv));
    if (ret < 0) {
        std::cerr << "DPDK EAL initialization failed: " << rte_strerror(-ret) << std::endl;
        return false;
    }
    
    std::cout << "DPDK EAL initialized successfully" << std::endl;
    return true;
}

bool DPDKBypassClient::setup_memory_pools() {
    // Create memory pool for packet buffers
    const std::string pool_name = "mbuf_pool_" + std::to_string(port_id_);
    
    mbuf_pool_ = rte_pktmbuf_pool_create(
        pool_name.c_str(),
        8192,                    // Number of mbufs
        256,                     // Cache size
        0,                       // Application private area size
        RTE_MBUF_DEFAULT_BUF_SIZE,  // Data buffer size
        rte_socket_id()          // Socket ID
    );
    
    if (!mbuf_pool_) {
        std::cerr << "Failed to create DPDK memory pool: " << rte_strerror(rte_errno) << std::endl;
        return false;
    }
    
    std::cout << "DPDK memory pool created successfully" << std::endl;
    return true;
}

bool DPDKBypassClient::setup_ethernet_port() {
    // Check if port is available
    if (!rte_eth_dev_is_valid_port(port_id_)) {
        std::cerr << "DPDK port " << port_id_ << " is not available" << std::endl;
        return false;
    }
    
    // Configure the ethernet device
    struct rte_eth_conf port_conf = {};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rxmode.split_hdr_size = 0;
    
    int ret = rte_eth_dev_configure(port_id_, 1, 1, &port_conf);
    if (ret < 0) {
        std::cerr << "Failed to configure DPDK port " << port_id_ << ": " << rte_strerror(-ret) << std::endl;
        return false;
    }
    
    // Setup RX queue
    ret = rte_eth_rx_queue_setup(port_id_, queue_id_, config_.rx_ring_size,
                                 rte_eth_dev_socket_id(port_id_), nullptr, mbuf_pool_);
    if (ret < 0) {
        std::cerr << "Failed to setup DPDK RX queue: " << rte_strerror(-ret) << std::endl;
        return false;
    }
    
    // Setup TX queue (minimal, for completeness)
    ret = rte_eth_tx_queue_setup(port_id_, 0, 512, rte_eth_dev_socket_id(port_id_), nullptr);
    if (ret < 0) {
        std::cerr << "Failed to setup DPDK TX queue: " << rte_strerror(-ret) << std::endl;
        return false;
    }
    
    std::cout << "DPDK ethernet port " << port_id_ << " configured successfully" << std::endl;
    return true;
}

bool DPDKBypassClient::connect() {
    if (!initialized_) {
        return false;
    }
    
    // Start the ethernet port
    int ret = rte_eth_dev_start(port_id_);
    if (ret < 0) {
        std::cerr << "Failed to start DPDK port " << port_id_ << ": " << rte_strerror(-ret) << std::endl;
        return false;
    }
    
    // Enable promiscuous mode for packet capture
    ret = rte_eth_promiscuous_enable(port_id_);
    if (ret < 0) {
        std::cerr << "Failed to enable promiscuous mode: " << rte_strerror(-ret) << std::endl;
        return false;
    }
    
    std::cout << "DPDK port " << port_id_ << " started successfully" << std::endl;
    return true;
}

void DPDKBypassClient::disconnect() {
    stop_reception();
    
    if (initialized_) {
        rte_eth_dev_stop(port_id_);
        rte_eth_dev_close(port_id_);
        initialized_ = false;
    }
}

void DPDKBypassClient::start_reception(PacketHandler handler) {
    if (!handler || running_.load() || !initialized_) {
        return;
    }
    
    packet_handler_ = std::move(handler);
    running_ = true;
    
    reception_thread_ = std::thread([this]() {
        reception_loop();
    });
    
    // Set CPU affinity for DPDK reception thread
    if (config_.cpu_core > 0) {
        set_cpu_affinity(config_.cpu_core);
    }
}

void DPDKBypassClient::stop_reception() {
    running_ = false;
    if (reception_thread_.joinable()) {
        reception_thread_.join();
    }
}

void DPDKBypassClient::release_packet(void* context) {
    if (context) {
        struct rte_mbuf* mbuf = static_cast<struct rte_mbuf*>(context);
        rte_pktmbuf_free(mbuf);
    }
}

double DPDKBypassClient::cpu_utilization() const {
    // TODO: Implement CPU utilization measurement using DPDK cycles
    return 0.0;
}

std::string DPDKBypassClient::backend_info() const {
    return "DPDK " + std::string(rte_version()) + " (Intel high-performance)";
}

void DPDKBypassClient::reception_loop() {
    const std::uint16_t batch_size = static_cast<std::uint16_t>(config_.batch_size);
    std::vector<struct rte_mbuf*> packets(batch_size);
    
    std::cout << "DPDK reception loop started on core " << rte_lcore_id() << std::endl;
    
    while (running_.load()) {
        // Receive packet batch
        std::uint16_t nb_rx = rte_eth_rx_burst(port_id_, queue_id_, 
                                               packets.data(), batch_size);
        
        if (nb_rx > 0) {
            process_packet_batch(packets.data(), nb_rx);
        } else {
            // Small delay to prevent busy-waiting
            rte_delay_us(config_.poll_timeout_us);
        }
    }
    
    std::cout << "DPDK reception loop stopped" << std::endl;
}

void DPDKBypassClient::process_packet_batch(struct rte_mbuf** packets, std::uint16_t count) {
    for (std::uint16_t i = 0; i < count; ++i) {
        struct rte_mbuf* mbuf = packets[i];
        
        // Extract packet data
        const std::uint8_t* data = rte_pktmbuf_mtod(mbuf, const std::uint8_t*);
        std::size_t length = rte_pktmbuf_pkt_len(mbuf);
        
        // Get hardware timestamp if available
        std::uint64_t timestamp_ns = (mbuf->ol_flags & RTE_MBUF_F_RX_TIMESTAMP) ? 
                                     mbuf->timestamp : get_timestamp_ns();
        
        // Update statistics
        packets_received_.fetch_add(1, std::memory_order_relaxed);
        bytes_received_.fetch_add(length, std::memory_order_relaxed);
        
        // Create packet descriptor with mbuf context for zero-copy
        PacketDesc packet(data, length, timestamp_ns, mbuf);
        
        // Call packet handler
        packet_handler_(packet);
        
        // Note: packet will be released when release_packet() is called
    }
}
#endif // MDFH_ENABLE_DPDK

#ifdef MDFH_ENABLE_SOLARFLARE
SolarflareBypassClient::SolarflareBypassClient() = default;

SolarflareBypassClient::~SolarflareBypassClient() {
    disconnect();
}

bool SolarflareBypassClient::initialize(const BypassConfig& config) {
    if (!config.is_valid()) {
        std::cerr << "Invalid bypass configuration for Solarflare" << std::endl;
        return false;
    }
    
    config_ = config;
    
    // Setup virtual interface
    if (!setup_virtual_interface()) {
        std::cerr << "Failed to setup Solarflare virtual interface" << std::endl;
        return false;
    }
    
    // Setup packet buffers
    if (!setup_packet_buffers()) {
        std::cerr << "Failed to setup Solarflare packet buffers" << std::endl;
        return false;
    }
    
    initialized_ = true;
    return true;
}

bool SolarflareBypassClient::setup_virtual_interface() {
    // Allocate protection domain
    int rc = ef_pd_alloc(&pd_, EF_PD_DEFAULT, 0);
    if (rc < 0) {
        std::cerr << "Failed to allocate Solarflare protection domain: " << rc << std::endl;
        return false;
    }
    
    // Allocate virtual interface
    rc = ef_vi_alloc_from_pd(&vi_, EF_VI_RX_RING_SIZE(config_.rx_ring_size), 
                             0, pd_, -1, EF_VI_FLAGS_DEFAULT);
    if (rc < 0) {
        std::cerr << "Failed to allocate Solarflare virtual interface: " << rc << std::endl;
        ef_pd_free(pd_, 0);
        return false;
    }
    
    std::cout << "Solarflare virtual interface allocated successfully" << std::endl;
    return true;
}

bool SolarflareBypassClient::setup_packet_buffers() {
    // Calculate buffer size (ring size * max packet size)
    const std::size_t max_packet_size = 2048;
    buffer_size_ = config_.rx_ring_size * max_packet_size;
    
    // Allocate packet buffer memory
    int rc = posix_memalign(&packet_buffer_, 4096, buffer_size_);
    if (rc != 0) {
        std::cerr << "Failed to allocate Solarflare packet buffer memory" << std::endl;
        return false;
    }
    
    // Register memory with ef_vi
    rc = ef_memreg_alloc(&memreg_, pd_, packet_buffer_, buffer_size_);
    if (rc < 0) {
        std::cerr << "Failed to register Solarflare memory: " << rc << std::endl;
        free(packet_buffer_);
        return false;
    }
    
    std::cout << "Solarflare packet buffers setup successfully" << std::endl;
    return true;
}

bool SolarflareBypassClient::connect() {
    if (!initialized_) {
        return false;
    }
    
    // Post initial receive buffers
    const std::size_t packet_size = 2048;
    for (std::uint32_t i = 0; i < config_.rx_ring_size; ++i) {
        ef_vi_receive_init(vi_, 
                          static_cast<std::uint8_t*>(packet_buffer_) + i * packet_size,
                          i, packet_size);
    }
    ef_vi_receive_push(vi_);
    
    std::cout << "Solarflare interface connected successfully" << std::endl;
    return true;
}

void SolarflareBypassClient::disconnect() {
    stop_reception();
    
    if (initialized_) {
        if (memreg_) {
            ef_memreg_free(memreg_, pd_);
            memreg_ = nullptr;
        }
        if (packet_buffer_) {
            free(packet_buffer_);
            packet_buffer_ = nullptr;
        }
        if (vi_) {
            ef_vi_free(vi_, pd_);
            vi_ = nullptr;
        }
        if (pd_) {
            ef_pd_free(pd_, 0);
            pd_ = nullptr;
        }
        initialized_ = false;
    }
}

void SolarflareBypassClient::start_reception(PacketHandler handler) {
    if (!handler || running_.load() || !initialized_) {
        return;
    }
    
    packet_handler_ = std::move(handler);
    running_ = true;
    
    reception_thread_ = std::thread([this]() {
        reception_loop();
    });
    
    // Set CPU affinity for Solarflare reception thread
    if (config_.cpu_core > 0) {
        set_cpu_affinity(config_.cpu_core);
    }
}

void SolarflareBypassClient::stop_reception() {
    running_ = false;
    if (reception_thread_.joinable()) {
        reception_thread_.join();
    }
}

void SolarflareBypassClient::release_packet(void* context) {
    // For Solarflare, we need to repost the buffer
    if (context && vi_) {
        std::size_t buffer_id = reinterpret_cast<std::size_t>(context);
        const std::size_t packet_size = 2048;
        ef_vi_receive_init(vi_, 
                          static_cast<std::uint8_t*>(packet_buffer_) + buffer_id * packet_size,
                          buffer_id, packet_size);
        ef_vi_receive_push(vi_);
    }
}

double SolarflareBypassClient::cpu_utilization() const {
    // TODO: Implement CPU utilization measurement
    return 0.0;
}

std::string SolarflareBypassClient::backend_info() const {
    return "Solarflare ef_vi (ultra-low latency)";
}

void SolarflareBypassClient::reception_loop() {
    std::cout << "Solarflare reception loop started" << std::endl;
    
    while (running_.load()) {
        process_events();
        
        // Small delay to prevent busy-waiting
        usleep(config_.poll_timeout_us);
    }
    
    std::cout << "Solarflare reception loop stopped" << std::endl;
}

void SolarflareBypassClient::process_events() {
    ef_event events[config_.batch_size];
    int n_events = ef_eventq_poll(vi_, events, config_.batch_size);
    
    for (int i = 0; i < n_events; ++i) {
        if (EF_EVENT_TYPE(events[i]) == EF_EVENT_TYPE_RX) {
            std::size_t buffer_id = EF_EVENT_RX_RQ_ID(events[i]);
            std::size_t length = EF_EVENT_RX_BYTES(events[i]);
            
            // Calculate packet data pointer
            const std::size_t packet_size = 2048;
            const std::uint8_t* data = static_cast<std::uint8_t*>(packet_buffer_) + 
                                      buffer_id * packet_size;
            
            // Update statistics
            packets_received_.fetch_add(1, std::memory_order_relaxed);
            bytes_received_.fetch_add(length, std::memory_order_relaxed);
            
            // Create packet descriptor with buffer ID as context
            PacketDesc packet(data, length, get_timestamp_ns(), 
                            reinterpret_cast<void*>(buffer_id));
            
            // Call packet handler
            packet_handler_(packet);
            
            // Note: buffer will be reposted when release_packet() is called
        }
    }
}
#endif // MDFH_ENABLE_SOLARFLARE

// BypassIngestionClient implementation
BypassIngestionClient::BypassIngestionClient(BypassConfig config)
    : config_(std::move(config)) {}

BypassIngestionClient::~BypassIngestionClient() {
    stop_ingestion();
}

bool BypassIngestionClient::initialize() {
    bypass_client_ = create_bypass_client(config_.backend);
    if (!bypass_client_) {
        std::cerr << "Failed to create bypass client" << std::endl;
        return false;
    }
    
    if (!bypass_client_->initialize(config_)) {
        std::cerr << "Failed to initialize bypass client" << std::endl;
        return false;
    }
    
    parser_ = std::make_unique<MessageParser>();
    
    std::cout << "Kernel bypass client initialized: " << backend_info() << std::endl;
    return true;
}

bool BypassIngestionClient::connect() {
    if (!bypass_client_) {
        return false;
    }
    
    return bypass_client_->connect();
}

void BypassIngestionClient::disconnect() {
    if (bypass_client_) {
        bypass_client_->disconnect();
    }
}

void BypassIngestionClient::start_ingestion(RingBuffer& ring, IngestionStats& stats) {
    if (!bypass_client_) {
        return;
    }
    
    ring_buffer_ = &ring;
    stats_ = &stats;
    
    // Start packet reception with our handler
    bypass_client_->start_reception([this](const PacketDesc& packet) {
        packet_handler(packet);
    });
}

void BypassIngestionClient::stop_ingestion() {
    if (bypass_client_) {
        bypass_client_->stop_reception();
    }
    
    // Cleanup any pending packets
    cleanup_processed_packets();
}

bool BypassIngestionClient::is_connected() const {
    return bypass_client_ && bypass_client_->is_connected();
}

std::string BypassIngestionClient::backend_info() const {
    return bypass_client_ ? bypass_client_->backend_info() : "No backend";
}

std::uint64_t BypassIngestionClient::packets_received() const {
    return bypass_client_ ? bypass_client_->packets_received() : 0;
}

std::uint64_t BypassIngestionClient::bytes_received() const {
    return bypass_client_ ? bypass_client_->bytes_received() : 0;
}

std::uint64_t BypassIngestionClient::packets_dropped() const {
    return bypass_client_ ? bypass_client_->packets_dropped() : 0;
}

double BypassIngestionClient::cpu_utilization() const {
    return bypass_client_ ? bypass_client_->cpu_utilization() : 0.0;
}

void BypassIngestionClient::packet_handler(const PacketDesc& packet) {
    if (!ring_buffer_ || !stats_ || !parser_) {
        return;
    }
    
    // Record bytes received
    stats_->record_bytes_received(packet.length);
    
    // Parse packet data and push messages to ring buffer
    if (config_.enable_zero_copy && packet.length >= config_.zero_copy_threshold) {
        // Use zero-copy parsing
        parser_->parse_bytes_zero_copy(packet.data, packet.length, *ring_buffer_, *stats_);
    } else {
        // Use regular parsing (with copy)
        parser_->parse_bytes(packet.data, packet.length, *ring_buffer_, *stats_);
    }
    
    // Manage packet lifecycle for zero-copy
    if (config_.enable_zero_copy && packet.context) {
        std::lock_guard<std::mutex> lock(packet_mutex_);
        pending_packets_.push_back(packet.context);
        
        // Periodic cleanup of processed packets
        if (pending_packets_.size() > config_.batch_size * 2) {
            cleanup_processed_packets();
        }
    } else {
        // Release packet immediately if not using zero-copy
        if (bypass_client_ && packet.context) {
            bypass_client_->release_packet(packet.context);
        }
    }
}

void BypassIngestionClient::cleanup_processed_packets() {
    std::lock_guard<std::mutex> lock(packet_mutex_);
    
    // For simplicity, release all pending packets
    // In a production system, you'd track which packets are actually processed
    for (void* context : pending_packets_) {
        if (bypass_client_) {
            bypass_client_->release_packet(context);
        }
    }
    pending_packets_.clear();
}

} // namespace mdfh 