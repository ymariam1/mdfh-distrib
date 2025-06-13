#pragma once

#include "core.hpp"
#include "timing.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace mdfh {

// Stage timestamps for detailed latency analysis
struct alignas(64) StageTimestamps {
    std::uint64_t packet_rx;      // Hardware timestamp when packet received
    std::uint64_t parse_start;    // Start of message parsing
    std::uint64_t parse_end;      // End of message parsing
    std::uint64_t ring_push;      // When message pushed to ring buffer
    std::uint64_t ring_pop;       // When message popped from ring buffer
    std::uint64_t process_end;    // End of message processing
    
    // Padding to ensure cache line alignment
    char padding[8];
};

// Cache performance counters
struct alignas(64) CacheStats {
    std::atomic<std::uint64_t> l1_misses{0};
    std::atomic<std::uint64_t> l2_misses{0};
    std::atomic<std::uint64_t> l3_misses{0};
    std::atomic<std::uint64_t> cache_references{0};
    
    // Padding to ensure cache line alignment
    char padding[32];
};

// Performance tracking configuration
struct PerformanceConfig {
    bool enable_hardware_timestamps = true;
    bool enable_cache_analysis = true;
    bool enable_detailed_latency = true;
    std::uint32_t sampling_rate = 1000;  // Sample every Nth packet
    std::uint32_t max_samples = 1000000; // Maximum samples to store (pre-allocated)
};

class PerformanceTracker {
private:
    PerformanceConfig config_;
    
    // Pre-allocated circular buffer for timestamp samples (ZERO ALLOCATION IN HOT PATH)
    std::vector<StageTimestamps> timestamp_samples_;
    alignas(64) std::atomic<std::uint64_t> sample_write_pos_{0};
    alignas(64) std::atomic<std::uint64_t> sample_count_{0};
    std::uint64_t sample_mask_;
    
    CacheStats cache_stats_;
    
    // Hardware performance counter handles
    void* perf_fd_{nullptr};
    
public:
    explicit PerformanceTracker(const PerformanceConfig& config = PerformanceConfig());
    ~PerformanceTracker();
    
    // Initialize hardware performance counters
    bool initialize();
    
    // Record timestamps for a processing stage (ZERO ALLOCATION - HOT PATH)
    void record_timestamp(const StageTimestamps& timestamps);
    
    // Update cache statistics
    void update_cache_stats();
    
    // Get latency statistics
    struct LatencyStats {
        double p50, p90, p95, p99, p99_9;
        double mean;
        std::uint64_t samples;
    };
    
    LatencyStats get_latency_stats() const;
    
    // Get cache statistics
    struct CacheMetrics {
        double l1_miss_rate;
        double l2_miss_rate;
        double l3_miss_rate;
        std::uint64_t total_references;
    };
    
    CacheMetrics get_cache_metrics() const;
    
    // Print detailed performance report
    void print_performance_report() const;
    
private:
    // Helper methods for hardware performance counters
    bool setup_perf_counters();
    void read_perf_counters();
    void close_perf_counters();
    
    // Calculate latency percentiles
    double calculate_percentile(const std::vector<double>& latencies, double percentile) const;
    
    // Get current samples for analysis (creates temporary vector)
    std::vector<StageTimestamps> get_current_samples() const;
};

} // namespace mdfh 