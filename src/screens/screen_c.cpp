#include "trader/screens/screen_c.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/core/sr_provider.hpp"
#include "trader/screens/screen_g.hpp"
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
std::string get_current_utc_time_str() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}
}

ScreenC::ScreenC(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<storage::TimeSeriesStore> ts_store,
    std::shared_ptr<core::ISupportResistanceProvider> sr_provider,
    std::shared_ptr<ScreenG> screen_g,
    std::shared_ptr<core::AlertDispatcher> dispatcher
) : store_(store), ts_store_(ts_store), sr_provider_(sr_provider), screen_g_(screen_g), dispatcher_(dispatcher) {
    load_config("./config/screens.yaml");
}

void ScreenC::load_config(const std::string& path) {
    std::ifstream infile(path);
    if (!infile.is_open()) {
        std::cout << "[ScreenC] Warning: Could not open screens config: " << path << ", using defaults." << std::endl;
        return;
    }

    std::string line;
    bool in_screen_c = false;
    auto trim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    };

    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        bool starts_with_whitespace = (line.front() == ' ' || line.front() == '\t');
        trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("screen_c:", 0) == 0) {
            in_screen_c = true;
            continue;
        } else if (line.find(":") != std::string::npos && !starts_with_whitespace && line.rfind("screen_c", 0) != 0 && line.rfind("screen_g", 0) != 0) {
            in_screen_c = false;
        }

        if (in_screen_c) {
            if (line.rfind("vol_drop_threshold:", 0) == 0) {
                vol_drop_threshold_ = std::stod(line.substr(19));
            } else if (line.rfind("confluence_pct_equities:", 0) == 0) {
                confluence_pct_equities_ = std::stod(line.substr(24));
            } else if (line.rfind("confluence_pct_crypto:", 0) == 0) {
                confluence_pct_crypto_ = std::stod(line.substr(22));
            } else if (line.rfind("wick_ratio:", 0) == 0) {
                wick_ratio_ = std::stod(line.substr(11));
            } else if (line.rfind("close_third:", 0) == 0) {
                close_third_ = std::stod(line.substr(12));
            } else if (line.rfind("volume_mult:", 0) == 0) {
                volume_mult_ = std::stod(line.substr(12));
            } else if (line.rfind("panic_drawdown_percentile:", 0) == 0) {
                panic_drawdown_percentile_ = std::stod(line.substr(26));
            } else if (line.rfind("panic_drawdown_period:", 0) == 0) {
                panic_drawdown_period_ = std::stoi(line.substr(22));
            } else if (line.rfind("active_watch_duration_hours:", 0) == 0) {
                active_watch_duration_hours_ = std::stoi(line.substr(28));
            }
        }
    }
    std::cout << "[ScreenC] Config loaded: vol_drop_threshold=" << vol_drop_threshold_
              << ", wick_ratio=" << wick_ratio_
              << ", volume_mult=" << volume_mult_
              << ", panic_drawdown_percentile=" << panic_drawdown_percentile_ << std::endl;
}

bool ScreenC::is_watching(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    auto it = watch_list_.find(symbol);
    if (it == watch_list_.end()) return false;
    auto now = std::chrono::system_clock::now();
    return now < it->second;
}

void ScreenC::add_to_watch(const std::string& symbol, int duration_hours) {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    auto now = std::chrono::system_clock::now();
    watch_list_[symbol] = now + std::chrono::hours(duration_hours);
    std::cout << "[ScreenC] Added " << symbol << " to watch list for " << duration_hours << " hours." << std::endl;
}

void ScreenC::check_symbol(const std::string& symbol, const std::string& date) {
    auto opt_inst = store_->get_instrument_by_symbol(symbol);
    if (!opt_inst) return;

    auto now = std::chrono::system_clock::now();

    // 1. Stage 1 Check: Volume surge on drop or Screen G divergence
    bool is_div = false;
    double zscore = 0.0, corr = 0.0;
    if (screen_g_) {
        // Query Screen G results
        for (const auto& res : screen_g_->get_results()) {
            if ((res.symbol_a == symbol || res.symbol_b == symbol) && res.is_diverged) {
                is_div = true;
                zscore = res.spread_zscore;
                corr = res.pearson_corr;
                break;
            }
        }
    }

    bool vol_spike_drop = false;
    auto daily_bars = store_->get_bars_daily(opt_inst->id);
    if (daily_bars.size() >= 21) {
        double vol_sum = 0.0;
        for (size_t i = daily_bars.size() - 21; i < daily_bars.size() - 1; ++i) {
            vol_sum += daily_bars[i].volume;
        }
        double avg_vol_20d = vol_sum / 20.0;
        double latest_vol = daily_bars.back().volume;
        double latest_close = daily_bars.back().close;
        double prior_close = daily_bars[daily_bars.size() - 2].close;

        if (latest_close < prior_close && latest_vol > avg_vol_20d * vol_drop_threshold_) {
            vol_spike_drop = true;
        }
    }

    if (is_div || vol_spike_drop) {
        if (!is_watching(symbol)) {
            add_to_watch(symbol, active_watch_duration_hours_);
        }
    }

    // 2. Multi-timeframe wick trigger checks
    auto ts = ts_store_ ? ts_store_->get(symbol) : nullptr;
    if (!ts) return;

    std::vector<broker::Resolution> timeframes = {broker::Resolution::H1, broker::Resolution::H4, broker::Resolution::D1};
    for (broker::Resolution tf : timeframes) {
        auto tf_bars = ts->last_n(tf, panic_drawdown_period_ + 5);
        if (tf_bars.size() < 21) continue;

        const auto& trigger_bar = tf_bars.back();
        double open = trigger_bar.open.value;
        double high = trigger_bar.high.value;
        double low = trigger_bar.low.value;
        double close = trigger_bar.close.value;
        double vol = trigger_bar.volume.value;
        double range = high - low;

        if (range <= 0.0) continue;

        // Wick calculations
        double lower_wick = std::min(open, close) - low;
        double wick_size_pct = lower_wick / range;
        double close_ratio = (close - low) / range;

        // Volume average
        double vol_sum = 0.0;
        for (size_t i = tf_bars.size() - 21; i < tf_bars.size() - 1; ++i) {
            vol_sum += tf_bars[i].volume.value;
        }
        double avg_vol = vol_sum / 20.0;

        // Panic drawdown check: is the latest return in the bottom 5%?
        std::vector<double> returns;
        for (size_t i = tf_bars.size() - std::min(tf_bars.size(), static_cast<size_t>(panic_drawdown_period_)); i < tf_bars.size(); ++i) {
            double prev = tf_bars[i-1].close.value;
            if (prev > 0.0) {
                returns.push_back((tf_bars[i].close.value - prev) / prev);
            }
        }
        bool is_panic = false;
        if (!returns.empty()) {
            double latest_ret = returns.back();
            std::vector<double> sorted = returns;
            std::sort(sorted.begin(), sorted.end());
            size_t idx = static_cast<size_t>(sorted.size() * panic_drawdown_percentile_);
            if (idx >= sorted.size()) idx = sorted.size() - 1;
            is_panic = (latest_ret <= sorted[idx]);
        }

        // Proximity for confluence (Stage 2)
        double confluence_pct = (opt_inst->asset_class == "crypto") ? confluence_pct_crypto_ : confluence_pct_equities_;

        // Piercing support check (Stage 3)
        auto sr_levels = sr_provider_ ? sr_provider_->get_levels(symbol) : std::vector<core::SRLevel>();
        double pierced_support_price = 0.0;
        int support_sources = 0;
        std::vector<std::string> sources;

        for (const auto& sr : sr_levels) {
            double tol = sr.price * confluence_pct;
            // Support is pierced if low goes below support level, but close is above it (or close to it)
            if (low <= sr.price && close >= sr.price - tol) {
                pierced_support_price = sr.price;
                support_sources++;
                sources.push_back(sr.source);
            }
        }

        // Wick trigger conditions:
        bool pass_wick = (wick_size_pct >= wick_ratio_);
        bool pass_close = (close_ratio >= (1.0 - close_third_));
        bool pass_vol = (avg_vol > 0.0 && vol > avg_vol * volume_mult_);
        bool pass_support = (pierced_support_price > 0.0);

        // We require the asset to be in the watch list OR trigger on panic
        bool in_watch = is_watching(symbol);

        if (pass_wick && pass_close && pass_vol && pass_support && (in_watch || is_panic)) {
            // Screen C Triggered!
            // Build result
            ScreenCResult res;
            res.symbol = symbol;
            res.price = close;
            res.low = low;
            res.high = high;
            res.wick_size_pct = wick_size_pct;
            res.volume = vol;
            res.avg_volume = avg_vol;
            res.pierced_support = pierced_support_price;
            res.in_watch = in_watch;

            // Determine setup tier
            if (is_div && support_sources >= 2) {
                res.setup_tier = "premium";
            } else if (in_watch || support_sources >= 1) {
                res.setup_tier = "opportunity";
            } else {
                res.setup_tier = "interesting";
            }

            res.entry_zone_high = close;
            res.entry_zone_low = close * 0.99;
            res.suggested_stop = low * 0.995; // 0.5% below trigger low
            res.rr_target = 3.0;

            std::string tf_str = (tf == core::Resolution::H1) ? "1H" : (tf == core::Resolution::H4) ? "4H" : "1D";
            std::stringstream notes_ss;
            notes_ss << "Capitulation Wick Reversal on " << tf_str << ". Lower wick: " 
                     << std::fixed << std::setprecision(1) << (wick_size_pct * 100.0) << "%, Volume: " 
                     << std::setprecision(1) << (vol / avg_vol) << "x avg, Pierced support: " 
                     << std::setprecision(2) << pierced_support_price << " (" << support_sources << " sources: ";
            for (size_t s_idx = 0; s_idx < sources.size(); ++s_idx) {
                notes_ss << sources[s_idx] << (s_idx + 1 < sources.size() ? "," : "");
            }
            notes_ss << ").";
            res.notes = notes_ss.str();

            // Persist candidate to SQLite
            auto active_cands = store_->get_candidates();
            bool exists = false;
            for (const auto& ac : active_cands) {
                if (ac.instrument_id == opt_inst->id && ac.status == "active" && ac.screen == "C") {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                persistence::DbCandidate c;
                c.created_ts = get_current_utc_time_str();
                c.screen = "C";
                c.instrument_id = opt_inst->id;
                c.entry_zone_low = res.entry_zone_low;
                c.entry_zone_high = res.entry_zone_high;
                c.suggested_stop = res.suggested_stop;
                c.rr_target = res.rr_target;
                c.notes = res.notes + " (Tier: " + res.setup_tier + ")";
                c.status = "active";

                int64_t cand_id = store_->add_candidate(c);
                std::cout << "[ScreenC] Registered new C candidate ID=" << cand_id << " for " << symbol << std::endl;

                if (dispatcher_) {
                    core::Alert alert;
                    alert.ts = c.created_ts;
                    alert.screen = "C";
                    alert.tier = res.setup_tier;
                    alert.instrument_id = opt_inst->id;
                    alert.symbol = symbol;

                    std::string regime_str = "Chop";
                    auto logs = store_->get_regime_log(1);
                    if (!logs.empty()) {
                        regime_str = logs[0].regime;
                    }
                    alert.regime_at_alert = regime_str;

                    alert.suggested_entry_low = res.entry_zone_low;
                    alert.suggested_entry_high = res.entry_zone_high;
                    alert.suggested_stop = res.suggested_stop;
                    alert.target_1 = res.entry_zone_high + res.rr_target * (res.entry_zone_high - res.suggested_stop);
                    alert.target_2 = res.entry_zone_high + 2.0 * res.rr_target * (res.entry_zone_high - res.suggested_stop);
                    alert.target_3 = res.entry_zone_high + 3.0 * res.rr_target * (res.entry_zone_high - res.suggested_stop);
                    alert.rr_to_target_1 = res.rr_target;

                    alert.confluence_factors.push_back("Wick: " + std::to_string((int)(wick_size_pct * 100)) + "%");
                    alert.confluence_factors.push_back("Vol: " + std::to_string(vol / avg_vol).substr(0, 3) + "x");
                    alert.confluence_factors.push_back("Support Pierce");
                    for (const auto& src : sources) {
                        alert.confluence_factors.push_back("SR: " + src);
                    }

                    alert.conviction_score = 4.0;
                    alert.extra["notes"] = res.notes;

                    dispatcher_->dispatch(alert);
                }
            }

            // Lock and add to latest results
            std::lock_guard<std::mutex> lock(results_mutex_);
            // Remove previous result for same symbol if exists
            latest_results_.erase(
                std::remove_if(latest_results_.begin(), latest_results_.end(), 
                               [&](const ScreenCResult& x) { return x.symbol == symbol; }),
                latest_results_.end()
            );
            latest_results_.push_back(res);

            break; // Triggered on one TF, no need to check other TFs for same symbol
        }
    }
}

void ScreenC::evaluate(const std::string& date) {
    auto instruments = store_->get_instruments();
    for (const auto& inst : instruments) {
        check_symbol(inst.symbol, date);
    }
    std::cout << "[ScreenC] Capitulation Wick Reversal evaluation complete. Results: " << latest_results_.size() << std::endl;
}

std::vector<ScreenCResult> ScreenC::get_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return latest_results_;
}

} // namespace screens
} // namespace trader
