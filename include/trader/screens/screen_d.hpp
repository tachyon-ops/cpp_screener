#pragma once

#include "trader/screens/screen.hpp"
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>

namespace trader {
namespace core {
    class AlertDispatcher;
}

namespace screens {

struct ScreenDResult {
    std::string symbol;
    std::string name;
    double price = 0.0;
    double ma50 = 0.0;
    double ma200 = 0.0;
    double dist_50ma = 0.0;  // (price - 50ma) / 50ma
    double dist_200ma = 0.0; // (price - 200ma) / 200ma
    double return_1m = 0.0;
    double return_3m = 0.0;
    double return_6m = 0.0;
    double return_12m = 0.0;
    int rs_rank = 0;         // 1 to N
    double rs_percentile = 0.0; // 0.0 to 100.0
    bool cross_50_200 = false; // 50MA crossed 200MA in last 5 sessions
    bool test_50ma = false;    // price within 1% of 50MA
    bool test_200ma = false;   // price within 1% of 200MA
};

class ScreenD : public Screen {
public:
    ScreenD(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenD() override = default;

    std::string name() const override { return "ScreenD"; }

    // Evaluates Screen D on a given date
    void evaluate(const std::string& date) override;

    // Returns the latest computed rotation results
    std::vector<ScreenDResult> get_results() const;

    // Standard list of sector ETFs to monitor
    static const std::vector<std::string>& sector_etf_symbols();

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;
    mutable std::mutex results_mutex_;
    std::vector<ScreenDResult> latest_results_;

    // Cooldown map: symbol -> end of cooldown time_point
    std::unordered_map<std::string, std::chrono::system_clock::time_point> cooldowns_;
    mutable std::mutex cooldowns_mutex_;
};

} // namespace screens
} // namespace trader
