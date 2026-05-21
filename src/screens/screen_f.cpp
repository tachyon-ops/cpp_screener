#include "trader/screens/screen_f.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace trader {
namespace screens {

ScreenF::ScreenF(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<core::AlertDispatcher> dispatcher
) : store_(store), dispatcher_(dispatcher) {}

void ScreenF::evaluate(const std::string& date) {
    std::cout << "[ScreenF] Running Darvas Box Breakout calculations for date: " << date << std::endl;

    auto instruments = store_->get_instruments();
    std::vector<ScreenFResult> current_results;

    for (const auto& inst : instruments) {
        // Exclude ETFs and NAV instruments
        if (inst.asset_class == "ETF" || inst.asset_class == "CEF_NAV" || inst.asset_class == "CEF") {
            continue; 
        }

        std::string symbol = inst.symbol;
        auto bars = store_->get_bars_daily_range(inst.id, "1900-01-01", date);

        // We need at least 85 bars (60 days prior + 20 days last + 5 days buffer)
        if (bars.size() < 85) {
            continue;
        }

        size_t curr_idx = bars.size() - 1;
        double current_price = bars[curr_idx].close;
        double current_volume = bars[curr_idx].volume;

        // 1. Calculate average daily dollar volume over the last 50 days (liquidity check)
        double sum_dollar_vol = 0.0;
        double sum_vol_50 = 0.0;
        for (size_t i = curr_idx - 50; i < curr_idx; ++i) {
            sum_dollar_vol += bars[i].close * bars[i].volume;
            sum_vol_50 += bars[i].volume;
        }
        double avg_dollar_volume = sum_dollar_vol / 50.0;
        double avg_volume_50 = sum_vol_50 / 50.0;

        // Liquidity filter: avg daily $ vol > $10M
        if (avg_dollar_volume < 10000000.0) {
            continue;
        }

        // 2. Box detection windows
        // Consolidation window: last 20 sessions prior to today (curr_idx - 20 to curr_idx - 1)
        // Prior window: 60 sessions prior to consolidation (curr_idx - 80 to curr_idx - 21)
        
        double box_top = 0.0;
        double box_bottom = 999999.0;
        for (size_t i = curr_idx - 80; i <= curr_idx - 21; ++i) {
            box_top = std::max(box_top, bars[i].high);
            box_bottom = std::min(box_bottom, bars[i].low);
        }
        double range_prior = box_top - box_bottom;

        double high_last = 0.0;
        double low_last = 999999.0;
        for (size_t i = curr_idx - 20; i < curr_idx; ++i) {
            high_last = std::max(high_last, bars[i].high);
            low_last = std::min(low_last, bars[i].low);
        }
        double range_last = high_last - low_last;

        // Check compression: last 20-session range < 50% of prior 60-session range
        bool is_compressed = range_last < 0.5 * range_prior;
        if (!is_compressed) {
            continue;
        }

        // Verify ceiling and floor support: during consolidation, price did not break above box_top or below box_bottom
        bool box_unbroken = true;
        for (size_t i = curr_idx - 20; i < curr_idx; ++i) {
            if (bars[i].high > box_top || bars[i].low < box_bottom) {
                box_unbroken = false;
                break;
            }
        }
        if (!box_unbroken) {
            continue;
        }

        // 3. Count consolidation duration (how many consecutive days prior to curr_idx price stayed within box)
        int consolidation_days = 0;
        for (int i = static_cast<int>(curr_idx) - 1; i >= 0; --i) {
            if (bars[i].high <= box_top && bars[i].low >= box_bottom) {
                consolidation_days++;
            } else {
                break;
            }
        }

        // Box must be established for at least 2 weeks (10 trading days) to trigger
        if (consolidation_days < 10) {
            continue;
        }

        // 4. Trigger breakout conditions today
        // Daily close > box top + 1%
        bool close_breakout = current_price > box_top * 1.01;
        // Volume > 2x 50-day average
        double volume_ratio = avg_volume_50 > 0.0 ? current_volume / avg_volume_50 : 0.0;
        bool volume_spike = volume_ratio > 2.0;

        if (!close_breakout || !volume_spike) {
            continue;
        }

        // 5. Industry Sector MA test
        std::string sector_etf = "SPY";
        try {
            if (!inst.metadata_json.empty()) {
                auto meta = nlohmann::json::parse(inst.metadata_json);
                sector_etf = meta.value("sector_etf_symbol", "SPY");
            }
        } catch (...) {}

        bool sector_above_ma200 = false;
        auto opt_sector = store_->get_instrument_by_symbol(sector_etf);
        if (opt_sector) {
            auto sec_bars = store_->get_bars_daily_range(opt_sector->id, "1900-01-01", date);
            if (sec_bars.size() >= 200) {
                double sum_close = 0.0;
                for (size_t i = sec_bars.size() - 200; i < sec_bars.size(); ++i) {
                    sum_close += sec_bars[i].close;
                }
                double ma200 = sum_close / 200.0;
                sector_above_ma200 = sec_bars.back().close > ma200;
            }
        }

        // 6. Tiering rules
        std::string setup_tier = "";
        if (consolidation_days >= 40 && volume_ratio > 3.0 && sector_above_ma200) { // 8 weeks = 40 trading days
            setup_tier = "premium";
        } else if (consolidation_days >= 20) { // 4 weeks = 20 trading days
            setup_tier = "opportunity";
        } else if (consolidation_days >= 10) { // 2 weeks = 10 trading days
            setup_tier = "interesting";
        }

        if (!setup_tier.empty()) {
            double box_height_pct = box_top > 0.0 ? (box_top - box_bottom) / box_top : 0.0;
            std::stringstream ss;
            ss << "Screen F triggered on " << symbol << ". Box Top: $" << std::fixed << std::setprecision(2)
               << box_top << ", Bottom: $" << box_bottom << " (Height: " << (box_height_pct * 100.0) 
               << "%), Consolidation: " << consolidation_days << " days, Vol Ratio: " << volume_ratio << "x";

            ScreenFResult res;
            res.symbol = symbol;
            res.name = symbol + " Common Stock";
            
            try {
                if (!inst.metadata_json.empty()) {
                    auto meta = nlohmann::json::parse(inst.metadata_json);
                    res.name = meta.value("name", res.name);
                }
            } catch (...) {}

            res.price = current_price;
            res.box_top = box_top;
            res.box_bottom = box_bottom;
            res.box_height_pct = box_height_pct;
            res.consolidation_days = consolidation_days;
            res.volume_ratio = volume_ratio;
            res.sector_above_ma200 = sector_above_ma200;
            res.setup_tier = setup_tier;
            res.notes = ss.str();

            current_results.push_back(res);

            // Dispatch alert
            if (dispatcher_) {
                core::Alert alert;

                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::stringstream ts_ss;
                ts_ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");

                alert.ts = ts_ss.str();
                alert.screen = "F";
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
                
                // Suggested stop is 1% below box top (which is now support)
                alert.suggested_stop = box_top * 0.99;
                
                double risk = current_price - alert.suggested_stop;
                alert.target_1 = current_price + 2.0 * risk;
                alert.target_2 = current_price + 4.0 * risk;
                alert.target_3 = current_price + 6.0 * risk;
                alert.rr_to_target_1 = 2.0;

                alert.confluence_factors.push_back("Box height: " + std::to_string(box_height_pct * 100.0) + "%");
                alert.confluence_factors.push_back("Consolidation: " + std::to_string(consolidation_days) + " days");
                alert.confluence_factors.push_back("Volume spike: " + std::to_string(volume_ratio) + "x");
                alert.confluence_factors.push_back("Sector above 200MA: " + std::string(sector_above_ma200 ? "Yes" : "No"));

                alert.news_summary = "Stock breaks out of a " + std::to_string(consolidation_days) + "-day Darvas consolidation box on high volume.";

                alert.extra["box_top"] = box_top;
                alert.extra["box_bottom"] = box_bottom;
                alert.extra["box_height_pct"] = box_height_pct;
                alert.extra["consolidation_days"] = consolidation_days;
                alert.extra["volume_ratio"] = volume_ratio;
                alert.extra["sector_above_ma200"] = sector_above_ma200 ? 1.0 : 0.0;

                alert.conviction_score = volume_ratio;

                dispatcher_->dispatch(alert);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        latest_results_ = current_results;
    }
}

std::vector<ScreenFResult> ScreenF::get_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return latest_results_;
}

} // namespace screens
} // namespace trader
