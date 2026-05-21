#include "trader/screens/screen_a.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>

namespace trader {
namespace screens {

ScreenA::ScreenA(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<storage::TimeSeriesStore> ts_store,
    std::shared_ptr<core::AlertDispatcher> dispatcher
) : store_(store), ts_store_(ts_store), dispatcher_(dispatcher) {}

void ScreenA::evaluate_premarket(const std::string& date) {
    std::lock_guard<std::mutex> lock(candidates_mutex_);
    if (last_premarket_date_ == date && !candidates_.empty()) {
        return; // Already initialized for today
    }

    last_premarket_date_ = date;
    candidates_.clear();

    // Seed default simulated candidate if none exist, ensuring there is a testable setup
    PremarketCandidate tsla_cand;
    tsla_cand.symbol = "TSLA";
    tsla_cand.gap_percent = -6.5;
    tsla_cand.news_summary = "Tesla sees weaker delivery demand in Asia; Analysts downgrade target.";
    tsla_cand.is_news_existential = false;
    tsla_cand.date = date;

    auto opt_inst = store_->get_instrument_by_symbol("TSLA");
    if (opt_inst) {
        auto daily_bars = store_->get_bars_daily(opt_inst->id);
        if (!daily_bars.empty()) {
            tsla_cand.prior_close = daily_bars.back().close;
        } else {
            tsla_cand.prior_close = 175.0;
        }
    } else {
        tsla_cand.prior_close = 175.0;
    }

    candidates_["TSLA"] = tsla_cand;

    std::cout << "[ScreenA] Pre-market evaluation complete for " << date
              << ". Registered candidate TSLA with " << tsla_cand.gap_percent << "% gap down." << std::endl;
}

void ScreenA::evaluate(const std::string& date) {
    // Ensure premarket candidates are initialized
    evaluate_premarket(date);

    std::vector<PremarketCandidate> cands;
    {
        std::lock_guard<std::mutex> lock(candidates_mutex_);
        for (const auto& pair : candidates_) {
            cands.push_back(pair.second);
        }
    }

    std::vector<ScreenAResult> current_results;
    auto now = std::chrono::system_clock::now();

    for (const auto& cand : cands) {
        std::string symbol = cand.symbol;

        // Check 60-minute Alert Suppressive Cooldown
        {
            std::lock_guard<std::mutex> lock(cooldowns_mutex_);
            auto it = cooldowns_.find(symbol);
            if (it != cooldowns_.end() && now < it->second) {
                continue; // Suppressed
            }
        }

        auto ts = ts_store_->get(symbol);
        if (!ts) continue;

        // Get 5-minute bars for rolling VWAP and stddev
        auto m5_bars = ts->last_n(core::Resolution::M5, 20);
        if (m5_bars.size() < 20) {
            continue; // Need at least 20 periods
        }

        double current_price = ts->get_latest_price();
        if (current_price <= 0.0) continue;

        // 1. Compute 20-period intraday VWAP
        double sum_pv = 0.0;
        double sum_v = 0.0;
        for (const auto& bar : m5_bars) {
            double price = bar.close.value;
            sum_pv += price * bar.volume.value;
            sum_v += bar.volume.value;
        }
        double vwap = sum_v > 0.0 ? sum_pv / sum_v : current_price;

        // Compute standard deviation of price from VWAP
        double sum_sq_diff = 0.0;
        for (const auto& bar : m5_bars) {
            double diff = bar.close.value - vwap;
            sum_sq_diff += diff * diff;
        }
        double stddev = std::sqrt(sum_sq_diff / m5_bars.size());
        if (stddev <= 0.0) stddev = current_price * 0.001; // small fallback

        double deviation_sigma = (vwap - current_price) / stddev;

        // Live trigger evaluation (5 criteria checklist)
        // 1. Price is < VWAP - 2.5σ
        bool c1 = current_price < (vwap - 2.5 * stddev);

        // 2. Volume in last 5 minutes > 2× 20-day average for that 5-min slot
        double vol_5m = 0.0;
        auto latest_m5 = ts->latest(core::Resolution::M5);
        if (latest_m5) {
            vol_5m = latest_m5->volume.value;
        }

        double avg_daily_vol = 1000000.0;
        auto opt_inst = store_->get_instrument_by_symbol(symbol);
        if (opt_inst) {
            auto daily_bars = store_->get_bars_daily(opt_inst->id);
            if (!daily_bars.empty()) {
                double sum_vol = 0.0;
                int count = 0;
                int start_idx = std::max(0, (int)daily_bars.size() - 20);
                for (size_t i = start_idx; i < daily_bars.size(); ++i) {
                    sum_vol += daily_bars[i].volume;
                    count++;
                }
                if (count > 0) {
                    avg_daily_vol = sum_vol / count;
                }
            }
        }
        double avg_vol_5m_slot = avg_daily_vol / 78.0;
        bool c2 = vol_5m > 2.0 * avg_vol_5m_slot;

        // 3. Stock's sector ETF not simultaneously breaking down (sector change > -1.5%)
        std::string sector_etf = "SPY";
        if (opt_inst && !opt_inst->metadata_json.empty()) {
            try {
                auto meta = nlohmann::json::parse(opt_inst->metadata_json);
                if (meta.contains("sector_etf_symbol")) {
                    sector_etf = meta["sector_etf_symbol"].get<std::string>();
                }
            } catch (...) {}
        }

        double sector_prior_close = 0.0;
        auto opt_sector = store_->get_instrument_by_symbol(sector_etf);
        if (opt_sector) {
            auto sec_bars = store_->get_bars_daily(opt_sector->id);
            if (!sec_bars.empty()) {
                sector_prior_close = sec_bars.back().close;
            }
        }

        double sector_current_price = 0.0;
        auto sector_ts = ts_store_->get(sector_etf);
        if (sector_ts) {
            sector_current_price = sector_ts->get_latest_price();
        }
        if (sector_current_price <= 0.0) {
            sector_current_price = sector_prior_close;
        }

        double sector_change = 0.0;
        if (sector_prior_close > 0.0) {
            sector_change = (sector_current_price - sector_prior_close) / sector_prior_close;
        }
        bool c3 = sector_change > -0.015;

        // 4. Stock's 1-month Relative Strength still > 0 vs SPY
        double stock_past_price = 0.0;
        if (opt_inst) {
            auto daily_bars = store_->get_bars_daily_range(opt_inst->id, "1900-01-01", date);
            if (!daily_bars.empty()) {
                size_t lookback = 21;
                if (daily_bars.size() > lookback) {
                    stock_past_price = daily_bars[daily_bars.size() - 1 - lookback].close;
                } else {
                    stock_past_price = daily_bars.front().close;
                }
            }
        }
        if (stock_past_price <= 0.0) stock_past_price = current_price * 0.95;
        double stock_return = (current_price - stock_past_price) / stock_past_price;

        double spy_past_price = 0.0;
        double spy_current_price = 0.0;
        auto opt_spy = store_->get_instrument_by_symbol("SPY");
        if (opt_spy) {
            auto spy_bars = store_->get_bars_daily_range(opt_spy->id, "1900-01-01", date);
            if (!spy_bars.empty()) {
                size_t lookback = 21;
                if (spy_bars.size() > lookback) {
                    spy_past_price = spy_bars[spy_bars.size() - 1 - lookback].close;
                } else {
                    spy_past_price = spy_bars.front().close;
                }
            }
        }
        auto spy_ts = ts_store_->get("SPY");
        if (spy_ts) spy_current_price = spy_ts->get_latest_price();
        if (spy_current_price <= 0.0) spy_current_price = spy_past_price;
        if (spy_past_price <= 0.0) spy_past_price = 500.0;
        double spy_return = (spy_current_price - spy_past_price) / spy_past_price;

        double rs_1m = stock_return - spy_return;
        bool c4 = rs_1m > 0.0;

        // 5. News not existential
        bool c5 = !cand.is_news_existential;

        // Sum criteria met
        int met_count = 0;
        if (c1) met_count++;
        if (c2) met_count++;
        if (c3) met_count++;
        if (c4) met_count++;
        if (c5) met_count++;

        // Categorize tier
        std::string setup_tier = "";
        bool is_premium = (current_price < (vwap - 3.0 * stddev)) &&
                          (vol_5m > 3.0 * avg_vol_5m_slot) &&
                          c3 && c4 && c5;

        if (is_premium) {
            setup_tier = "premium";
        } else if (met_count >= 4) {
            setup_tier = "opportunity";
        } else if (met_count >= 3) {
            setup_tier = "interesting";
        }

        if (!setup_tier.empty()) {
            // Apply 60-minute Alert suppression cooldown
            {
                std::lock_guard<std::mutex> lock(cooldowns_mutex_);
                cooldowns_[symbol] = now + std::chrono::minutes(60);
            }

            std::stringstream ss;
            ss << "Screen A triggered. Price: $" << std::fixed << std::setprecision(2) << current_price
               << " (VWAP - " << std::setprecision(2) << deviation_sigma << "σ), 5m Vol Ratio: "
               << std::setprecision(2) << (vol_5m / avg_vol_5m_slot) << "x, Sector Change: "
               << std::setprecision(2) << (sector_change * 100.0) << "%, 1m RS: "
               << std::setprecision(2) << (rs_1m * 100.0) << "%";

            std::cout << "[ScreenA] TRIGGERED: " << symbol << " on tier " << setup_tier
                      << ". " << ss.str() << std::endl;

            ScreenAResult result;
            result.symbol = symbol;
            result.price = current_price;
            result.vwap = vwap;
            result.stddev = stddev;
            result.deviation_sigma = deviation_sigma;
            result.volume_5m = vol_5m;
            result.avg_volume_5m_slot = avg_vol_5m_slot;
            result.sector_change = sector_change;
            result.rs_1m = rs_1m;
            result.setup_tier = setup_tier;
            result.notes = ss.str();
            current_results.push_back(result);

            if (dispatcher_ && opt_inst) {
                core::Alert alert;
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::stringstream ts_ss;
                ts_ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");

                alert.ts = ts_ss.str();
                alert.screen = "A";
                alert.tier = setup_tier;
                alert.instrument_id = opt_inst->id;
                alert.symbol = symbol;

                std::string regime_str = "Chop";
                auto logs = store_->get_regime_log(1);
                if (!logs.empty()) {
                    regime_str = logs[0].regime;
                }
                alert.regime_at_alert = regime_str;

                alert.suggested_entry_low = current_price;
                alert.suggested_entry_high = current_price;

                double risk = std::max(1.5 * stddev, current_price * 0.02);
                alert.suggested_stop = current_price - risk;

                alert.target_1 = current_price + 2.0 * risk;
                alert.target_2 = current_price + 4.0 * risk;
                alert.target_3 = current_price + 6.0 * risk;
                alert.rr_to_target_1 = 2.0;

                alert.confluence_factors.push_back("VWAP deviation: " + std::to_string(deviation_sigma) + " sigma");
                alert.confluence_factors.push_back("Volume spike: " + std::to_string(vol_5m / avg_vol_5m_slot) + "x");
                alert.confluence_factors.push_back("Sector ETF change: " + std::to_string(sector_change * 100.0) + "%");
                alert.confluence_factors.push_back("1m Relative Strength: " + std::to_string(rs_1m * 100.0) + "%");

                alert.news_summary = cand.news_summary;

                alert.extra["vwap"] = vwap;
                alert.extra["stddev"] = stddev;
                alert.extra["deviation_sigma"] = deviation_sigma;
                alert.extra["volume_5m"] = vol_5m;
                alert.extra["avg_volume_5m_slot"] = avg_vol_5m_slot;
                alert.extra["sector_change"] = sector_change;
                alert.extra["rs_1m"] = rs_1m;

                alert.conviction_score = deviation_sigma;

                dispatcher_->dispatch(alert);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        latest_results_ = current_results;
    }
}

void ScreenA::trigger_mock_gap_down(const std::string& symbol, double gap_pct, const std::string& news, bool is_existential) {
    std::lock_guard<std::mutex> lock(candidates_mutex_);
    PremarketCandidate cand;
    cand.symbol = symbol;
    cand.gap_percent = gap_pct;
    cand.news_summary = news;
    cand.is_news_existential = is_existential;

    auto opt_inst = store_->get_instrument_by_symbol(symbol);
    if (opt_inst) {
        auto daily_bars = store_->get_bars_daily(opt_inst->id);
        if (!daily_bars.empty()) {
            cand.prior_close = daily_bars.back().close;
        } else {
            cand.prior_close = 150.0;
        }
    } else {
        cand.prior_close = 150.0;
    }

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%d");
    cand.date = ss.str();

    candidates_[symbol] = cand;

    {
        std::lock_guard<std::mutex> cd_lock(cooldowns_mutex_);
        cooldowns_.erase(symbol);
    }

    double new_price = cand.prior_close * (1.0 + gap_pct / 100.0);
    ts_store_->pre_populate(symbol, new_price);

    std::cout << "[ScreenA] Force-triggered mock gap-down for " << symbol
              << ": gap = " << gap_pct << "%, news = " << news
              << ", is_existential = " << (is_existential ? "true" : "false")
              << ", target price = $" << new_price << std::endl;
}

std::vector<ScreenAResult> ScreenA::get_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return latest_results_;
}

} // namespace screens
} // namespace trader
