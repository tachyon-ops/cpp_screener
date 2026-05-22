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
    class AlertDispatcher;
}

namespace screens {

struct ScreenGResult {
    std::string symbol_a;
    std::string symbol_b;
    double pearson_corr = 0.0;
    double spread_zscore = 0.0;
    bool is_diverged = false;
    double return_a = 0.0;
    double return_b = 0.0;
    double spread = 0.0;
    std::string setup_tier; // "premium", "opportunity", "interesting"
};

struct DivergencePair {
    std::string symbol_a;
    std::string symbol_b;
};

class ScreenG : public Screen {
public:
    ScreenG(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<storage::TimeSeriesStore> ts_store,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~ScreenG() override = default;

    std::string name() const override { return "ScreenG"; }

    void evaluate(const std::string& date) override;

    std::vector<ScreenGResult> get_results() const;

    // Direct check if a specific pair is diverged, so Screen C can query it
    bool check_divergence(const std::string& symbol_a, const std::string& symbol_b, double& zscore_out, double& corr_out) const;

    // Load parameters from screens.yaml
    void load_config(const std::string& path);

private:
    bool calculate_pair_divergence(
        const std::string& symbol_a, const std::string& symbol_b, 
        double& zscore_out, double& corr_out, 
        double& ret_a_out, double& ret_b_out, double& spread_out) const;

    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<storage::TimeSeriesStore> ts_store_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;

    mutable std::mutex results_mutex_;
    std::vector<ScreenGResult> latest_results_;

    // Config parameters
    int correlation_period_ = 30;
    int zscore_period_ = 90;
    double zscore_threshold_ = 2.0;
    std::vector<DivergencePair> pairs_;

    // Cooldown map: pair key (symbol_a + "_" + symbol_b) -> end of cooldown time_point
    std::unordered_map<std::string, std::chrono::system_clock::time_point> cooldowns_;
    mutable std::mutex cooldowns_mutex_;
};

} // namespace screens
} // namespace trader
