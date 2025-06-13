# MDFH Benchmarking Guide

This guide shows you how to run performance benchmarks with the MDFH (Market Data Feed Handler) system.

## 🚀 Quick Start

### Step 1: Start the Market Data Server

The market data server generates synthetic market data for benchmarking. Start it in one terminal:

```bash
cd build

# Basic server (50K messages/second)
./market_data_server --rate 50000 --verbose

# High-performance server (1M messages/second)  
./market_data_server --rate 1000000 --batch-size 1000 --verbose

# Custom configuration
./market_data_server --host 127.0.0.1 --port 9001 --rate 100000 --max-seconds 120
```

### Step 2: Run the Benchmark Client

In another terminal, run the benchmark client:

```bash
cd build

# Basic benchmark
./bypass_ingestion_benchmark --max-seconds 30 --verbose

# High-performance benchmark with detailed analysis
./bypass_ingestion_benchmark \
    --max-seconds 60 \
    --rx-ring-size 8192 \
    --batch-size 64 \
    --performance-report \
    --latency-histogram \
    --verbose
```

## 📊 Example Benchmark Session

### Terminal 1 (Server):
```bash
$ ./market_data_server --rate 100000 --verbose
Market Data Server Configuration:
  Listen: 127.0.0.1:9001
  Rate: 100000 msgs/sec
  Batch Size: 100 msgs
  Base Price: $100
  Price Jitter: ±$0.05
  Max Quantity: 1000

Starting market data server on 127.0.0.1:9001
Starting message generation...
Client connected from 127.0.0.1:58901
Sent 100000 messages to 1 clients
Sent 200000 messages to 1 clients
...
```

### Terminal 2 (Client):
```bash
$ ./bypass_ingestion_benchmark --max-seconds 30 --verbose
Kernel Bypass Ingestion Benchmark Configuration:
  Network: 127.0.0.1:9001
  Interface: eth0
  Backend: asio
  RX Ring Size: 2048
  Batch Size: 32
  CPU Core: 0
  Zero-copy: enabled
  NUMA Awareness: enabled
  Buffer Capacity: 65536 slots
  Max Duration: 30 seconds

Using backend: Boost.Asio (kernel networking)
Connected successfully, starting ingestion...
T+   5.0s | Recv:   500000 msgs | Proc:   500000 msgs | Drop:      0 | Rate: 100000.0 msg/s | BW:   1.9 MB/s
T+  10.0s | Recv:  1000000 msgs | Proc:  1000000 msgs | Drop:      0 | Rate: 100000.0 msg/s | BW:   1.9 MB/s
...

=== Kernel Bypass Ingestion Benchmark Results ===
Backend: Boost.Asio (kernel networking)
Duration: 30 seconds

--- Network Layer Statistics ---
Packets received: 30000
Packet bytes: 57600000 (54.9 MB)
Packets dropped: 0
Packet rate: 1000 packets/s
Network bandwidth: 1.83 MB/s

--- Application Layer Statistics ---
Messages received: 3000000
Messages processed: 3000000
Messages dropped: 0
Sequence gaps: 0
Message bytes: 60000000 (57.2 MB)
Message rate: 100000 msg/s
Processing rate: 100000 msg/s

--- Performance Analysis ---
Messages per packet: 100
Processing efficiency: 100%
```

## 🛠️ Server Configuration Options

### Basic Options
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--host` | Host address to bind to | 127.0.0.1 | `--host 0.0.0.0` |
| `--port` | Port to listen on | 9001 | `--port 9002` |
| `--rate` | Message rate (msgs/sec) | 50000 | `--rate 1000000` |
| `--batch-size` | Messages per batch | 100 | `--batch-size 1000` |

### Duration Control
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--max-seconds` | Max duration (0 = infinite) | 0 | `--max-seconds 120` |
| `--max-messages` | Max messages (0 = infinite) | 0 | `--max-messages 10000000` |

### Market Data Settings
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--base-price` | Base price for messages | 100.0 | `--base-price 50.0` |
| `--price-jitter` | Price jitter range (±) | 0.05 | `--price-jitter 0.1` |
| `--max-quantity` | Maximum quantity per message | 1000 | `--max-quantity 5000` |

### Output
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--verbose` | Enable verbose output | false | `--verbose` |

## 🎯 Client Configuration Options

### Network Settings
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--host` | Server host address | 127.0.0.1 | `--host 192.168.1.100` |
| `--port` | Server port | 9001 | `--port 9002` |
| `--interface` | Network interface | eth0 | `--interface lo` |

### Performance Settings
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--backend` | Kernel bypass backend | asio | `--backend dpdk` |
| `--rx-ring-size` | RX ring buffer size | 2048 | `--rx-ring-size 8192` |
| `--batch-size` | Packet batch size | 32 | `--batch-size 64` |
| `--cpu-core` | CPU core for networking | 0 | `--cpu-core 2` |
| `--buffer-capacity` | Ring buffer capacity | 65536 | `--buffer-capacity 262144` |

### Optimization Flags
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--no-zero-copy` | Disable zero-copy | enabled | `--no-zero-copy` |
| `--no-numa` | Disable NUMA awareness | enabled | `--no-numa` |
| `--zero-copy-threshold` | Min packet size for zero-copy | 64 | `--zero-copy-threshold 128` |

### Test Duration
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--max-seconds` | Max test duration | 60 | `--max-seconds 120` |
| `--max-messages` | Max messages to process | 0 | `--max-messages 10000000` |

### Output Options
| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--verbose` | Enable verbose output | false | `--verbose` |
| `--performance-report` | Show detailed performance report | false | `--performance-report` |
| `--latency-histogram` | Show latency histogram | false | `--latency-histogram` |

## 🚀 Performance Testing Scenarios

### 1. Basic Functionality Test
```bash
# Terminal 1: Start basic server
./market_data_server --rate 10000 --max-seconds 30

# Terminal 2: Run basic client
./bypass_ingestion_benchmark --max-seconds 30 --verbose
```

### 2. High Throughput Test
```bash
# Terminal 1: High-rate server
./market_data_server --rate 1000000 --batch-size 1000 --verbose

# Terminal 2: Optimized client
./bypass_ingestion_benchmark \
    --max-seconds 60 \
    --rx-ring-size 8192 \
    --batch-size 128 \
    --buffer-capacity 262144 \
    --performance-report \
    --verbose
```

### 3. Latency Analysis Test
```bash
# Terminal 1: Moderate rate server
./market_data_server --rate 100000 --verbose

# Terminal 2: Latency-focused client
./bypass_ingestion_benchmark \
    --max-seconds 60 \
    --performance-report \
    --latency-histogram \
    --sampling-rate 100 \
    --verbose
```

### 4. Zero-Copy Performance Test
```bash
# Terminal 1: Large batch server
./market_data_server --rate 500000 --batch-size 500

# Terminal 2: Zero-copy client
./bypass_ingestion_benchmark \
    --max-seconds 60 \
    --zero-copy-threshold 32 \
    --rx-ring-size 4096 \
    --performance-report \
    --verbose
```

## 📈 Using the Convenience Script

The `run_benchmarks.sh` script simplifies client-side benchmarking:

```bash
# Start server manually first
./build/market_data_server --rate 100000 --verbose &

# Run benchmark script
./scripts/run_benchmarks.sh --duration 30 --backend asio --verbose --zero-copy

# Stop server
kill %1
```

### Script Options
```bash
./scripts/run_benchmarks.sh --help

# Examples:
./scripts/run_benchmarks.sh --duration 60 --rx-ring 8192 --batch 64
./scripts/run_benchmarks.sh --type multi_feed --backend asio --verbose
./scripts/run_benchmarks.sh --cpu-core 2 --zero-copy --duration 120
```

## 🔍 Interpreting Results

### Key Metrics to Watch

**Network Layer:**
- **Packets received**: Total network packets processed
- **Packet rate**: Packets per second (should match server batch rate)
- **Packets dropped**: Should be 0 for good performance

**Application Layer:**
- **Message rate**: Messages per second (should match server rate)
- **Processing efficiency**: Should be close to 100%
- **Sequence gaps**: Should be 0 for reliable delivery

**Performance Indicators:**
- **Messages per packet**: Higher is better (indicates good batching)
- **Processing efficiency**: 100% means no message loss
- **Latency percentiles**: Lower is better (P99 < 100µs is excellent)

### Troubleshooting Performance Issues

**High Packet Drops:**
- Increase `--rx-ring-size`
- Increase `--buffer-capacity`
- Use `--cpu-core` for CPU affinity

**High Latency:**
- Reduce `--batch-size`
- Enable `--zero-copy`
- Use dedicated CPU cores

**Low Throughput:**
- Increase `--batch-size`
- Increase `--rx-ring-size`
- Check server rate matches client capacity

## 🎯 Performance Targets

### Expected Performance (Boost.Asio backend)

| Metric | Good | Excellent | Outstanding |
|--------|------|-----------|-------------|
| Throughput | 100K msg/s | 500K msg/s | 1M+ msg/s |
| Latency P50 | < 50µs | < 20µs | < 10µs |
| Latency P99 | < 200µs | < 100µs | < 50µs |
| CPU Usage | < 50% | < 30% | < 20% |
| Packet Loss | < 0.1% | < 0.01% | 0% |

### Hardware Recommendations

**For High Throughput (1M+ msg/s):**
- CPU: 8+ cores, 3.0+ GHz
- Memory: 16GB+ RAM
- Network: 10GbE+
- Storage: NVMe SSD

**For Ultra-Low Latency (<10µs):**
- CPU: High-frequency cores (4.0+ GHz)
- Memory: Low-latency DDR4/DDR5
- Network: Kernel bypass (DPDK/Solarflare)
- System: Real-time kernel, CPU isolation

## 🔧 Advanced Configuration

### System Tuning for Maximum Performance

```bash
# Increase network buffer sizes
echo 'net.core.rmem_max = 134217728' >> /etc/sysctl.conf
echo 'net.core.wmem_max = 134217728' >> /etc/sysctl.conf

# CPU isolation (add to kernel command line)
# isolcpus=2-7 nohz_full=2-7 rcu_nocbs=2-7

# Huge pages for DPDK
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### Multi-Client Testing

```bash
# Terminal 1: Server
./market_data_server --rate 1000000 --verbose

# Terminal 2: Client 1
./bypass_ingestion_benchmark --cpu-core 1 --max-seconds 60 &

# Terminal 3: Client 2  
./bypass_ingestion_benchmark --cpu-core 2 --max-seconds 60 &

# Wait for completion
wait
```

---

**Happy Benchmarking!** 🚀

For more advanced configurations and kernel bypass networking, see `docs/kernel_bypass_networking.md`. 