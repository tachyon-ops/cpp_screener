#pragma once

#include "trader/screens/screen.hpp"
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>

namespace trader {
namespace storage {
    class TimeSeriesStore;
}
namespace core {
    class AlertDispatcher;
}

namespace screens {

struct ScreenAResult {
    std::string symbol;
    double price = 0.0;
    double vwap = 0.0;
    double stddev = 0.0;
    double deviation_sigma = 0.0;
    double volume_5m = 0.0;
    double avg_volume_5m_slot = 0.0;
    double sector_change = 0.0;
    double rs_1m = 0.0;
    std::string setup_tier; // "premium", "opportunity", "interesting"
    std::string notes;
};

class ScreenA : public Screen {
public:
    ScreenA(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<storage::TimeSeriesStore> ts_store,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenA() override = default;

    std::string name() const override { return "ScreenA"; }

    // Setup gate: runs pre-market to detect gap-down candidates
    void evaluate_premarket(const std::string& date);

    // Live trigger: evaluated periodically (e.g. every minute or 15s) during US session
    void evaluate(const std::string& date) override;

    // Trigger a mock gap-down for a symbol (for manual testing)
    void trigger_mock_gap_down(const std::string& symbol, double gap_pct, const std::string& news, bool is_existential);

    // Returns latest results
    std::vector<ScreenAResult> get_results() const;

private:
    struct PremarketCandidate {
        std::string symbol;
        double prior_close = 0.0;
        double gap_percent = 0.0;
        std::string news_summary;
        bool is_news_existential = false;
        std::string date;
    };

    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<storage::TimeSeriesStore> ts_store_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;

    mutable std::mutex candidates_mutex_;
    std::unordered_map<std::string, PremarketCandidate> candidates_;
    std::string last_premarket_date_;

    mutable std::mutex results_mutex_;
    std::vector<ScreenAResult> latest_results_;

    // Cooldown map: symbol -> end of cooldown time_point
    std::unordered_map<std::string, std::chrono::system_clock::time_point> cooldowns_;
    mutable std::mutex cooldowns_mutex_;
};

} // namespace screens
} // namespace trader
