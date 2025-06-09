#include "mdfh/kernel_bypass.hpp"
#include "mdfh/ring_buffer.hpp"
#include "mdfh/ingestion.hpp"
#include "mdfh/core.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

using namespace mdfh;
using namespace boost::asio;
using tcp = ip::tcp;

std::atomic<bool> stop_server{false};

// Simple TCP server that sends test data
void run_test_server(std::uint16_t port, std::uint32_t rate, std::uint32_t duration) {
    try {
        io_context ctx;
        tcp::acceptor acceptor(ctx, tcp::endpoint(tcp::v4(), port));
        
        std::cout << "Test server listening on port " << port << std::endl;
        
        while (!stop_server) {
            tcp::socket socket(ctx);
            acceptor.accept(socket);
            
            std::cout << "Client connected from " << socket.remote_endpoint() << std::endl;
            
            // Generate test messages
            auto start_time = std::chrono::steady_clock::now();
            std::uint64_t messages_sent = 0;
            std::uint64_t sequence = 1;
            
            while (!stop_server && 
                   std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - start_time).count() < duration) {
                
                // Create a test message
                Msg test_msg;
                test_msg.seq = sequence++;
                test_msg.px = 100.0 + (sequence % 100) * 0.01;  // Simple price variation
                test_msg.qty = (sequence % 2 == 0) ? 100 : -100;  // Alternating buy/sell
                
                // Send the message
                boost::system::error_code ec;
                write(socket, buffer(&test_msg, sizeof(test_msg)), ec);
                
                if (ec) {
                    std::cout << "Send error: " << ec.message() << std::endl;
                    break;
                }
                
                messages_sent++;
                
                // Rate limiting
                if (rate > 0) {
                    auto sleep_time = std::chrono::microseconds(1000000 / rate);
                    std::this_thread::sleep_for(sleep_time);
                }
            }
            
            std::cout << "Sent " << messages_sent << " messages" << std::endl;
            socket.close();
        }
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    const std::uint16_t port = 9002;
    const std::uint32_t rate = 1000;  // messages per second
    const std::uint32_t duration = 10; // seconds
    
    std::cout << "=== Simple Kernel Bypass Test ===" << std::endl;
    std::cout << "Testing Boost.Asio bypass client implementation" << std::endl;
    
    // Start server in background thread
    std::thread server_thread([=]() {
        run_test_server(port, rate, duration);
    });
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Setup bypass client
    BypassConfig config;
    config.backend = BypassBackend::BOOST_ASIO;
    config.host = "127.0.0.1";
    config.port = port;
    config.interface_name = "lo";
    config.rx_ring_size = 1024;
    config.batch_size = 16;
    config.enable_zero_copy = false;
    config.poll_timeout_us = 1000;
    
    RingBuffer ring(4096);
    IngestionStats stats;
    BypassIngestionClient client(config);
    
    // Initialize and connect
    if (!client.initialize()) {
        std::cerr << "Failed to initialize bypass client" << std::endl;
        stop_server = true;
        server_thread.join();
        return 1;
    }
    
    std::cout << "Using backend: " << client.backend_info() << std::endl;
    
    if (!client.connect()) {
        std::cerr << "Failed to connect to test server" << std::endl;
        stop_server = true;
        server_thread.join();
        return 1;
    }
    
    std::cout << "Connected successfully!" << std::endl;
    
    // Start ingestion
    client.start_ingestion(ring, stats);
    
    // Consumer loop
    std::cout << "Starting message consumption..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();
    std::uint64_t messages_processed = 0;
    Slot slot;
    
    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start_time).count() < duration + 2) {
        
        if (ring.try_pop(slot)) {
            messages_processed++;
            stats.record_message_processed(slot);
            
            if (messages_processed % 100 == 0) {
                std::cout << "Processed " << messages_processed << " messages" << std::endl;
            }
        }
        
        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    // Stop and collect results
    client.stop_ingestion();
    client.disconnect();
    
    stop_server = true;
    server_thread.join();
    
    // Print results
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Backend: " << client.backend_info() << std::endl;
    std::cout << "Duration: " << elapsed << " seconds" << std::endl;
    std::cout << "Messages processed: " << messages_processed << std::endl;
    std::cout << "Packets received: " << client.packets_received() << std::endl;
    std::cout << "Bytes received: " << client.bytes_received() << std::endl;
    std::cout << "Packets dropped: " << client.packets_dropped() << std::endl;
    
    if (messages_processed > 0) {
        std::cout << "Message rate: " << (messages_processed / elapsed) << " msgs/sec" << std::endl;
        std::cout << "✓ SUCCESS: Kernel bypass client is working!" << std::endl;
    } else {
        std::cout << "✗ FAILED: No messages received" << std::endl;
        return 1;
    }
    
    return 0;
} 