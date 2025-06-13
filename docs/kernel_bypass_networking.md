# Kernel Bypass Networking for MDFH

## Overview

The MDFH (Market Data Feed Handler) kernel bypass networking implementation provides true zero-copy, ultra-low latency network packet reception by bypassing the kernel's network stack. This implementation supports multiple high-performance networking technologies including DPDK and Solarflare ef_vi.

## Supported Backends

### 1. Boost.Asio (Default/Fallback)
- **Use Case**: Development, testing, and environments without specialized hardware
- **Latency**: Standard kernel networking (~10-50µs)
- **Throughput**: Good for most applications
- **Hardware Requirements**: Any standard NIC

### 2. DPDK (Intel High-Performance)
- **Use Case**: Intel NICs with high packet rates
- **Latency**: Very low (~1-5µs)
- **Throughput**: Multi-million packets/second
- **Hardware Requirements**: Intel NICs (82599, X540, X710, etc.)

### 3. Solarflare ef_vi (Ultra-Low Latency)
- **Use Case**: Solarflare NICs for ultimate latency performance
- **Latency**: Ultra-low (<1µs)
- **Throughput**: High packet rates with consistent latency
- **Hardware Requirements**: Solarflare NICs

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Application Layer                            │
├─────────────────────────────────────────────────────────────────┤
│  BypassIngestionClient                                          │
│  ├── PacketHandler (Zero-copy callback)                        │
│  ├── MessageParser (Zero-copy parsing)                         │
│  └── RingBuffer Integration                                    │
├─────────────────────────────────────────────────────────────────┤
│  KernelBypassClient (Abstract Interface)                       │
│  ├── BoostAsioBypassClient (Fallback)                         │
│  ├── DPDKBypassClient (Intel NICs)                            │
│  └── SolarflareBypassClient (Solarflare NICs)                 │
├─────────────────────────────────────────────────────────────────┤
│  Hardware Abstraction Layer                                    │
│  ├── Boost.Asio Socket API                                    │
│  ├── DPDK PMD (Poll Mode Driver)                              │
│  └── Solarflare ef_vi API                                     │
├─────────────────────────────────────────────────────────────────┤
│                    Network Hardware                            │
└─────────────────────────────────────────────────────────────────┘
```

## Key Features

### Zero-Copy Packet Processing
- Packets are processed directly from NIC memory
- No kernel buffer copies
- Configurable zero-copy threshold based on packet size
- Automatic packet lifecycle management

### Batch Processing
- Configurable batch sizes for optimal CPU utilization
- Reduces per-packet overhead
- Improves cache efficiency

### CPU Affinity and NUMA Awareness
- Pin networking threads to specific CPU cores
- NUMA-aware memory allocation
- Isolation from system interrupt handling

### Hardware Timestamping
- Support for hardware-generated timestamps when available
- Nanosecond precision timing
- Consistent latency measurements

## Configuration

### Basic Configuration

```cpp
#include "mdfh/kernel_bypass.hpp"

// Create configuration
BypassConfig config;
config.backend = BypassBackend::DPDK;  // or BOOST_ASIO, SOLARFLARE_VI
config.interface_name = "eth0";
config.host = "224.0.0.1";             // Multicast address
config.port = 9001;
config.rx_ring_size = 2048;            // Must be power of 2
config.batch_size = 32;
config.cpu_core = 1;                   // Pin to CPU core 1
config.enable_zero_copy = true;
config.enable_numa_awareness = true;
config.zero_copy_threshold = 64;       // Minimum packet size for zero-copy
config.poll_timeout_us = 100;          // Polling timeout

// Create and initialize client
BypassIngestionClient client(config);
client.initialize();
client.connect();

// Start ingestion
RingBuffer ring(65536);
IngestionStats stats;
client.start_ingestion(ring, stats);
```

### Advanced Configuration Options

| Option | Description | Default | Notes |
|--------|-------------|---------|-------|
| `rx_ring_size` | RX ring buffer size | 2048 | Must be power of 2 |
| `batch_size` | Packet batch processing size | 32 | <= rx_ring_size |
| `cpu_core` | CPU core for networking thread | 0 | 0 = no affinity |
| `enable_zero_copy` | Enable zero-copy processing | true | Requires hardware support |
| `enable_numa_awareness` | NUMA-aware allocations | true | For multi-socket systems |
| `zero_copy_threshold` | Min packet size for zero-copy | 64 bytes | Performance tuning |
| `poll_timeout_us` | Polling timeout | 100µs | Balance latency vs CPU |

## Build Configuration

### Enable DPDK Support

```bash
# Install DPDK
sudo apt-get install dpdk-dev libdpdk-dev

# Build with DPDK
mkdir build && cd build
cmake -DENABLE_DPDK=ON ..
make
```

### Enable Solarflare Support

```bash
# Install Solarflare OpenOnload
# (Follow Solarflare installation guide)

# Build with Solarflare
mkdir build && cd build
cmake -DENABLE_SOLARFLARE=ON ..
make
```

### Build without Kernel Bypass (Boost.Asio only)

```bash
mkdir build && cd build
cmake ..
make
```

## Performance Tuning

### System Configuration for DPDK

```bash
# Huge pages (required for DPDK)
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# CPU isolation (optional)
# Add to kernel command line: isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3

# Disable NIC interrupt coalescing
ethtool -C eth0 rx-usecs 0 rx-frames 1
```

### System Configuration for Solarflare

```bash
# Load Solarflare drivers
modprobe sfc

# Configure interrupt affinity
echo 1 > /proc/irq/24/smp_affinity  # Pin IRQ to core 0

# Enable hardware timestamping
ethtool -T eth0  # Check capabilities
```

### Performance Optimization Guidelines

1. **CPU Affinity**: Pin networking threads to isolated cores
2. **Interrupt Handling**: Separate data processing from interrupt handling
3. **Memory**: Use huge pages for better TLB efficiency
4. **Batch Size**: Tune based on packet arrival rate and latency requirements
5. **Ring Buffer Size**: Balance memory usage with burst handling capability

## Usage Examples

### Basic Usage with Boost.Asio

```cpp
#include "mdfh/kernel_bypass.hpp"

int main() {
    BypassConfig config;
    config.backend = BypassBackend::BOOST_ASIO;
    config.host = "127.0.0.1";
    config.port = 9001;
    
    BypassIngestionClient client(config);
    client.initialize();
    client.connect();
    
    RingBuffer ring(65536);
    IngestionStats stats;
    
    client.start_ingestion(ring, stats);
    
    // Process messages
    Slot slot;
    while (client.is_connected()) {
        if (ring.try_pop(slot)) {
            // Process message
            stats.record_message_processed(slot);
        }
        stats.check_periodic_flush();
    }
    
    client.stop_ingestion();
    return 0;
}
```

### High-Performance DPDK Usage

```cpp
#include "mdfh/kernel_bypass.hpp"

int main() {
    BypassConfig config;
    config.backend = BypassBackend::DPDK;
    config.interface_name = "eth0";
    config.host = "224.0.0.1";  // Multicast
    config.port = 9001;
    config.rx_ring_size = 4096;
    config.batch_size = 64;
    config.cpu_core = 2;  // Isolated core
    config.enable_zero_copy = true;
    config.enable_numa_awareness = true;
    
    BypassIngestionClient client(config);
    
    if (!client.initialize()) {
        std::cerr << "Failed to initialize DPDK" << std::endl;
        return 1;
    }
    
    if (!client.connect()) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "Using: " << client.backend_info() << std::endl;
    
    RingBuffer ring(262144);  // Large ring for high throughput
    IngestionStats stats;
    
    client.start_ingestion(ring, stats);
    
    // High-performance message processing loop
    Slot slot;
    while (client.is_connected()) {
        // Batch processing for efficiency
        int processed = 0;
        while (ring.try_pop(slot) && processed < 1000) {
            stats.record_message_processed(slot);
            processed++;
        }
        
        // Periodic statistics
        if (processed == 0) {
            stats.check_periodic_flush();
        }
    }
    
    client.stop_ingestion();
    
    // Print performance statistics
    std::cout << "Packets: " << client.packets_received() << std::endl;
    std::cout << "Messages: " << stats.messages_processed() << std::endl;
    std::cout << "Rate: " << (stats.messages_processed() / stats.elapsed_seconds()) 
              << " msg/s" << std::endl;
    
    return 0;
}
```

### Command Line Benchmark Tool

```bash
# Run with Boost.Asio (default)
./bypass_ingestion_benchmark --host 127.0.0.1 --port 9001 --max-seconds 60

# Run with DPDK
./bypass_ingestion_benchmark --backend dpdk --interface eth0 \
    --rx-ring-size 4096 --batch-size 64 --cpu-core 2 \
    --max-seconds 60 --verbose

# Run with Solarflare
./bypass_ingestion_benchmark --backend solarflare --interface eth0 \
    --cpu-core 1 --max-seconds 60 --latency-histogram

# Performance tuning options
./bypass_ingestion_benchmark --backend dpdk \
    --rx-ring-size 8192 --batch-size 128 \
    --buffer-capacity 262144 --zero-copy-threshold 32 \
    --poll-timeout 50 --no-numa
```

## Performance Expectations

### Latency Performance (typical values)

| Backend | Mean Latency | 99th Percentile | 99.9th Percentile |
|---------|--------------|-----------------|-------------------|
| Boost.Asio | 15-30µs | 50-100µs | 200-500µs |
| DPDK | 2-5µs | 10-20µs | 30-50µs |
| Solarflare | 0.5-2µs | 5-10µs | 15-25µs |

### Throughput Performance

| Backend | Typical Rate | Max Burst Rate | CPU Usage |
|---------|--------------|----------------|-----------|
| Boost.Asio | 1-5M msg/s | 10M msg/s | Medium |
| DPDK | 10-50M msg/s | 100M msg/s | Low-Medium |
| Solarflare | 5-30M msg/s | 50M msg/s | Low |

*Note: Performance varies significantly based on hardware, system configuration, and packet sizes.*

## Troubleshooting

### DPDK Issues

**Problem**: DPDK initialization fails
**Solution**: 
- Check huge pages: `cat /proc/meminfo | grep Huge`
- Verify NIC binding: `dpdk-devbind --status`
- Ensure sufficient privileges: Run as root or with CAP_SYS_ADMIN

**Problem**: No packets received
**Solution**:
- Verify NIC is in promiscuous mode
- Check multicast group membership
- Verify hardware filters

### Solarflare Issues

**Problem**: ef_vi initialization fails
**Solution**:
- Verify Solarflare drivers are loaded: `lsmod | grep sfc`
- Check hardware support: `ethtool -i eth0`
- Ensure proper licensing for advanced features

### General Performance Issues

**Problem**: High latency or packet drops
**Solution**:
- Increase ring buffer sizes
- Tune batch processing size
- Check CPU affinity settings
- Monitor system interrupts: `cat /proc/interrupts`

**Problem**: Low throughput
**Solution**:
- Increase batch size
- Verify zero-copy is enabled
- Check for CPU thermal throttling
- Monitor memory bandwidth usage

## Integration with Existing Code

The kernel bypass implementation is designed to be a drop-in replacement for existing networking code:

```cpp
// Before: Standard ingestion
IngestionBenchmark benchmark(config);

// After: Kernel bypass ingestion
BypassConfig bypass_config = convert_config(config);
BypassIngestionClient client(bypass_config);
// Same ring buffer and stats interfaces
```

The `MessageParser` and `RingBuffer` interfaces remain unchanged, ensuring compatibility with existing message processing code.

## Future Enhancements

1. **RDMA Support**: InfiniBand and RoCE integration
2. **Hardware Timestamping**: Enhanced precision timing
3. **Multi-Queue Support**: Parallel processing with RSS/flow steering
4. **Custom Memory Allocators**: Optimized memory management
5. **Advanced Statistics**: Detailed performance profiling
6. **Hot Plugging**: Dynamic NIC configuration changes

## References

- [DPDK Programmer's Guide](https://doc.dpdk.org/guides/prog_guide/)
- [Solarflare ef_vi User Guide](https://www.xilinx.com/content/dam/xilinx/support/documentation/user_guides/sf-ef_vi-user.pdf)
- [Linux Network Performance Tuning](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/performance_tuning_guide/chap-red_hat_enterprise_linux-performance_tuning_guide-networking) 