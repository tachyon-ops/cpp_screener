#include "trader/core/regime_classifier.hpp"
#include <iostream>
#include <cmath>
#include <nlohmann/json.hpp>

namespace trader {
namespace core {

std::string regime_to_string(Regime regime) {
    switch (regime) {
        case Regime::Bull:   return "bull";
        case Regime::Chop:   return "chop";
        case Regime::Stress: return "stress";
        case Regime::Crisis: return "crisis";
    }
    return "chop";
}

Regime string_to_regime(const std::string& str) {
    if (str == "bull") return Regime::Bull;
    if (str == "chop") return Regime::Chop;
    if (str == "stress") return Regime::Stress;
    if (str == "crisis") return Regime::Crisis;
    return Regime::Chop;
}

RegimeClassifier::RegimeClassifier(std::shared_ptr<persistence::SQLiteStore> store)
    : store_(store) {
    // Attempt to load current regime from the database on startup
    auto logs = store_->get_regime_log(1);
    if (!logs.empty()) {
        current_regime_ = string_to_regime(logs[0].regime);
        candidate_regime_ = current_regime_;
        consecutive_days_ = 0;
        std::cout << "[RegimeClassifier] Loaded current regime from DB: " << regime_to_string(current_regime_) << std::endl;
    } else {
        current_regime_ = Regime::Chop;
        candidate_regime_ = Regime::Chop;
        consecutive_days_ = 0;
        std::cout << "[RegimeClassifier] No history found, defaulted to: chop" << std::endl;
    }
}

Regime RegimeClassifier::evaluate(const std::string& date, double vix_val, double hy_oas_val, double breadth_val, double spx_val) {
    double spx_close = spx_val;
    double spx_200ma = spx_val;
    double spx_200ma_slope = 0.0;
    double spx_vs_200ma = 0.0;

    // Retrieve historical daily bars of SPY to compute 200MA and slope
    auto opt_inst = store_->get_instrument_by_symbol("SPY");
    if (opt_inst) {
        auto bars = store_->get_bars_daily_range(opt_inst->id, "1900-01-01", date);
        
        // Append today's price if not already in the list
        bool exists = false;
        for (const auto& b : bars) {
            if (b.date == date) {
                exists = true;
                break;
            }
        }
        if (!exists && spx_val > 0.0) {
            persistence::DbBarDaily today_bar;
            today_bar.instrument_id = opt_inst->id;
            today_bar.date = date;
            today_bar.close = spx_val;
            bars.push_back(today_bar);
        }

        if (bars.size() >= 219) {
            std::vector<double> mas;
            for (size_t i = bars.size() - 20; i < bars.size(); ++i) {
                double sum = 0.0;
                for (size_t j = i - 199; j <= i; ++j) {
                    sum += bars[j].close;
                }
                mas.push_back(sum / 200.0);
            }
            spx_close = bars.back().close;
            spx_200ma = mas.back();
            spx_vs_200ma = (spx_close - spx_200ma) / spx_200ma;

            // Calculate linear regression slope of 200MA over 20 sessions
            double sum_x = 0;
            double sum_y = 0;
            double sum_xy = 0;
            double sum_xx = 0;
            int n = mas.size();
            for (int i = 0; i < n; ++i) {
                sum_x += i;
                sum_y += mas[i];
                sum_xy += i * mas[i];
                sum_xx += i * i;
            }
            spx_200ma_slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);
        } else {
            std::cerr << "[RegimeClassifier] Warning: Not enough SPY daily bars (need 219, got " 
                      << bars.size() << ") for MA calculation. Falling back to default trend score." << std::endl;
            if (spx_val <= 0.0) spx_close = 500.0; // Default fallback
            spx_200ma = spx_close;
        }
    } else {
        std::cerr << "[RegimeClassifier] Warning: SPY instrument not found. Trend score will be defaulted." << std::endl;
        if (spx_val <= 0.0) spx_close = 500.0;
        spx_200ma = spx_close;
    }

    // Retrieve previous log to compare state and compute credit spread jump
    auto prev_logs = store_->get_regime_log(1);
    double prev_hy_oas = 0.0;
    Regime prev_regime = current_regime_;

    if (!prev_logs.empty()) {
        prev_hy_oas = prev_logs[0].hy_oas;
        prev_regime = string_to_regime(prev_logs[0].regime);
    }

    // Crisis override check
    bool crisis_override = false;
    if (vix_val >= 35.0) {
        crisis_override = true;
    } else if (prev_hy_oas > 0.0) {
        double prev_hy_oas_bps = prev_hy_oas < 100.0 ? prev_hy_oas * 100.0 : prev_hy_oas;
        double hy_oas_bps = hy_oas_val < 100.0 ? hy_oas_val * 100.0 : hy_oas_val;
        if (hy_oas_bps - prev_hy_oas_bps >= 100.0) {
            crisis_override = true;
        }
    }

    Regime next_regime = prev_regime;

    if (crisis_override) {
        next_regime = Regime::Crisis;
        candidate_regime_ = Regime::Crisis;
        consecutive_days_ = 0; // reset
        std::cout << "[RegimeClassifier] !! Crisis Override Triggered !! VIX = " << vix_val 
                  << ", HY OAS = " << hy_oas_val << std::endl;
    } else {
        // Normal path with scoring and 2-day hysteresis
        RegimeScores scores = compute_scores(spx_close, spx_200ma, spx_200ma_slope, breadth_val, vix_val, hy_oas_val);
        
        Regime calculated_regime;
        if (scores.total_score >= 5) {
            calculated_regime = Regime::Bull;
        } else if (scores.total_score >= 1) {
            calculated_regime = Regime::Chop;
        } else if (scores.total_score >= -3) {
            calculated_regime = Regime::Stress;
        } else {
            calculated_regime = Regime::Crisis;
        }

        if (calculated_regime == prev_regime) {
            next_regime = prev_regime;
            candidate_regime_ = prev_regime;
            consecutive_days_ = 0;
        } else {
            if (calculated_regime == candidate_regime_) {
                consecutive_days_++;
                if (consecutive_days_ >= 2) {
                    next_regime = candidate_regime_;
                } else {
                    next_regime = prev_regime; // Keep previous regime for now
                }
            } else {
                candidate_regime_ = calculated_regime;
                consecutive_days_ = 1;
                next_regime = prev_regime; // Keep previous regime
            }
        }
    }

    current_regime_ = next_regime;

    // Log calculation details in JSON
    nlohmann::json detail;
    detail["spx_close"] = spx_close;
    detail["spx_200ma"] = spx_200ma;
    detail["spx_200ma_slope"] = spx_200ma_slope;
    detail["consecutive_days"] = consecutive_days_;
    detail["candidate_regime"] = regime_to_string(candidate_regime_);

    // Save regime log to SQLite
    persistence::DbRegimeLog log_entry;
    log_entry.ts = date + "T16:30:00Z"; // standard EOD timestamp
    log_entry.regime = regime_to_string(current_regime_);
    log_entry.vix = vix_val;
    log_entry.breadth = breadth_val;
    log_entry.hy_oas = hy_oas_val;
    log_entry.spx_vs_200ma = spx_vs_200ma;
    log_entry.detail_json = detail.dump();

    store_->add_regime_log(log_entry);

    std::cout << "[RegimeClassifier] Evaluated date " << date << ": " << regime_to_string(current_regime_)
              << " (vix=" << vix_val << ", oas=" << hy_oas_val << ", breadth=" << breadth_val 
              << ", spx_vs_200ma=" << spx_vs_200ma << ")" << std::endl;

    return current_regime_;
}

RegimeScores RegimeClassifier::compute_scores(double spx_close, double spx_200ma, double spx_200ma_slope,
                                            double breadth, double vix, double hy_oas) {
    RegimeScores scores;
    
    // Trend score
    if (spx_close > spx_200ma) {
        if (spx_200ma_slope > 0) {
            scores.trend_score = 2;
        } else {
            scores.trend_score = 1;
        }
    } else {
        if (spx_200ma_slope >= 0) {
            scores.trend_score = -1;
        } else {
            scores.trend_score = -2;
        }
    }

    // Breadth score
    if (breadth > 0.70) {
        scores.breadth_score = 2;
    } else if (breadth > 0.50) {
        scores.breadth_score = 1;
    } else if (breadth > 0.30) {
        scores.breadth_score = 0;
    } else {
        scores.breadth_score = -2;
    }

    // Vol score (VIX)
    if (vix < 15.0) {
        scores.vol_score = 1;
    } else if (vix < 20.0) {
        scores.vol_score = 0;
    } else if (vix < 25.0) {
        scores.vol_score = -1;
    } else if (vix < 35.0) {
        scores.vol_score = -2;
    } else {
        scores.vol_score = -3;
    }

    // Credit score (HY OAS)
    double hy_oas_bps = hy_oas;
    if (hy_oas < 100.0) {
        hy_oas_bps = hy_oas * 100.0;
    }

    if (hy_oas_bps < 350.0) {
        scores.credit_score = 1;
    } else if (hy_oas_bps < 450.0) {
        scores.credit_score = 0;
    } else if (hy_oas_bps < 600.0) {
        scores.credit_score = -1;
    } else {
        scores.credit_score = -2;
    }

    scores.total_score = scores.trend_score + scores.breadth_score + scores.vol_score + scores.credit_score;
    return scores;
}

} // namespace core
} // namespace trader
