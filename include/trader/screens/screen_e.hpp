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

struct ScreenEResult {
    std::string symbol;
    std::string name;
    double price = 0.0;
    double nav = 0.0;
    double discount = 0.0;
    double mean_discount = 0.0;
    double stddev_discount = 0.0;
    double discount_sigma = 0.0;
    double leverage_ratio = 0.0;
    double avg_dollar_volume = 0.0;
    std::string setup_tier; // "premium", "opportunity", "interesting"
    std::string notes;
};

class ScreenE : public Screen {
public:
    ScreenE(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenE() override = default;

    std::string name() const override { return "ScreenE"; }

    // Evaluates Screen E on a given date
    void evaluate(const std::string& date) override;

    // Returns the latest computed results
    std::vector<ScreenEResult> get_results() const;

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;
    mutable std::mutex results_mutex_;
    std::vector<ScreenEResult> latest_results_;
};

} // namespace screens
} // namespace trader
