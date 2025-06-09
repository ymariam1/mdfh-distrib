#include "mdfh/multi_feed_ingestion.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <vector>
#include <string>

int main(int argc, char* argv[]) {
    CLI::App app{"MDFH Multi-Feed Ingestion Benchmark"};
    
    // Configuration options
    std::string config_file;
    std::vector<std::string> feed_specs;
    std::uint32_t max_seconds = 0;
    std::uint64_t max_messages = 0;
    std::uint32_t global_buffer_capacity = 262144;
    
    // CLI options
    app.add_option("-c,--config", config_file, "YAML configuration file");
    app.add_option("-f,--feed", feed_specs, "Feed specification (host:port), can be repeated")
        ->expected(-1); // Allow multiple values
    app.add_option("-t,--time", max_seconds, "Maximum runtime in seconds (0 = infinite)");
    app.add_option("-m,--messages", max_messages, "Maximum messages to process (0 = infinite)");
    app.add_option("-b,--buffer", global_buffer_capacity, "Global buffer capacity (power of 2)");
    
    CLI11_PARSE(app, argc, argv);
    
    try {
        mdfh::MultiFeedConfig config;
        
        // Load configuration
        if (!config_file.empty()) {
            std::cout << "Loading configuration from: " << config_file << std::endl;
            config = mdfh::MultiFeedConfig::from_yaml(config_file);
        } else if (!feed_specs.empty()) {
            std::cout << "Using CLI feed specifications" << std::endl;
            config = mdfh::MultiFeedConfig::from_cli_feeds(feed_specs);
        } else {
            std::cerr << "Error: Must specify either --config or --feed options" << std::endl;
            return 1;
        }
        
        // Override with CLI options if provided
        if (max_seconds > 0) {
            config.max_seconds = max_seconds;
        }
        if (max_messages > 0) {
            config.max_messages = max_messages;
        }
        if (global_buffer_capacity != 262144) {
            config.global_buffer_capacity = global_buffer_capacity;
        }
        
        // Validate configuration
        if (!config.is_valid()) {
            std::cerr << "Error: Invalid configuration" << std::endl;
            return 1;
        }
        
        // Print configuration summary
        std::cout << "\n=== Multi-Feed Configuration ===" << std::endl;
        std::cout << "Number of feeds: " << config.feeds.size() << std::endl;
        std::cout << "Global buffer capacity: " << config.global_buffer_capacity << std::endl;
        std::cout << "Health check interval: " << config.health_check_interval_ms << "ms" << std::endl;
        if (config.max_seconds > 0) {
            std::cout << "Max runtime: " << config.max_seconds << " seconds" << std::endl;
        }
        if (config.max_messages > 0) {
            std::cout << "Max messages: " << config.max_messages << std::endl;
        }
        
        std::cout << "\nFeeds:" << std::endl;
        for (const auto& feed : config.feeds) {
            std::cout << "  - " << feed.name << " [" << feed.host << ":" << feed.port << "] "
                      << (feed.is_primary ? "(PRIMARY)" : "(BACKUP)") << std::endl;
        }
        
        // Run benchmark
        std::cout << "\nStarting multi-feed ingestion benchmark..." << std::endl;
        mdfh::MultiFeedIngestionBenchmark benchmark(std::move(config));
        benchmark.run();
        
        std::cout << "\nBenchmark completed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
} 