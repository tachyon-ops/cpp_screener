#pragma once

#include <string>
#include <vector>
#include <memory>
#include "trader/persistence/sqlite_store.hpp"

namespace trader {
namespace core {

enum class Regime {
    Bull,
    Chop,
    Stress,
    Crisis
};

std::string regime_to_string(Regime regime);
Regime string_to_regime(const std::string& str);

struct RegimeScores {
    int trend_score = 0;
    int breadth_score = 0;
    int vol_score = 0;
    int credit_score = 0;
    int total_score = 0;
};

class RegimeClassifier {
public:
    explicit RegimeClassifier(std::shared_ptr<persistence::SQLiteStore> store);
    ~RegimeClassifier() = default;

    // Evaluates current regime for a given date (YYYY-MM-DD), using database historical data.
    // Logs to regime_log table if there is a transition.
    Regime evaluate(const std::string& date, double vix_val, double hy_oas_val, double breadth_val, double spx_val);

    // Get current regime (cached/state)
    Regime current_regime() const { return current_regime_; }
    void set_current_regime(Regime regime) { current_regime_ = regime; }

    // Helpers to compute scores
    RegimeScores compute_scores(double spx_close, double spx_200ma, double spx_200ma_slope,
                                double breadth, double vix, double hy_oas);

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    Regime current_regime_ = Regime::Chop; // Default
    Regime candidate_regime_ = Regime::Chop;
    int consecutive_days_ = 0;
};

} // namespace core
} // namespace trader
