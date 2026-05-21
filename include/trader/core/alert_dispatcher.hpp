#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <nlohmann/json.hpp>
#include "trader/persistence/sqlite_store.hpp"
#include "trader/broker/broker_adapter.hpp"
#include "trader/core/telegram.hpp"

namespace trader {
namespace core {

enum class Regime;

struct Alert {
    int64_t id = 0;
    std::string ts;                 // ISO 8601 UTC
    std::string screen;             // "A", "B", ..., "G"
    std::string tier;               // "premium", "opportunity", "interesting"
    int64_t instrument_id = 0;
    std::string symbol;
    std::string regime_at_alert;

    // Core trade params
    double suggested_entry_low = 0.0;
    double suggested_entry_high = 0.0;
    double suggested_stop = 0.0;
    double target_1 = 0.0;
    double target_2 = 0.0;
    double target_3 = 0.0;
    double rr_to_target_1 = 0.0;

    // Position sizing
    struct PositionSize {
        double units = 0.0;
        double cost = 0.0;
        double pct_account = 0.0;
        bool capped = false;
    };
    PositionSize size_1pct;
    PositionSize size_2pct;
    PositionSize size_5pct;

    // Context
    std::vector<std::string> confluence_factors;
    std::string news_summary;
    nlohmann::json extra;

    // Sizing score / conviction score for throttling
    double conviction_score = 0.0;
};

struct ClusterDefinition {
    std::string id;
    std::string leader;
    std::vector<std::string> members;
    std::string asset_class;
};

class AlertDispatcher {
public:
    AlertDispatcher(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<broker::BrokerAdapter> broker,
        std::shared_ptr<TelegramBot> tg_bot
    );
    ~AlertDispatcher();

    // Disable copy
    AlertDispatcher(const AlertDispatcher&) = delete;
    AlertDispatcher& operator=(const AlertDispatcher&) = delete;

    void start();
    void stop();

    // Ingest a new alert from screens
    void dispatch(const Alert& alert);

    // Reload clusters configuration
    void reload_clusters();

    // Register callback for realtime alert propagation
    void set_alert_callback(std::function<void(const std::string&)> cb);

private:
    void process_loop();
    void digest_check_loop();

    // Helper to calculate suggestion position sizes
    void calculate_position_sizes(Alert& alert);

    // Grouping & Throttling
    void process_queued_alerts(std::vector<Alert>& alerts);
    std::string get_cluster_id_for_symbol(const std::string& symbol);
    bool check_throttle_limit(const std::string& tier, Regime regime);
    
    // Send message helpers
    void send_to_channels(const Alert& alert, const std::string& custom_text = "", const std::string& custom_markup = "");
    void send_pushover_message(const std::string& message);
    void send_daily_digest();

    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<broker::BrokerAdapter> broker_;
    std::shared_ptr<TelegramBot> tg_bot_;

    std::function<void(const std::string&)> alert_cb_;
    std::mutex cb_mutex_;

    std::vector<ClusterDefinition> clusters_;
    
    // Cooldown tracker: cluster_id -> end of cooldown time_point
    std::map<std::string, std::chrono::steady_clock::time_point> cluster_cooldowns_;
    std::mutex cooldowns_mutex_;

    // Throttle tracker: tier -> list of timestamps when alert was sent in the last 60 minutes
    std::map<std::string, std::vector<std::chrono::system_clock::time_point>> sent_alert_times_;
    std::mutex throttle_mutex_;

    // Queued alerts for batch processing
    std::vector<Alert> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::thread worker_thread_;
    std::thread digest_thread_;
    std::atomic<bool> running_{false};

    std::string last_digest_day_;
    std::mutex digest_mutex_;
};

} // namespace core
} // namespace trader
