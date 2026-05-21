#include "trader/storage/time_series_store.hpp"
#include <random>
#include <chrono>
#include <algorithm>
#include <iostream>

namespace trader {
namespace storage {

TimeSeries::TimeSeries(size_t max_bars)
    : daily_(500), hourly_(1000), h4_(1000), w1_(500), min5_(max_bars), min1_(max_bars), ticks_(1000) {}

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
    } else if (r == core::Resolution::H4) {
        h4_.push_back(b);
    } else if (r == core::Resolution::D1) {
        daily_.push_back(b);
    } else if (r == core::Resolution::W1) {
        w1_.push_back(b);
    }
}

std::vector<core::Bar> TimeSeries::last_n(core::Resolution r, size_t n) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<core::Bar> res;
    const auto* buffer = &min5_;
    if (r == core::Resolution::M1) buffer = &min1_;
    else if (r == core::Resolution::M5) buffer = &min5_;
    else if (r == core::Resolution::H1) buffer = &hourly_;
    else if (r == core::Resolution::H4) buffer = &h4_;
    else if (r == core::Resolution::D1) buffer = &daily_;
    else if (r == core::Resolution::W1) buffer = &w1_;

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
    else if (r == core::Resolution::H4) buffer = &h4_;
    else if (r == core::Resolution::D1) buffer = &daily_;
    else if (r == core::Resolution::W1) buffer = &w1_;

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
    
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::random_device rd;
    std::mt19937 gen(rd() ^ std::hash<std::string>()(symbol));
    
    // Helper lambda to generate and push bars going backward
    auto generate_and_push = [&](core::Resolution r, int count, uint64_t interval_ms, double walk_stddev) {
        std::normal_distribution<> local_walk(0.00005, walk_stddev);
        double price = current_price;
        std::vector<core::Bar> mock_bars;
        mock_bars.reserve(count);
        for (int i = count; i >= 1; --i) {
            price *= (1.0 + local_walk(gen));
            core::Bar b;
            b.ts.ms_since_epoch = now_ms - (i * interval_ms);
            b.open.value = price * (1.0 - 0.001);
            b.high.value = price * (1.0 + 0.002);
            b.low.value = price * (1.0 - 0.002);
            b.close.value = price;
            b.volume.value = 1000.0 + (gen() % 9000);
            mock_bars.push_back(b);
        }
        for (const auto& b : mock_bars) {
            ts->append_bar(b, r);
        }
    };

    // Populate buffers
    generate_and_push(core::Resolution::M1, 100, 60 * 1000, 0.0005);
    generate_and_push(core::Resolution::M5, 200, 5 * 60 * 1000, 0.001);
    generate_and_push(core::Resolution::H1, 200, 60 * 60 * 1000, 0.002);
    generate_and_push(core::Resolution::H4, 150, 4 * 60 * 60 * 1000, 0.004);
    generate_and_push(core::Resolution::D1, 150, 24 * 60 * 60 * 1000, 0.008);
    generate_and_push(core::Resolution::W1, 50, 7 * 24 * 60 * 60 * 1000, 0.015);

    std::cout << "[TimeSeriesStore] Pre-populated " << symbol << " for all resolutions (M1-W1) at base price $" << current_price << std::endl;
}

} // namespace storage
} // namespace trader
