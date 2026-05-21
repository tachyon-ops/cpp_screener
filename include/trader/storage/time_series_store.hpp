#pragma once

#include "trader/core/circular_buffer.hpp"
#include "trader/core/types.hpp"
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <string>

namespace trader {
namespace storage {

class TimeSeries {
public:
    explicit TimeSeries(size_t max_bars = 2000);
    ~TimeSeries() = default;

    // Append tick and auto-aggregate into 1-minute and 5-minute bars
    void append_tick(const core::Tick& t);
    
    // Manual append of bars (e.g. for pre-populating historical data)
    void append_bar(const core::Bar& b, core::Resolution r);

    // Fetch historical data
    std::vector<core::Bar> last_n(core::Resolution r, size_t n) const;
    std::optional<core::Bar> latest(core::Resolution r) const;
    std::vector<core::Tick> get_ticks() const;

    // Helper to get latest price from ticks or bars
    double get_latest_price() const;

private:
    void aggregate_tick(const core::Tick& t);

    core::CircularBuffer<core::Bar> daily_;
    core::CircularBuffer<core::Bar> hourly_;
    core::CircularBuffer<core::Bar> h4_;
    core::CircularBuffer<core::Bar> w1_;
    core::CircularBuffer<core::Bar> min5_;
    core::CircularBuffer<core::Bar> min1_;
    core::CircularBuffer<core::Tick> ticks_;
    mutable std::shared_mutex mu_;

    // Real-time aggregation state
    std::optional<core::Bar> current_m1_;
    std::optional<core::Bar> current_m5_;
};

class TimeSeriesStore {
public:
    TimeSeriesStore() = default;
    ~TimeSeriesStore() = default;

    // Disable copy
    TimeSeriesStore(const TimeSeriesStore&) = delete;
    TimeSeriesStore& operator=(const TimeSeriesStore&) = delete;

    // Returns reference to TimeSeries, creates one if it doesn't exist
    std::shared_ptr<TimeSeries> get_or_create(const std::string& symbol);
    
    // Returns pointer to TimeSeries, or nullptr if not found
    std::shared_ptr<TimeSeries> get(const std::string& symbol) const;

    // Pre-populate TimeSeries with mock historical intraday bars
    void pre_populate(const std::string& symbol, double current_price);

private:
    std::unordered_map<std::string, std::shared_ptr<TimeSeries>> series_;
    mutable std::shared_mutex mu_;
};

} // namespace storage
} // namespace trader
