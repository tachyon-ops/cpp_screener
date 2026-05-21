#pragma once

#include "trader/screens/screen.hpp"
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <chrono>

namespace trader {
namespace storage {
    class TimeSeriesStore;
}
namespace core {
    class ISupportResistanceProvider;
    class AlertDispatcher;
}
namespace screens {
    class ScreenG;

struct ScreenCResult {
    std::string symbol;
    double price = 0.0;
    double low = 0.0;
    double high = 0.0;
    double wick_size_pct = 0.0;
    double volume = 0.0;
    double avg_volume = 0.0;
    double pierced_support = 0.0;
    std::string setup_tier; // "premium", "opportunity", "interesting"
    double entry_zone_low = 0.0;
    double entry_zone_high = 0.0;
    double suggested_stop = 0.0;
    double rr_target = 3.0;
    std::string notes;
    bool in_watch = false;
};

class ScreenC : public Screen {
public:
    ScreenC(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<storage::TimeSeriesStore> ts_store,
        std::shared_ptr<core::ISupportResistanceProvider> sr_provider,
        std::shared_ptr<ScreenG> screen_g,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenC() override = default;

    std::string name() const override { return "ScreenC"; }

    // Periodically evaluate Screen C
    void evaluate(const std::string& date) override;

    std::vector<ScreenCResult> get_results() const;

    // Trigger Screen C check manually for a symbol
    void check_symbol(const std::string& symbol, const std::string& date);

    // Watch status
    bool is_watching(const std::string& symbol) const;
    void add_to_watch(const std::string& symbol, int duration_hours = 72);

    // Load parameters from screens.yaml
    void load_config(const std::string& path);

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<storage::TimeSeriesStore> ts_store_;
    std::shared_ptr<core::ISupportResistanceProvider> sr_provider_;
    std::shared_ptr<ScreenG> screen_g_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;

    mutable std::mutex results_mutex_;
    std::vector<ScreenCResult> latest_results_;

    // Watch list: symbol -> expiration time
    std::unordered_map<std::string, std::chrono::system_clock::time_point> watch_list_;
    mutable std::mutex watch_mutex_;

    // Config parameters
    double vol_drop_threshold_ = 3.0;
    double confluence_pct_equities_ = 0.010;
    double confluence_pct_crypto_ = 0.015;
    double wick_ratio_ = 0.60;
    double close_third_ = 0.33;
    double volume_mult_ = 2.0;
    double panic_drawdown_percentile_ = 0.05;
    int panic_drawdown_period_ = 60;
    int active_watch_duration_hours_ = 72;
};

} // namespace screens
} // namespace trader
