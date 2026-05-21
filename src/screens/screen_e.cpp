#include "trader/screens/screen_e.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace trader {
namespace screens {

ScreenE::ScreenE(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<core::AlertDispatcher> dispatcher
) : store_(store), dispatcher_(dispatcher) {}

void ScreenE::evaluate(const std::string& date) {
    std::cout << "[ScreenE] Running CEF Discount calculations for date: " << date << std::endl;

    auto instruments = store_->get_instruments();
    std::vector<ScreenEResult> current_results;

    for (const auto& inst : instruments) {
        if (inst.asset_class != "CEF") {
            continue; // Only evaluate Closed-End Funds
        }

        std::string symbol = inst.symbol;
        
        // Parse leverage ratio and sector ETF from metadata
        double leverage_ratio = 0.0;
        try {
            if (!inst.metadata_json.empty()) {
                auto meta = nlohmann::json::parse(inst.metadata_json);
                if (meta.contains("leverage_ratio")) {
                    if (meta["leverage_ratio"].is_number()) {
                        leverage_ratio = meta["leverage_ratio"].get<double>();
                    } else if (meta["leverage_ratio"].is_string()) {
                        leverage_ratio = std::stod(meta["leverage_ratio"].get<std::string>());
                    }
                }
            }
        } catch (...) {
            // Default leverage to 0 if parsing fails
        }

        // CEF leverage ratio filter: must be < 50%
        if (leverage_ratio >= 0.50) {
            continue;
        }

        // Look up companion NAV instrument (symbol.NAV)
        std::string nav_symbol = symbol + ".NAV";
        auto opt_nav_inst = store_->get_instrument_by_symbol(nav_symbol);
        if (!opt_nav_inst) {
            continue; // Missing NAV instrument
        }

        // Fetch daily bars for parent CEF and companion NAV
        auto parent_bars = store_->get_bars_daily_range(inst.id, "1900-01-01", date);
        auto nav_bars = store_->get_bars_daily_range(opt_nav_inst->id, "1900-01-01", date);

        if (parent_bars.empty() || nav_bars.empty()) {
            continue;
        }

        // Match bars by date to build discount series
        // discounts[t] = (NAV_t - Close_t) / NAV_t
        std::vector<double> discounts;
        std::vector<double> dollar_volumes; // to compute average daily dollar volume

        size_t parent_idx = 0;
        size_t nav_idx = 0;

        while (parent_idx < parent_bars.size() && nav_idx < nav_bars.size()) {
            const auto& p_bar = parent_bars[parent_idx];
            const auto& n_bar = nav_bars[nav_idx];

            if (p_bar.date == n_bar.date) {
                if (n_bar.close > 0.0) {
                    double disc = (n_bar.close - p_bar.close) / n_bar.close;
                    discounts.push_back(disc);
                }
                dollar_volumes.push_back(p_bar.close * p_bar.volume);
                parent_idx++;
                nav_idx++;
            } else if (p_bar.date < n_bar.date) {
                parent_idx++;
            } else {
                nav_idx++;
            }
        }

        // We need some minimum history to calculate statistics
        if (discounts.size() < 10) {
            continue; 
        }

        // Average daily dollar volume over the last 50 sessions (or whatever history we have)
        double sum_dollar_vol = 0.0;
        size_t vol_lookback = std::min((size_t)50, dollar_volumes.size());
        for (size_t i = dollar_volumes.size() - vol_lookback; i < dollar_volumes.size(); ++i) {
            sum_dollar_vol += dollar_volumes[i];
        }
        double avg_dollar_volume = vol_lookback > 0 ? sum_dollar_vol / vol_lookback : 0.0;

        // Liquidity filter: average daily dollar volume must be > $500k
        if (avg_dollar_volume < 500000.0) {
            continue;
        }

        // Compute mean and standard deviation of discounts (2-year lookback max)
        size_t stats_lookback = std::min((size_t)504, discounts.size()); // 2 years = ~504 trading days
        double sum_disc = 0.0;
        size_t start_idx = discounts.size() - stats_lookback;
        for (size_t i = start_idx; i < discounts.size() - 1; ++i) { // exclude latest day from historical baseline
            sum_disc += discounts[i];
        }
        double mean_discount = stats_lookback > 1 ? sum_disc / (stats_lookback - 1) : 0.0;

        double sum_sq_diff = 0.0;
        for (size_t i = start_idx; i < discounts.size() - 1; ++i) {
            double diff = discounts[i] - mean_discount;
            sum_sq_diff += diff * diff;
        }
        double stddev_discount = stats_lookback > 2 ? std::sqrt(sum_sq_diff / (stats_lookback - 2)) : 0.01;
        if (stddev_discount <= 0.0) stddev_discount = 0.01;

        // Current discount
        double current_price = parent_bars.back().close;
        double current_nav = nav_bars.back().close;
        double current_discount = (current_nav - current_price) / current_nav;
        double discount_sigma = (current_discount - mean_discount) / stddev_discount;

        // Trigger condition
        // 1. Current discount > 8% absolute
        // 2. Current discount > mean discount + 1.0σ (for Interesting tier, 1.5σ for standard)
        std::string setup_tier = "";
        if (current_discount > 0.08) {
            if (discount_sigma > 2.0 && current_discount > 0.15) {
                setup_tier = "premium";
            } else if (discount_sigma > 1.5) {
                setup_tier = "opportunity";
            } else if (discount_sigma > 1.0) {
                setup_tier = "interesting";
            }
        }

        if (!setup_tier.empty()) {
            std::stringstream ss;
            ss << "Screen E triggered on " << symbol << ". Current Discount: " 
               << std::fixed << std::setprecision(2) << (current_discount * 100.0) << "% (Mean + "
               << discount_sigma << "σ), NAV: $" << current_nav << ", Price: $" << current_price;

            ScreenEResult res;
            res.symbol = symbol;
            res.name = symbol + " Closed-End Fund";
            
            // Get name from metadata if available
            try {
                if (!inst.metadata_json.empty()) {
                    auto meta = nlohmann::json::parse(inst.metadata_json);
                    res.name = meta.value("name", res.name);
                }
            } catch (...) {}

            res.price = current_price;
            res.nav = current_nav;
            res.discount = current_discount;
            res.mean_discount = mean_discount;
            res.stddev_discount = stddev_discount;
            res.discount_sigma = discount_sigma;
            res.leverage_ratio = leverage_ratio;
            res.avg_dollar_volume = avg_dollar_volume;
            res.setup_tier = setup_tier;
            res.notes = ss.str();

            current_results.push_back(res);

            // Dispatch alert if dispatcher is available
            if (dispatcher_) {
                core::Alert alert;
                
                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::stringstream ts_ss;
                ts_ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");

                alert.ts = ts_ss.str();
                alert.screen = "E";
                alert.tier = setup_tier;
                alert.instrument_id = inst.id;
                alert.symbol = symbol;

                std::string regime_str = "Chop";
                auto logs = store_->get_regime_log(1);
                if (!logs.empty()) {
                    regime_str = logs[0].regime;
                }
                alert.regime_at_alert = regime_str;

                alert.suggested_entry_low = current_price;
                alert.suggested_entry_high = current_price;
                
                // Suggested stop is 10% below current price
                alert.suggested_stop = current_price * 0.90;
                
                // Suggested target is mean-reversion to historical discount
                // Price at mean discount = NAV * (1 - mean_discount)
                double price_at_mean = current_nav * (1.0 - mean_discount);
                alert.target_1 = price_at_mean;
                alert.target_2 = price_at_mean * 1.10;
                alert.target_3 = price_at_mean * 1.20;
                alert.rr_to_target_1 = (price_at_mean - current_price) / (current_price - alert.suggested_stop);

                alert.confluence_factors.push_back("Discount deviation: " + std::to_string(discount_sigma) + " sigma");
                alert.confluence_factors.push_back("Current discount: " + std::to_string(current_discount * 100.0) + "%");
                alert.confluence_factors.push_back("Leverage ratio: " + std::to_string(leverage_ratio * 100.0) + "%");
                alert.confluence_factors.push_back("50d Avg Dollar Vol: $" + std::to_string(avg_dollar_volume / 1000.0) + "k");

                alert.news_summary = "Closed-end fund trades at extreme historical discount. Leverage: " + std::to_string((int)(leverage_ratio * 100)) + "%.";

                alert.extra["nav"] = current_nav;
                alert.extra["discount"] = current_discount;
                alert.extra["mean_discount"] = mean_discount;
                alert.extra["stddev_discount"] = stddev_discount;
                alert.extra["discount_sigma"] = discount_sigma;
                alert.extra["leverage_ratio"] = leverage_ratio;
                alert.extra["avg_dollar_volume"] = avg_dollar_volume;

                alert.conviction_score = discount_sigma;

                dispatcher_->dispatch(alert);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        latest_results_ = current_results;
    }
}

std::vector<ScreenEResult> ScreenE::get_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return latest_results_;
}

} // namespace screens
} // namespace trader
