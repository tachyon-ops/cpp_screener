#include "trader/storage/time_series_store.hpp"
#include <random>
#include <chrono>
#include <algorithm>
#include <iostream>

namespace trader {
namespace storage {

TimeSeries::TimeSeries(size_t max_bars)
    : daily_(500), hourly_(1000), min5_(max_bars), min1_(max_bars), ticks_(1000) {}

void TimeSeries::append_tick(const core::Tick& t) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    ticks_.push_back(t);
    aggregate_tick(t);
}

void TimeSeries::aggregate_tick(const core::Tick& t) {
    double price = (t.bid.value + t.ask.value) / 2.0;
    uint64_t ts_ms = t.ts.ms_since_epoch;

    // 1-minute aggregation
    uint64_t m1_key = (ts_ms / 60000) * 60000;
    if (!current_m1_) {
        core::Bar b;
        b.ts.ms_since_epoch = m1_key;
        b.open.value = price;
        b.high.value = price;
        b.low.value = price;
        b.close.value = price;
        b.volume.value = 100.0; // Simulated volume increment per tick
        current_m1_ = b;
    } else if (current_m1_->ts.ms_since_epoch == m1_key) {
        current_m1_->high.value = std::max(current_m1_->high.value, price);
        current_m1_->low.value = std::min(current_m1_->low.value, price);
        current_m1_->close.value = price;
        current_m1_->volume.value += 100.0;
    } else {
        // Complete current M1 bar and push
        min1_.push_back(*current_m1_);
        
        // Start new M1 bar
        core::Bar b;
        b.ts.ms_since_epoch = m1_key;
        b.open.value = price;
        b.high.value = price;
        b.low.value = price;
        b.close.value = price;
        b.volume.value = 100.0;
        current_m1_ = b;
    }

    // 5-minute aggregation
    uint64_t m5_key = (ts_ms / 300000) * 300000;
    if (!current_m5_) {
        core::Bar b;
        b.ts.ms_since_epoch = m5_key;
        b.open.value = price;
        b.high.value = price;
        b.low.value = price;
        b.close.value = price;
        b.volume.value = 100.0;
        current_m5_ = b;
    } else if (current_m5_->ts.ms_since_epoch == m5_key) {
        current_m5_->high.value = std::max(current_m5_->high.value, price);
        current_m5_->low.value = std::min(current_m5_->low.value, price);
        current_m5_->close.value = price;
        current_m5_->volume.value += 100.0;
    } else {
        // Complete current M5 bar and push
        min5_.push_back(*current_m5_);
        
        // Start new M5 bar
        core::Bar b;
        b.ts.ms_since_epoch = m5_key;
        b.open.value = price;
        b.high.value = price;
        b.low.value = price;
        b.close.value = price;
        b.volume.value = 100.0;
        current_m5_ = b;
    }
}

void TimeSeries::append_bar(const core::Bar& b, core::Resolution r) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (r == core::Resolution::M1) {
        min1_.push_back(b);
    } else if (r == core::Resolution::M5) {
        min5_.push_back(b);
    } else if (r == core::Resolution::H1) {
        hourly_.push_back(b);
    } else if (r == core::Resolution::D1) {
        daily_.push_back(b);
    }
}

std::vector<core::Bar> TimeSeries::last_n(core::Resolution r, size_t n) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<core::Bar> res;
    const auto* buffer = &min5_;
    if (r == core::Resolution::M1) buffer = &min1_;
    else if (r == core::Resolution::M5) buffer = &min5_;
    else if (r == core::Resolution::H1) buffer = &hourly_;
    else if (r == core::Resolution::D1) buffer = &daily_;

    size_t sz = buffer->size();
    size_t count = std::min(n, sz);
    res.reserve(count);
    for (size_t i = sz - count; i < sz; ++i) {
        res.push_back((*buffer)[i]);
    }
    return res;
}

std::optional<core::Bar> TimeSeries::latest(core::Resolution r) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    const auto* buffer = &min5_;
    if (r == core::Resolution::M1) buffer = &min1_;
    else if (r == core::Resolution::M5) buffer = &min5_;
    else if (r == core::Resolution::H1) buffer = &hourly_;
    else if (r == core::Resolution::D1) buffer = &daily_;

    if (buffer->empty()) return std::nullopt;
    return buffer->back();
}

std::vector<core::Tick> TimeSeries::get_ticks() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<core::Tick> res;
    size_t sz = ticks_.size();
    res.reserve(sz);
    for (size_t i = 0; i < sz; ++i) {
        res.push_back(ticks_[i]);
    }
    return res;
}

double TimeSeries::get_latest_price() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!ticks_.empty()) {
        const auto& t = ticks_.back();
        return (t.bid.value + t.ask.value) / 2.0;
    }
    if (!min1_.empty()) {
        return min1_.back().close.value;
    }
    if (!min5_.empty()) {
        return min5_.back().close.value;
    }
    return 0.0;
}

std::shared_ptr<TimeSeries> TimeSeriesStore::get_or_create(const std::string& symbol) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    auto it = series_.find(symbol);
    if (it != series_.end()) {
        return it->second;
    }
    auto ts = std::make_shared<TimeSeries>();
    series_[symbol] = ts;
    return ts;
}

std::shared_ptr<TimeSeries> TimeSeriesStore::get(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = series_.find(symbol);
    if (it != series_.end()) {
        return it->second;
    }
    return nullptr;
}

void TimeSeriesStore::pre_populate(const std::string& symbol, double current_price) {
    auto ts = get_or_create(symbol);
    
    // Clear the buffers first
    // Note: The CircularBuffer itself doesn't have a resize, but we can clear by calling ts's private clear if we exposed it,
    // or just let pre_populate construct a new one. Since ts is fresh or we want to overwrite, let's just push bars.
    // In our case we generate 200 bars going backward.
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::random_device rd;
    std::mt19937 gen(rd() ^ std::hash<std::string>()(symbol));
    std::normal_distribution<> pct_change(0.00005, 0.001); // slightly upward random walk
    
    double price = current_price;
    std::vector<core::Bar> mock_bars;
    
    // Generate 200 bars going backward
    for (int i = 200; i >= 1; --i) {
        price *= (1.0 + pct_change(gen));
        core::Bar b;
        b.ts.ms_since_epoch = now_ms - (i * 5 * 60 * 1000);
        b.open.value = price * (1.0 - 0.001);
        b.high.value = price * (1.0 + 0.002);
        b.low.value = price * (1.0 - 0.002);
        b.close.value = price;
        b.volume.value = 1000.0 + (gen() % 9000);
        mock_bars.push_back(b);
    }
    
    for (const auto& b : mock_bars) {
        ts->append_bar(b, core::Resolution::M5);
    }

    // Also populate 100 1-minute bars
    price = current_price;
    std::vector<core::Bar> mock_m1_bars;
    for (int i = 100; i >= 1; --i) {
        price *= (1.0 + pct_change(gen));
        core::Bar b;
        b.ts.ms_since_epoch = now_ms - (i * 1 * 60 * 1000);
        b.open.value = price * (1.0 - 0.0005);
        b.high.value = price * (1.0 + 0.001);
        b.low.value = price * (1.0 - 0.001);
        b.close.value = price;
        b.volume.value = 200.0 + (gen() % 1800);
        mock_m1_bars.push_back(b);
    }
    for (const auto& b : mock_m1_bars) {
        ts->append_bar(b, core::Resolution::M1);
    }
    
    std::cout << "[TimeSeriesStore] Pre-populated " << symbol << " with 200 M5 bars and 100 M1 bars at base price $" << current_price << std::endl;
}

} // namespace storage
} // namespace trader
