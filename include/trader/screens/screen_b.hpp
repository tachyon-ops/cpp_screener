#pragma once

#include "trader/screens/screen.hpp"
#include <mutex>
#include <vector>
#include <string>

namespace trader {
namespace core {
    class AlertDispatcher;
}

namespace screens {

struct ScreenBResult {
    std::string symbol;
    std::string name;
    double price = 0.0;
    double ma20 = 0.0;
    double ma50 = 0.0;
    double ma150 = 0.0;
    double ma200 = 0.0;
    double rsi14 = 0.0;
    double vol_5day_avg = 0.0;
    double vol_20day_avg = 0.0;
    double low_52week = 0.0;
    double high_52week = 0.0;
    double rs_percentile = 0.0;
    std::string setup_tier; // "premium", "opportunity", "interesting"
    double entry_zone_low = 0.0;
    double entry_zone_high = 0.0;
    double suggested_stop = 0.0;
    double rr_target = 3.0;
    std::string notes;
};

class ScreenB : public Screen {
public:
    ScreenB(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenB() override = default;

    std::string name() const override { return "ScreenB"; }

    // Evaluates Screen B on a given date
    void evaluate(const std::string& date) override;

    // Returns the latest computed results
    std::vector<ScreenBResult> get_results() const;

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;
    mutable std::mutex results_mutex_;
    std::vector<ScreenBResult> latest_results_;
};

} // namespace screens
} // namespace trader
