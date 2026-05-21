#include "trader/screens/screen_g.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include <iostream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace trader {
namespace screens {

namespace {
std::string ms_to_date_str(uint64_t ms) {
    auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    auto time = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&time));
    return buf;
}
}

ScreenG::ScreenG(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<storage::TimeSeriesStore> ts_store,
    std::shared_ptr<core::AlertDispatcher> dispatcher
) : store_(store), ts_store_(ts_store), dispatcher_(dispatcher) {
    load_config("./config/screens.yaml");
}

void ScreenG::load_config(const std::string& path) {
    std::ifstream infile(path);
    if (!infile.is_open()) {
        std::cout << "[ScreenG] Warning: Could not open screens config: " << path << ", using default pairs." << std::endl;
        pairs_ = {
            {"BTCUSD", "NDX"},
            {"ETHUSD", "NDX"},
            {"^N225", "^STOXX"}
        };
        return;
    }

    std::string line;
    bool in_screen_g = false;
    DivergencePair current_pair;
    bool in_pair = false;

    auto trim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    };

    while (std::getline(infile, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("screen_g:", 0) == 0) {
            in_screen_g = true;
            continue;
        } else if (line.find(":") != std::string::npos && line.front() != ' ' && line.front() != '\t' && line.rfind("screen_g", 0) != 0 && line.rfind("screen_c", 0) != 0) {
            in_screen_g = false;
        }

        if (in_screen_g) {
            if (line.rfind("correlation_period:", 0) == 0) {
                correlation_period_ = std::stoi(line.substr(19));
            } else if (line.rfind("zscore_period:", 0) == 0) {
                zscore_period_ = std::stoi(line.substr(14));
            } else if (line.rfind("zscore_threshold:", 0) == 0) {
                zscore_threshold_ = std::stod(line.substr(17));
            } else {
                size_t pos_a = line.find("symbol_a:");
                if (pos_a != std::string::npos) {
                    if (in_pair) {
                        pairs_.push_back(current_pair);
                        current_pair = DivergencePair();
                    }
                    current_pair.symbol_a = line.substr(pos_a + 9);
                    trim(current_pair.symbol_a);
                    if (!current_pair.symbol_a.empty() && (current_pair.symbol_a.front() == '"' || current_pair.symbol_a.front() == '\'')) {
                        current_pair.symbol_a = current_pair.symbol_a.substr(1, current_pair.symbol_a.size() - 2);
                    }
                    in_pair = true;
                }
                size_t pos_b = line.find("symbol_b:");
                if (pos_b != std::string::npos) {
                    current_pair.symbol_b = line.substr(pos_b + 9);
                    trim(current_pair.symbol_b);
                    if (!current_pair.symbol_b.empty() && (current_pair.symbol_b.front() == '"' || current_pair.symbol_b.front() == '\'')) {
                        current_pair.symbol_b = current_pair.symbol_b.substr(1, current_pair.symbol_b.size() - 2);
                    }
                }
            }
        }
    }
    if (in_screen_g && in_pair && !current_pair.symbol_a.empty() && !current_pair.symbol_b.empty()) {
        pairs_.push_back(current_pair);
    }
    std::cout << "[ScreenG] Config loaded: correlation_period=" << correlation_period_ 
              << ", zscore_period=" << zscore_period_ 
              << ", zscore_threshold=" << zscore_threshold_ 
              << ", loaded pairs=" << pairs_.size() << std::endl;
}

bool ScreenG::calculate_pair_divergence(
    const std::string& symbol_a, const std::string& symbol_b, 
    double& zscore_out, double& corr_out, 
    double& ret_a_out, double& ret_b_out, double& spread_out) const {

    struct PriceBar {
        std::string date;
        double close;
    };

    std::vector<PriceBar> pbars_a;
    std::vector<PriceBar> pbars_b;

    // 1. Fetch from Database
    auto opt_a = store_->get_instrument_by_symbol(symbol_a);
    if (opt_a) {
        auto db_bars = store_->get_bars_daily(opt_a->id);
        for (const auto& b : db_bars) {
            pbars_a.push_back({b.date, b.close});
        }
    }
    auto opt_b = store_->get_instrument_by_symbol(symbol_b);
    if (opt_b) {
        auto db_bars = store_->get_bars_daily(opt_b->id);
        for (const auto& b : db_bars) {
            pbars_b.push_back({b.date, b.close});
        }
    }

    // 2. Fetch from TimeSeriesStore as fallback/extension
    if (ts_store_) {
        auto ts_a = ts_store_->get(symbol_a);
        if (ts_a) {
            auto ts_bars = ts_a->last_n(Resolution::D1, 150);
            for (const auto& b : ts_bars) {
                std::string dt = ms_to_date_str(b.ts.ms_since_epoch);
                if (std::find_if(pbars_a.begin(), pbars_a.end(), [&](const PriceBar& x) { return x.date == dt; }) == pbars_a.end()) {
                    pbars_a.push_back({dt, b.close.value});
                }
            }
        }
        auto ts_b = ts_store_->get(symbol_b);
        if (ts_b) {
            auto ts_bars = ts_b->last_n(Resolution::D1, 150);
            for (const auto& b : ts_bars) {
                std::string dt = ms_to_date_str(b.ts.ms_since_epoch);
                if (std::find_if(pbars_b.begin(), pbars_b.end(), [&](const PriceBar& x) { return x.date == dt; }) == pbars_b.end()) {
                    pbars_b.push_back({dt, b.close.value});
                }
            }
        }
    }

    if (pbars_a.empty() || pbars_b.empty()) {
        return false;
    }

    // Sort bars by date
    auto sort_bars = [](std::vector<PriceBar>& bars) {
        std::sort(bars.begin(), bars.end(), [](const PriceBar& x, const PriceBar& y) {
            return x.date < y.date;
        });
    };
    sort_bars(pbars_a);
    sort_bars(pbars_b);

    // 3. Align by date
    std::unordered_map<std::string, double> map_a;
    std::unordered_map<std::string, double> map_b;
    for (const auto& b : pbars_a) map_a[b.date] = b.close;
    for (const auto& b : pbars_b) map_b[b.date] = b.close;

    std::vector<std::string> common_dates;
    for (const auto& [dt, _] : map_a) {
        if (map_b.find(dt) != map_b.end()) {
            common_dates.push_back(dt);
        }
    }
    std::sort(common_dates.begin(), common_dates.end());

    size_t num_days = common_dates.size();
    if (num_days < static_cast<size_t>(zscore_period_ + 1)) {
        return false;
    }

    // 4. Calculate Returns & Spreads
    std::vector<double> ret_a;
    std::vector<double> ret_b;
    std::vector<double> spreads;
    ret_a.reserve(num_days - 1);
    ret_b.reserve(num_days - 1);
    spreads.reserve(num_days - 1);

    for (size_t i = 1; i < num_days; ++i) {
        double prev_a = map_a[common_dates[i-1]];
        double curr_a = map_a[common_dates[i]];
        double r_a = (prev_a == 0.0) ? 0.0 : (curr_a - prev_a) / prev_a;

        double prev_b = map_b[common_dates[i-1]];
        double curr_b = map_b[common_dates[i]];
        double r_b = (prev_b == 0.0) ? 0.0 : (curr_b - prev_b) / prev_b;

        ret_a.push_back(r_a);
        ret_b.push_back(r_b);
        spreads.push_back(r_a - r_b);
    }

    size_t sz_ret = ret_a.size();
    if (sz_ret < static_cast<size_t>(zscore_period_)) {
        return false;
    }

    // 5. Pearson Correlation (over last 30 daily returns)
    size_t n_corr = std::min(sz_ret, static_cast<size_t>(correlation_period_));
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0, sum_y2 = 0.0;
    for (size_t i = sz_ret - n_corr; i < sz_ret; ++i) {
        double x = ret_a[i];
        double y = ret_b[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
        sum_y2 += y * y;
    }
    double num = n_corr * sum_xy - sum_x * sum_y;
    double den = std::sqrt((n_corr * sum_x2 - sum_x * sum_x) * (n_corr * sum_y2 - sum_y * sum_y));
    corr_out = (den == 0.0) ? 0.0 : num / den;

    // 6. Spread Z-score (over last 90 daily return spreads)
    size_t n_z = std::min(sz_ret, static_cast<size_t>(zscore_period_));
    double sum_s = 0.0;
    for (size_t i = sz_ret - n_z; i < sz_ret; ++i) {
        sum_s += spreads[i];
    }
    double mean_s = sum_s / n_z;

    double sum_sq_diff = 0.0;
    for (size_t i = sz_ret - n_z; i < sz_ret; ++i) {
        double diff = spreads[i] - mean_s;
        sum_sq_diff += diff * diff;
    }
    double stddev_s = std::sqrt(sum_sq_diff / (n_z - 1));
    if (stddev_s == 0.0) stddev_s = 0.0001; // Avoid division by zero

    double latest_spread = spreads.back();
    zscore_out = (latest_spread - mean_s) / stddev_s;

    ret_a_out = ret_a.back();
    ret_b_out = ret_b.back();
    spread_out = latest_spread;

    return true;
}

bool ScreenG::check_divergence(const std::string& symbol_a, const std::string& symbol_b, double& zscore_out, double& corr_out) const {
    double ret_a = 0.0, ret_b = 0.0, spread = 0.0;
    return calculate_pair_divergence(symbol_a, symbol_b, zscore_out, corr_out, ret_a, ret_b, spread);
}

void ScreenG::evaluate(const std::string& date) {
    std::vector<ScreenGResult> results;
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    std::string ts_str = ss.str();

    for (const auto& pair : pairs_) {
        double zscore = 0.0, corr = 0.0, ret_a = 0.0, ret_b = 0.0, spread = 0.0;
        bool ok = calculate_pair_divergence(pair.symbol_a, pair.symbol_b, zscore, corr, ret_a, ret_b, spread);
        if (!ok) {
            std::cout << "[ScreenG] Skipping " << pair.symbol_a << " vs " << pair.symbol_b << " due to insufficient data." << std::endl;
            continue;
        }

        ScreenGResult r;
        r.symbol_a = pair.symbol_a;
        r.symbol_b = pair.symbol_b;
        r.pearson_corr = corr;
        r.spread_zscore = zscore;
        r.is_diverged = std::abs(zscore) > zscore_threshold_;
        r.return_a = ret_a;
        r.return_b = ret_b;
        r.spread = spread;

        if (r.is_diverged) {
            if (std::abs(zscore) > 3.0 && corr > 0.7) {
                r.setup_tier = "premium";
            } else if (std::abs(zscore) > 2.0 && corr > 0.5) {
                r.setup_tier = "opportunity";
            } else {
                r.setup_tier = "interesting";
            }

            // Dispatch alert if cooldown allows
            std::string pair_key = pair.symbol_a + "_" + pair.symbol_b;
            bool allowed = false;
            {
                std::lock_guard<std::mutex> lock(cooldowns_mutex_);
                auto it = cooldowns_.find(pair_key);
                if (it == cooldowns_.end() || now >= it->second) {
                    cooldowns_[pair_key] = now + std::chrono::hours(24);
                    allowed = true;
                }
            }

            if (allowed && dispatcher_) {
                auto opt_a = store_->get_instrument_by_symbol(pair.symbol_a);
                if (opt_a) {
                    core::Alert alert;
                    alert.ts = ts_str;
                    alert.screen = "G";
                    alert.tier = r.setup_tier;
                    alert.instrument_id = opt_a->id;
                    alert.symbol = pair.symbol_a;

                    std::string regime_str = "Chop";
                    auto logs = store_->get_regime_log(1);
                    if (!logs.empty()) {
                        regime_str = logs[0].regime;
                    }
                    alert.regime_at_alert = regime_str;

                    alert.suggested_entry_low = 0.0;
                    alert.suggested_entry_high = 0.0;
                    alert.suggested_stop = 0.0;
                    alert.target_1 = 0.0;
                    alert.target_2 = 0.0;
                    alert.target_3 = 0.0;
                    alert.rr_to_target_1 = 0.0;

                    alert.confluence_factors.push_back("Pearson: " + std::to_string(corr).substr(0, 5));
                    alert.confluence_factors.push_back("Z-score: " + std::to_string(zscore).substr(0, 5));
                    alert.confluence_factors.push_back("Spread: " + std::to_string(spread).substr(0, 7));

                    alert.conviction_score = std::abs(zscore);
                    alert.notes = "Divergence detected: " + pair.symbol_a + " (daily ret " + std::to_string(ret_a * 100.0).substr(0, 4) + "%) vs " 
                                  + pair.symbol_b + " (daily ret " + std::to_string(ret_b * 100.0).substr(0, 4) + "%). Spread Z-score = " 
                                  + std::to_string(zscore).substr(0, 5) + ", Pearson = " + std::to_string(corr).substr(0, 5);

                    dispatcher_->dispatch(alert);
                }
            }
        } else {
            r.setup_tier = "interesting";
        }

        results.push_back(r);
    }

    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        latest_results_ = std::move(results);
    }

    std::cout << "[ScreenG] Cross-Asset Divergence evaluation complete. Results: " << latest_results_.size() << std::endl;
}

std::vector<ScreenGResult> ScreenG::get_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return latest_results_;
}

} // namespace screens
} // namespace trader
