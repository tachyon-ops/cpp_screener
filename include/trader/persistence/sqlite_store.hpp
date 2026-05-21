#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>

namespace trader {
namespace persistence {

struct DbInstrument {
    int64_t id = 0;
    std::string symbol;
    std::string asset_class;
    std::string exchange;
    int64_t saxo_uic = 0;
    std::string metadata_json;
};

struct DbBarDaily {
    int64_t instrument_id = 0;
    std::string date; // ISO 8601 YYYY-MM-DD
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

struct DbAlert {
    int64_t id = 0;
    std::string ts; // ISO 8601 UTC
    std::string screen; // e.g., 'A','B','C','D','E','F','G'
    int64_t instrument_id = 0;
    std::string tier; // 'premium','opportunity','interesting'
    std::string payload_json;
    std::string regime_at_alert;
    int acted_on = 0; // 0 or 1
};

struct DbRegimeLog {
    std::string ts; // ISO 8601 UTC
    std::string regime; // 'bull','chop','stress','crisis'
    double vix = 0.0;
    double breadth = 0.0;
    double hy_oas = 0.0;
    double spx_vs_200ma = 0.0;
    std::string detail_json;
};

struct DbCandidate {
    int64_t id = 0;
    std::string created_ts; // ISO 8601 UTC
    std::string screen; // e.g. 'B'
    int64_t instrument_id = 0;
    double entry_zone_low = 0.0;
    double entry_zone_high = 0.0;
    double suggested_stop = 0.0;
    double rr_target = 0.0;
    std::string notes;
    std::string status; // 'active','triggered','expired','cancelled'
};

class SQLiteStore {
public:
    explicit SQLiteStore(const std::string& db_path = "./data/screener.db");
    ~SQLiteStore();

    // Disable copy/move
    SQLiteStore(const SQLiteStore&) = delete;
    SQLiteStore& operator=(const SQLiteStore&) = delete;

    // Database Initialization
    void init_schema();

    // Synchronous Read / Write Operations
    void add_instrument(const DbInstrument& inst);
    std::vector<DbInstrument> get_instruments();
    std::optional<DbInstrument> get_instrument_by_symbol(const std::string& symbol);

    void add_bar_daily(const DbBarDaily& bar);
    std::vector<DbBarDaily> get_bars_daily(int64_t instrument_id);

    int64_t add_alert(const DbAlert& alert);
    void update_alert_acted(int64_t alert_id, int acted_on);
    std::vector<DbAlert> get_alerts(int limit = 100);

    void add_regime_log(const DbRegimeLog& log);
    std::vector<DbRegimeLog> get_regime_log(int limit = 100);

    int64_t add_candidate(const DbCandidate& cand);
    void update_candidate_status(int64_t candidate_id, const std::string& status);
    std::vector<DbCandidate> get_candidates();

    // Asynchronous Batched Writes
    void add_instrument_async(const DbInstrument& inst);
    void add_bar_daily_async(const DbBarDaily& bar);
    void add_alert_async(const DbAlert& alert);
    void add_regime_log_async(const DbRegimeLog& log);
    void add_candidate_async(const DbCandidate& cand);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace persistence
} // namespace trader
