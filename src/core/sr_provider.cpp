#include "trader/core/sr_provider.hpp"
#include "trader/storage/time_series_store.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace trader {
namespace core {

// AlgorithmicSRProvider Implementation
AlgorithmicSRProvider::AlgorithmicSRProvider(std::shared_ptr<storage::TimeSeriesStore> ts_store)
    : ts_store_(std::move(ts_store)) {}

std::vector<SRLevel> AlgorithmicSRProvider::get_levels(const std::string& symbol) {
    std::vector<SRLevel> levels;
    if (!ts_store_) return levels;

    auto ts = ts_store_->get(symbol);
    if (!ts) return levels;

    double latest_price = ts->get_latest_price();
    if (latest_price <= 0.0) return levels;

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 1. Fetch bars for calculation
    auto daily_bars = ts->last_n(Resolution::D1, 250);
    auto h4_bars = ts->last_n(Resolution::H4, 200);

    // Use H4 bars for touch count if available, otherwise daily
    const auto& touch_bars = h4_bars.empty() ? daily_bars : h4_bars;

    // Helper to calculate touch count for a given price level
    auto calc_touches = [&](double price) {
        int touches = 0;
        double tol = price * 0.0075; // 0.75% tolerance
        for (const auto& bar : touch_bars) {
            if (bar.low.value - tol <= price && price <= bar.high.value + tol) {
                touches++;
            }
        }
        return touches;
    };

    std::vector<SRLevel> candidates;

    // 2. Swing Points (K-bar window)
    auto extract_swings = [&](const std::vector<Bar>& bars, int K, const std::string& timeframe) {
        if (bars.size() < static_cast<size_t>(2 * K + 1)) return;
        for (size_t i = K; i < bars.size() - K; ++i) {
            bool is_high = true;
            bool is_low = true;
            for (int j = -K; j <= K; ++j) {
                if (j == 0) continue;
                if (bars[i].high.value < bars[i + j].high.value) is_high = false;
                if (bars[i].low.value > bars[i + j].low.value) is_low = false;
            }
            if (is_high) {
                candidates.push_back({bars[i].high.value, "swing_" + timeframe, 0, now_ms});
            }
            if (is_low) {
                candidates.push_back({bars[i].low.value, "swing_" + timeframe, 0, now_ms});
            }
        }
    };

    extract_swings(daily_bars, 5, "D1");
    extract_swings(h4_bars, 10, "H4");

    // 3. Moving Averages (200 SMA/EMA)
    if (daily_bars.size() >= 200) {
        // SMA 200
        double sum = 0.0;
        for (size_t i = daily_bars.size() - 200; i < daily_bars.size(); ++i) {
            sum += daily_bars[i].close.value;
        }
        double sma_200 = sum / 200.0;
        candidates.push_back({sma_200, "ma_sma200", 0, now_ms});

        // EMA 200
        double alpha = 2.0 / (200.0 + 1.0);
        // Initialize EMA with SMA of the first 200 bars in the vector
        double ema_200 = 0.0;
        for (size_t i = 0; i < 200; ++i) {
            ema_200 += daily_bars[i].close.value;
        }
        ema_200 /= 200.0;
        for (size_t i = 200; i < daily_bars.size(); ++i) {
            ema_200 = daily_bars[i].close.value * alpha + ema_200 * (1.0 - alpha);
        }
        candidates.push_back({ema_200, "ma_ema200", 0, now_ms});
    }

    // 4. Fibonacci Retracements (on last 100 days)
    if (!daily_bars.empty()) {
        size_t fib_lookback = std::min(daily_bars.size(), static_cast<size_t>(100));
        double highest = daily_bars[daily_bars.size() - fib_lookback].high.value;
        double lowest = daily_bars[daily_bars.size() - fib_lookback].low.value;
        for (size_t i = daily_bars.size() - fib_lookback; i < daily_bars.size(); ++i) {
            highest = std::max(highest, daily_bars[i].high.value);
            lowest = std::min(lowest, daily_bars[i].low.value);
        }
        double range = highest - lowest;
        if (range > 0.0) {
            std::vector<double> ratios = {0.236, 0.382, 0.500, 0.618, 0.786};
            candidates.push_back({lowest, "fib_0.0", 0, now_ms});
            candidates.push_back({highest, "fib_1.0", 0, now_ms});
            for (double r : ratios) {
                double price = lowest + r * range;
                std::string label = "fib_" + std::to_string(r).substr(0, 5);
                candidates.push_back({price, label, 0, now_ms});
            }
        }
    }

    // 5. Round Numbers
    double base = std::pow(10.0, std::floor(std::log10(latest_price)));
    double step = base / 10.0;
    if (latest_price / step > 50.0) {
        step *= 5.0;
    }
    double nearest = std::round(latest_price / step) * step;
    for (int i = -4; i <= 4; ++i) {
        double price = nearest + i * step;
        if (price > 0.0) {
            candidates.push_back({price, "round", 0, now_ms});
        }
    }

    // 6. Calculate touch counts and filter candidates
    for (auto& cand : candidates) {
        cand.touch_count = calc_touches(cand.price);
    }

    // Sort by price and deduplicate candidates within 1% proximity internally
    std::sort(candidates.begin(), candidates.end(), [](const SRLevel& a, const SRLevel& b) {
        return a.price < b.price;
    });

    for (const auto& lvl : candidates) {
        if (levels.empty()) {
            levels.push_back(lvl);
        } else {
            auto& last = levels.back();
            double proximity = std::abs(lvl.price - last.price) / last.price;
            if (proximity <= 0.01) {
                // Proximity overlap, keep the one with higher touch count
                if (lvl.touch_count > last.touch_count) {
                    last = lvl;
                }
            } else {
                levels.push_back(lvl);
            }
        }
    }

    return levels;
}

// FileBasedSRProvider Implementation
FileBasedSRProvider::FileBasedSRProvider(const std::string& folder_path)
    : folder_path_(folder_path) {}

std::vector<SRLevel> FileBasedSRProvider::get_levels(const std::string& symbol) {
    std::vector<SRLevel> levels;
    std::string path = folder_path_ + "/" + symbol + ".json";
    std::ifstream f(path);
    if (!f.is_open()) {
        // If file doesn't exist, just return empty vector silently or log
        return levels;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    try {
        nlohmann::json j;
        f >> j;
        if (j.is_array()) {
            for (const auto& item : j) {
                SRLevel lvl;
                lvl.price = item.value("price", 0.0);
                lvl.source = item.value("source", "file");
                lvl.touch_count = item.value("touch_count", 0);
                lvl.last_updated_ms = item.value("last_updated_ms", now_ms);
                if (lvl.price > 0.0) {
                    levels.push_back(lvl);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[FileBasedSRProvider] Error reading " << path << ": " << e.what() << std::endl;
    }

    return levels;
}

// CompositeSRProvider Implementation
CompositeSRProvider::CompositeSRProvider(std::vector<std::shared_ptr<ISupportResistanceProvider>> providers, double proximity_pct)
    : providers_(std::move(providers)), proximity_pct_(proximity_pct) {}

std::vector<SRLevel> CompositeSRProvider::get_levels(const std::string& symbol) {
    std::vector<SRLevel> all_levels;
    for (const auto& prov : providers_) {
        if (!prov) continue;
        auto levels = prov->get_levels(symbol);
        all_levels.insert(all_levels.end(), levels.begin(), levels.end());
    }

    if (all_levels.empty()) return all_levels;

    // Sort by price
    std::sort(all_levels.begin(), all_levels.end(), [](const SRLevel& a, const SRLevel& b) {
        return a.price < b.price;
    });

    // Deduplicate by proximity
    std::vector<SRLevel> merged;
    for (const auto& lvl : all_levels) {
        if (merged.empty()) {
            merged.push_back(lvl);
        } else {
            auto& last = merged.back();
            double proximity = std::abs(lvl.price - last.price) / last.price;
            if (proximity <= proximity_pct_) {
                // Merge proximity levels: keep the level with higher touch count
                if (lvl.touch_count > last.touch_count) {
                    last = lvl;
                }
            } else {
                merged.push_back(lvl);
            }
        }
    }

    return merged;
}

} // namespace core
} // namespace trader
