#!/bin/bash

# Script to run MDFH benchmark clients
# NOTE: You must start the market data server separately before running this script

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Get the project root directory (parent of scripts directory)
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

# Default configuration
HOST="127.0.0.1"
PORT=9001
DURATION=60
BENCHMARK_TYPE="bypass"  # Options: bypass, multi_feed
BATCH_SIZE=32           # Number of messages per batch
RX_RING_SIZE=4096       # RX ring buffer size
CPU_CORE=0              # CPU core for networking thread
BACKEND="asio"          # Kernel bypass backend: asio, dpdk, solarflare
ZERO_COPY=false         # Enable zero-copy optimization
VERBOSE=false           # Enable verbose output

# Function to print usage
usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "IMPORTANT: Start the market data server first:"
    echo "  ./build/market_data_server --port $PORT --rate 50000"
    echo ""
    echo "Options:"
    echo "  -h, --host HOST       Server host (default: $HOST)"
    echo "  -p, --port PORT       Server port (default: $PORT)"
    echo "  -d, --duration SECS   Benchmark duration in seconds (default: $DURATION)"
    echo "  -t, --type TYPE       Benchmark type: bypass or multi_feed (default: $BENCHMARK_TYPE)"
    echo "  -b, --batch SIZE      Batch size for processing (default: $BATCH_SIZE)"
    echo "  --rx-ring SIZE        RX ring buffer size (default: $RX_RING_SIZE)"
    echo "  --cpu-core CORE       CPU core for networking (default: $CPU_CORE)"
    echo "  --backend BACKEND     Kernel bypass backend: asio, dpdk, solarflare (default: $BACKEND)"
    echo "  --zero-copy           Enable zero-copy optimization"
    echo "  --verbose             Enable verbose output"
    echo "  --help                Show this help message"
    echo ""
    echo "Examples:"
    echo "  # Basic benchmark (start server first):"
    echo "  ./build/market_data_server --rate 100000 &"
    echo "  $0 --duration 30 --verbose"
    echo ""
    echo "  # High-performance benchmark:"
    echo "  ./build/market_data_server --rate 1000000 --batch-size 1000 &"
    echo "  $0 --backend asio --rx-ring 8192 --batch 64 --zero-copy --duration 60"
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--host)
            HOST="$2"
            shift 2
            ;;
        -p|--port)
            PORT="$2"
            shift 2
            ;;
        -d|--duration)
            DURATION="$2"
            shift 2
            ;;
        -t|--type)
            BENCHMARK_TYPE="$2"
            shift 2
            ;;
        -b|--batch)
            BATCH_SIZE="$2"
            shift 2
            ;;
        --rx-ring)
            RX_RING_SIZE="$2"
            shift 2
            ;;
        --cpu-core)
            CPU_CORE="$2"
            shift 2
            ;;
        --backend)
            BACKEND="$2"
            shift 2
            ;;
        --zero-copy)
            ZERO_COPY=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Validate benchmark type
if [[ "$BENCHMARK_TYPE" != "bypass" && "$BENCHMARK_TYPE" != "multi_feed" ]]; then
    echo "Error: Invalid benchmark type. Must be 'bypass' or 'multi_feed'"
    exit 1
fi

# Validate backend
if [[ "$BACKEND" != "asio" && "$BACKEND" != "dpdk" && "$BACKEND" != "solarflare" ]]; then
    echo "Error: Invalid backend. Must be 'asio', 'dpdk', or 'solarflare'"
    exit 1
fi

# Check if executables exist
if [[ "$BENCHMARK_TYPE" == "bypass" && ! -f "$BUILD_DIR/bypass_ingestion_benchmark" ]]; then
    echo "Error: bypass_ingestion_benchmark executable not found. Please build the project first."
    exit 1
elif [[ "$BENCHMARK_TYPE" == "multi_feed" && ! -f "$BUILD_DIR/multi_feed_benchmark" ]]; then
    echo "Error: multi_feed_benchmark executable not found. Please build the project first."
    exit 1
fi

# Check if server is running
echo "Checking if server is running on $HOST:$PORT..."
if ! nc -z "$HOST" "$PORT" 2>/dev/null; then
    echo ""
    echo "ERROR: No server found on $HOST:$PORT"
    echo ""
    echo "Please start the market data server first:"
    echo "  cd $BUILD_DIR"
    echo "  ./market_data_server --host $HOST --port $PORT --rate 50000 --verbose"
    echo ""
    echo "Then run this benchmark script in another terminal."
    exit 1
fi

echo "✓ Server is running on $HOST:$PORT"

# Build command line arguments
COMMON_ARGS=(
    "--host" "$HOST"
    "--port" "$PORT"
    "--max-seconds" "$DURATION"
    "--rx-ring-size" "$RX_RING_SIZE"
    "--batch-size" "$BATCH_SIZE"
    "--cpu-core" "$CPU_CORE"
    "--backend" "$BACKEND"
    "--performance-report"
    "--latency-histogram"
)

if [[ "$ZERO_COPY" == "true" ]]; then
    # Zero-copy is enabled by default, so we don't add --no-zero-copy
    :
else
    COMMON_ARGS+=("--no-zero-copy")
fi

if [[ "$VERBOSE" == "true" ]]; then
    COMMON_ARGS+=("--verbose")
fi

# Run the appropriate benchmark
echo ""
echo "=== MDFH Benchmark Configuration ==="
echo "Server: $HOST:$PORT"
echo "Benchmark Type: $BENCHMARK_TYPE"
echo "Backend: $BACKEND"
echo "Duration: $DURATION seconds"
echo "RX Ring Size: $RX_RING_SIZE"
echo "Batch Size: $BATCH_SIZE"
echo "CPU Core: $CPU_CORE"
echo "Zero-copy: $ZERO_COPY"
echo "Verbose: $VERBOSE"
echo ""

if [[ "$BENCHMARK_TYPE" == "bypass" ]]; then
    echo "Running bypass ingestion benchmark..."
    echo "Command: $BUILD_DIR/bypass_ingestion_benchmark ${COMMON_ARGS[*]}"
    echo ""
    "$BUILD_DIR/bypass_ingestion_benchmark" "${COMMON_ARGS[@]}"
else
    echo "Running multi-feed benchmark..."
    echo "Command: $BUILD_DIR/multi_feed_benchmark ${COMMON_ARGS[*]}"
    echo ""
    "$BUILD_DIR/multi_feed_benchmark" "${COMMON_ARGS[@]}"
fi

echo ""
echo "Benchmark completed!" 