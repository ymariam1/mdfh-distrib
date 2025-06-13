#!/bin/bash

echo "=== Quick Bypass Ingestion Test ==="
echo ""

# Kill any existing instances
pkill -f market_data_server
pkill -f bypass_ingestion_benchmark

# Start the server
echo "Starting market data server..."
./build/market_data_server --rate 100000 --batch-size 100 &
SERVER_PID=$!
sleep 2

# Run the client for 15 seconds
echo "Starting bypass ingestion benchmark for 15 seconds..."
timeout 15 ./build/bypass_ingestion_benchmark \
    --max-seconds 15 \
    --rx-ring-size 8192 \
    --batch-size 64 \
    --buffer-capacity 262144

# Kill the server
kill $SERVER_PID 2>/dev/null

echo ""
echo "Test complete! Check the output above for:"
echo "1. Current message rates (not decreasing cumulative averages)"
echo "2. No 'Partial buffer overflow' warnings"
echo "3. Continuous message reception" 