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

struct ScreenFResult {
    std::string symbol;
    std::string name;
    double price = 0.0;
    double box_top = 0.0;
    double box_bottom = 0.0;
    double box_height_pct = 0.0;
    int consolidation_days = 0;
    double volume_ratio = 0.0;
    bool sector_above_ma200 = false;
    std::string setup_tier; // "premium", "opportunity", "interesting"
    std::string notes;
};

class ScreenF : public Screen {
public:
    ScreenF(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenF() override = default;

    std::string name() const override { return "ScreenF"; }

    // Evaluates Screen F on a given date
    void evaluate(const std::string& date) override;

    // Returns the latest computed results
    std::vector<ScreenFResult> get_results() const;

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;
    mutable std::mutex results_mutex_;
    std::vector<ScreenFResult> latest_results_;
};

} // namespace screens
} // namespace trader
