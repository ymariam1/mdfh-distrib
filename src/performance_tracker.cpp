#include "mdfh/performance_tracker.hpp"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <stdexcept>

#if defined(MDFH_HAVE_PERF_EVENT_H)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mdfh {

PerformanceTracker::PerformanceTracker(const PerformanceConfig& config)
    : config_(config) {
    
    // Pre-allocate circular buffer for timestamp samples (ZERO ALLOCATION IN HOT PATH)
    if (config_.enable_detailed_latency && config_.max_samples > 0) {
        // Ensure max_samples is power of 2 for efficient masking
        std::uint32_t capacity = config_.max_samples;
        if (!is_power_of_two(capacity)) {
            // Round up to next power of 2
            capacity = 1;
            while (capacity < config_.max_samples) {
                capacity <<= 1;
            }
        }
        
        timestamp_samples_.resize(capacity);
        sample_mask_ = capacity - 1;
        
        MDFH_LOG_DEBUG("PerformanceTracker", 
                      "Pre-allocated " + std::to_string(capacity) + " timestamp samples");
    }
    
    if (config_.enable_cache_analysis) {
        initialize();
    }
}

PerformanceTracker::~PerformanceTracker() {
    if (perf_fd_) {
        close_perf_counters();
    }
}

bool PerformanceTracker::initialize() {
#if defined(MDFH_HAVE_PERF_EVENT_H)
    return setup_perf_counters();
#else
    if (config_.enable_cache_analysis) {
        std::cerr << "Warning: Hardware performance counters not supported on this platform" << std::endl;
        config_.enable_cache_analysis = false;
    }
    return false;
#endif
}

void PerformanceTracker::record_timestamp(const StageTimestamps& timestamps) {
    if (!config_.enable_detailed_latency || timestamp_samples_.empty()) {
        return;
    }
    
    // Only sample based on sampling rate (ZERO ALLOCATION)
    auto count = sample_count_.fetch_add(1, std::memory_order_relaxed);
    if (count % config_.sampling_rate != 0) {
        return;
    }
    
    // Write to pre-allocated circular buffer (ZERO ALLOCATION)
    auto write_pos = sample_write_pos_.fetch_add(1, std::memory_order_relaxed);
    timestamp_samples_[write_pos & sample_mask_] = timestamps;
}

std::vector<StageTimestamps> PerformanceTracker::get_current_samples() const {
    if (timestamp_samples_.empty()) {
        return {};
    }
    
    auto total_written = sample_write_pos_.load(std::memory_order_acquire);
    auto samples_to_read = std::min(total_written, static_cast<std::uint64_t>(timestamp_samples_.size()));
    
    std::vector<StageTimestamps> result;
    result.reserve(samples_to_read);
    
    // Read from circular buffer
    for (std::uint64_t i = 0; i < samples_to_read; ++i) {
        auto read_pos = (total_written - samples_to_read + i) & sample_mask_;
        result.push_back(timestamp_samples_[read_pos]);
    }
    
    return result;
}

void PerformanceTracker::update_cache_stats() {
    if (!config_.enable_cache_analysis || !perf_fd_) {
        return;
    }
    
#if defined(MDFH_HAVE_PERF_EVENT_H)
    read_perf_counters();
#endif
}

PerformanceTracker::LatencyStats PerformanceTracker::get_latency_stats() const {
    LatencyStats stats{};
    
    // Get current samples from circular buffer
    auto samples = get_current_samples();
    stats.samples = samples.size();
    
    if (stats.samples == 0) {
        return stats;
    }
    
    // Calculate latencies for each stage
    std::vector<double> latencies;
    latencies.reserve(stats.samples);
    
    for (const auto& ts : samples) {
        // Total latency from packet reception to processing end
        double latency = (ts.process_end - ts.packet_rx) / 1000.0;  // Convert to microseconds
        latencies.push_back(latency);
    }
    
    // Sort latencies for percentile calculation
    std::sort(latencies.begin(), latencies.end());
    
    // Calculate percentiles
    stats.p50 = calculate_percentile(latencies, 0.50);
    stats.p90 = calculate_percentile(latencies, 0.90);
    stats.p95 = calculate_percentile(latencies, 0.95);
    stats.p99 = calculate_percentile(latencies, 0.99);
    stats.p99_9 = calculate_percentile(latencies, 0.999);
    
    // Calculate mean
    stats.mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    
    return stats;
}

PerformanceTracker::CacheMetrics PerformanceTracker::get_cache_metrics() const {
    CacheMetrics metrics{};
    
    if (!config_.enable_cache_analysis) {
        return metrics;
    }
    
    std::uint64_t total_refs = cache_stats_.cache_references.load();
    if (total_refs == 0) {
        return metrics;
    }
    
    metrics.total_references = total_refs;
    metrics.l1_miss_rate = static_cast<double>(cache_stats_.l1_misses.load()) / total_refs;
    metrics.l2_miss_rate = static_cast<double>(cache_stats_.l2_misses.load()) / total_refs;
    metrics.l3_miss_rate = static_cast<double>(cache_stats_.l3_misses.load()) / total_refs;
    
    return metrics;
}

void PerformanceTracker::print_performance_report() const {
    std::cout << "\n=== Performance Analysis Report ===\n";
    
    if (config_.enable_detailed_latency) {
        auto latency_stats = get_latency_stats();
        std::cout << "\nLatency Statistics (microseconds):\n";
        std::cout << "  Samples: " << latency_stats.samples << "\n";
        std::cout << "  Mean: " << std::fixed << std::setprecision(2) << latency_stats.mean << "µs\n";
        std::cout << "  P50: " << latency_stats.p50 << "µs\n";
        std::cout << "  P90: " << latency_stats.p90 << "µs\n";
        std::cout << "  P95: " << latency_stats.p95 << "µs\n";
        std::cout << "  P99: " << latency_stats.p99 << "µs\n";
        std::cout << "  P99.9: " << latency_stats.p99_9 << "µs\n";
    }
    
    if (config_.enable_cache_analysis) {
        auto cache_metrics = get_cache_metrics();
        std::cout << "\nCache Performance:\n";
        std::cout << "  Total References: " << cache_metrics.total_references << "\n";
        std::cout << "  L1 Miss Rate: " << std::fixed << std::setprecision(4) 
                  << (cache_metrics.l1_miss_rate * 100) << "%\n";
        std::cout << "  L2 Miss Rate: " << (cache_metrics.l2_miss_rate * 100) << "%\n";
        std::cout << "  L3 Miss Rate: " << (cache_metrics.l3_miss_rate * 100) << "%\n";
    }
}

#if defined(MDFH_HAVE_PERF_EVENT_H)
bool PerformanceTracker::setup_perf_counters() {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_HW;
    pe.size = sizeof(pe);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    
    // Set up L1 cache misses
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    pe.config1 = PERF_COUNT_HW_CACHE_L1D;
    perf_fd_ = reinterpret_cast<void*>(syscall(SYS_perf_event_open, &pe, 0, -1, -1, 0));
    
    if (perf_fd_ == nullptr) {
        std::cerr << "Failed to set up performance counters" << std::endl;
        return false;
    }
    
    // Enable the counters
    ioctl(reinterpret_cast<int>(perf_fd_), PERF_EVENT_IOC_ENABLE, 0);
    return true;
}

void PerformanceTracker::read_perf_counters() {
    if (!perf_fd_) return;
    
    std::uint64_t count;
    if (read(reinterpret_cast<int>(perf_fd_), &count, sizeof(count)) == sizeof(count)) {
        cache_stats_.l1_misses.store(count, std::memory_order_relaxed);
    }
}

void PerformanceTracker::close_perf_counters() {
    if (perf_fd_) {
        ioctl(reinterpret_cast<int>(perf_fd_), PERF_EVENT_IOC_DISABLE, 0);
        close(reinterpret_cast<int>(perf_fd_));
        perf_fd_ = nullptr;
    }
}
#else
bool PerformanceTracker::setup_perf_counters() { return false; }
void PerformanceTracker::read_perf_counters() {}
void PerformanceTracker::close_perf_counters() {}
#endif

double PerformanceTracker::calculate_percentile(const std::vector<double>& latencies, double percentile) const {
    if (latencies.empty()) return 0.0;
    
    size_t index = static_cast<size_t>(latencies.size() * percentile);
    if (index >= latencies.size()) {
        index = latencies.size() - 1;
    }
    return latencies[index];
}

} // namespace mdfh 