#include "trader/persistence/sqlite_store.hpp"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <filesystem>

namespace trader {
namespace persistence {

struct SQLiteStore::Impl {
    std::string db_path;
    sqlite3* db = nullptr;
    std::mutex db_mutex;

    // Async writer thread state
    std::thread writer_thread;
    std::atomic<bool> run_writer_thread{false};
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::vector<std::function<void(sqlite3*)>> write_queue;

    void open_db() {
        std::filesystem::path path(db_path);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db);
            sqlite3_close(db);
            throw std::runtime_error("Failed to open SQLite database: " + err);
        }

        // Enable WAL mode
        char* errMsg = nullptr;
        rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::cerr << "[SQLiteStore] Warning: Failed to enable WAL mode: " 
                      << (errMsg ? errMsg : "unknown") << std::endl;
            sqlite3_free(errMsg);
        }
    }

    void close_db() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    void start_writer() {
        run_writer_thread = true;
        writer_thread = std::thread([this]() {
            while (run_writer_thread) {
                std::vector<std::function<void(sqlite3*)>> local_queue;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    queue_cv.wait_for(lock, std::chrono::seconds(1), [this]() {
                        return !write_queue.empty() || !run_writer_thread;
                    });
                    if (!write_queue.empty()) {
                        std::swap(local_queue, write_queue);
                    }
                }

                if (!local_queue.empty()) {
                    std::lock_guard<std::mutex> db_lock(db_mutex);
                    char* errMsg = nullptr;
                    int rc = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
                    if (rc != SQLITE_OK) {
                        std::cerr << "[SQLiteStore] Failed to begin transaction: " 
                                  << (errMsg ? errMsg : "unknown") << std::endl;
                        sqlite3_free(errMsg);
                        continue;
                    }

                    for (auto& task : local_queue) {
                        try {
                            task(db);
                        } catch (const std::exception& e) {
                            std::cerr << "[SQLiteStore] Exception in async task: " << e.what() << std::endl;
                        } catch (...) {
                            std::cerr << "[SQLiteStore] Unknown exception in async task" << std::endl;
                        }
                    }

                    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &errMsg);
                    if (rc != SQLITE_OK) {
                        std::cerr << "[SQLiteStore] Failed to commit transaction: " 
                                  << (errMsg ? errMsg : "unknown") << std::endl;
                        sqlite3_free(errMsg);
                        // Rollback on failure
                        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    }
                }
            }
        });
    }

    void stop_writer() {
        run_writer_thread = false;
        queue_cv.notify_all();
        if (writer_thread.joinable()) {
            writer_thread.join();
        }

        // Flush any remaining writes
        if (!write_queue.empty()) {
            std::lock_guard<std::mutex> db_lock(db_mutex);
            sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
            for (auto& task : write_queue) {
                try {
                    task(db);
                } catch (...) {}
            }
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            write_queue.clear();
        }
    }

    void enqueue_write(std::function<void(sqlite3*)> task) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        write_queue.push_back(std::move(task));
        queue_cv.notify_one();
    }
};

SQLiteStore::SQLiteStore(const std::string& db_path)
    : pimpl_(std::make_unique<Impl>()) {
    pimpl_->db_path = db_path;
    pimpl_->open_db();
    pimpl_->start_writer();
}

SQLiteStore::~SQLiteStore() {
    pimpl_->stop_writer();
    pimpl_->close_db();
}

void SQLiteStore::init_schema() {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS instruments ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  symbol TEXT NOT NULL UNIQUE,"
        "  asset_class TEXT NOT NULL,"
        "  exchange TEXT,"
        "  saxo_uic INTEGER,"
        "  metadata_json TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS bars_daily ("
        "  instrument_id INTEGER NOT NULL,"
        "  date TEXT NOT NULL,"
        "  open REAL, high REAL, low REAL, close REAL,"
        "  volume REAL,"
        "  PRIMARY KEY (instrument_id, date),"
        "  FOREIGN KEY (instrument_id) REFERENCES instruments(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS alerts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts TEXT NOT NULL,"
        "  screen TEXT NOT NULL,"
        "  instrument_id INTEGER,"
        "  tier TEXT NOT NULL,"
        "  payload_json TEXT NOT NULL,"
        "  regime_at_alert TEXT,"
        "  acted_on INTEGER DEFAULT 0,"
        "  FOREIGN KEY (instrument_id) REFERENCES instruments(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS regime_log ("
        "  ts TEXT PRIMARY KEY,"
        "  regime TEXT NOT NULL,"
        "  vix REAL,"
        "  breadth REAL,"
        "  hy_oas REAL,"
        "  spx_vs_200ma REAL,"
        "  detail_json TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS candidates ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_ts TEXT NOT NULL,"
        "  screen TEXT NOT NULL,"
        "  instrument_id INTEGER,"
        "  entry_zone_low REAL, entry_zone_high REAL,"
        "  suggested_stop REAL,"
        "  rr_target REAL,"
        "  notes TEXT,"
        "  status TEXT DEFAULT 'active',"
        "  FOREIGN KEY (instrument_id) REFERENCES instruments(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_alerts_ts ON alerts(ts);"
        "CREATE INDEX IF NOT EXISTS idx_alerts_screen ON alerts(screen);"
        "CREATE INDEX IF NOT EXISTS idx_bars_daily_date ON bars_daily(date);";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(pimpl_->db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        throw std::runtime_error("Failed to create tables: " + err);
    }
}

void SQLiteStore::add_instrument(const DbInstrument& inst) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "INSERT OR REPLACE INTO instruments (id, symbol, asset_class, exchange, saxo_uic, metadata_json) VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("Prepare fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    if (inst.id > 0) {
        sqlite3_bind_int64(stmt, 1, inst.id);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    sqlite3_bind_text(stmt, 2, inst.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, inst.asset_class.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, inst.exchange.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, inst.saxo_uic);
    sqlite3_bind_text(stmt, 6, inst.metadata_json.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("Step fail: ") + sqlite3_errmsg(pimpl_->db));
    }
}

std::vector<DbInstrument> SQLiteStore::get_instruments() {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    std::vector<DbInstrument> list;
    const char* sql = "SELECT id, symbol, asset_class, exchange, saxo_uic, metadata_json FROM instruments;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DbInstrument inst;
            inst.id = sqlite3_column_int64(stmt, 0);
            inst.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            inst.asset_class = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* ex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            inst.exchange = ex ? ex : "";
            inst.saxo_uic = sqlite3_column_int64(stmt, 4);
            const char* meta = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            inst.metadata_json = meta ? meta : "";
            list.push_back(inst);
        }
    }
    sqlite3_finalize(stmt);
    return list;
}

std::optional<DbInstrument> SQLiteStore::get_instrument_by_symbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "SELECT id, symbol, asset_class, exchange, saxo_uic, metadata_json FROM instruments WHERE symbol = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::optional<DbInstrument> result;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            DbInstrument inst;
            inst.id = sqlite3_column_int64(stmt, 0);
            inst.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            inst.asset_class = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* ex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            inst.exchange = ex ? ex : "";
            inst.saxo_uic = sqlite3_column_int64(stmt, 4);
            const char* meta = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            inst.metadata_json = meta ? meta : "";
            result = inst;
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

void SQLiteStore::add_bar_daily(const DbBarDaily& bar) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "INSERT OR REPLACE INTO bars_daily (instrument_id, date, open, high, low, close, volume) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("Prepare fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    sqlite3_bind_int64(stmt, 1, bar.instrument_id);
    sqlite3_bind_text(stmt, 2, bar.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, bar.open);
    sqlite3_bind_double(stmt, 4, bar.high);
    sqlite3_bind_double(stmt, 5, bar.low);
    sqlite3_bind_double(stmt, 6, bar.close);
    sqlite3_bind_double(stmt, 7, bar.volume);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("Step fail: ") + sqlite3_errmsg(pimpl_->db));
    }
}

std::vector<DbBarDaily> SQLiteStore::get_bars_daily(int64_t instrument_id) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    std::vector<DbBarDaily> list;
    const char* sql = "SELECT instrument_id, date, open, high, low, close, volume FROM bars_daily WHERE instrument_id = ? ORDER BY date ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, instrument_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DbBarDaily bar;
            bar.instrument_id = sqlite3_column_int64(stmt, 0);
            bar.date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            bar.open = sqlite3_column_double(stmt, 2);
            bar.high = sqlite3_column_double(stmt, 3);
            bar.low = sqlite3_column_double(stmt, 4);
            bar.close = sqlite3_column_double(stmt, 5);
            bar.volume = sqlite3_column_double(stmt, 6);
            list.push_back(bar);
        }
    }
    sqlite3_finalize(stmt);
    return list;
}

int64_t SQLiteStore::add_alert(const DbAlert& alert) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "INSERT INTO alerts (ts, screen, instrument_id, tier, payload_json, regime_at_alert, acted_on) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("Prepare fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    sqlite3_bind_text(stmt, 1, alert.ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, alert.screen.c_str(), -1, SQLITE_TRANSIENT);
    if (alert.instrument_id > 0) {
        sqlite3_bind_int64(stmt, 3, alert.instrument_id);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_text(stmt, 4, alert.tier.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, alert.payload_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, alert.regime_at_alert.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, alert.acted_on);

    int rc = sqlite3_step(stmt);
    int64_t last_id = 0;
    if (rc == SQLITE_DONE) {
        last_id = sqlite3_last_insert_rowid(pimpl_->db);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("Step fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    return last_id;
}

void SQLiteStore::update_alert_acted(int64_t alert_id, int acted_on) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "UPDATE alerts SET acted_on = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, acted_on);
        sqlite3_bind_int64(stmt, 2, alert_id);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

std::vector<DbAlert> SQLiteStore::get_alerts(int limit) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    std::vector<DbAlert> list;
    const char* sql = "SELECT id, ts, screen, instrument_id, tier, payload_json, regime_at_alert, acted_on FROM alerts ORDER BY ts DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DbAlert alert;
            alert.id = sqlite3_column_int64(stmt, 0);
            alert.ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            alert.screen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            alert.instrument_id = sqlite3_column_int64(stmt, 3);
            alert.tier = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            alert.payload_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            const char* reg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            alert.regime_at_alert = reg ? reg : "";
            alert.acted_on = sqlite3_column_int(stmt, 7);
            list.push_back(alert);
        }
    }
    sqlite3_finalize(stmt);
    return list;
}

void SQLiteStore::add_regime_log(const DbRegimeLog& log) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "INSERT OR REPLACE INTO regime_log (ts, regime, vix, breadth, hy_oas, spx_vs_200ma, detail_json) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("Prepare fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    sqlite3_bind_text(stmt, 1, log.ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, log.regime.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, log.vix);
    sqlite3_bind_double(stmt, 4, log.breadth);
    sqlite3_bind_double(stmt, 5, log.hy_oas);
    sqlite3_bind_double(stmt, 6, log.spx_vs_200ma);
    sqlite3_bind_text(stmt, 7, log.detail_json.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("Step fail: ") + sqlite3_errmsg(pimpl_->db));
    }
}

std::vector<DbRegimeLog> SQLiteStore::get_regime_log(int limit) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    std::vector<DbRegimeLog> list;
    const char* sql = "SELECT ts, regime, vix, breadth, hy_oas, spx_vs_200ma, detail_json FROM regime_log ORDER BY ts DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DbRegimeLog log;
            log.ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            log.regime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            log.vix = sqlite3_column_double(stmt, 2);
            log.breadth = sqlite3_column_double(stmt, 3);
            log.hy_oas = sqlite3_column_double(stmt, 4);
            log.spx_vs_200ma = sqlite3_column_double(stmt, 5);
            const char* detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            log.detail_json = detail ? detail : "";
            list.push_back(log);
        }
    }
    sqlite3_finalize(stmt);
    return list;
}

int64_t SQLiteStore::add_candidate(const DbCandidate& cand) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "INSERT INTO candidates (created_ts, screen, instrument_id, entry_zone_low, entry_zone_high, suggested_stop, rr_target, notes, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("Prepare fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    sqlite3_bind_text(stmt, 1, cand.created_ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, cand.screen.c_str(), -1, SQLITE_TRANSIENT);
    if (cand.instrument_id > 0) {
        sqlite3_bind_int64(stmt, 3, cand.instrument_id);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_double(stmt, 4, cand.entry_zone_low);
    sqlite3_bind_double(stmt, 5, cand.entry_zone_high);
    sqlite3_bind_double(stmt, 6, cand.suggested_stop);
    sqlite3_bind_double(stmt, 7, cand.rr_target);
    sqlite3_bind_text(stmt, 8, cand.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, cand.status.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    int64_t last_id = 0;
    if (rc == SQLITE_DONE) {
        last_id = sqlite3_last_insert_rowid(pimpl_->db);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("Step fail: ") + sqlite3_errmsg(pimpl_->db));
    }
    return last_id;
}

void SQLiteStore::update_candidate_status(int64_t candidate_id, const std::string& status) {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    const char* sql = "UPDATE candidates SET status = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, candidate_id);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

std::vector<DbCandidate> SQLiteStore::get_candidates() {
    std::lock_guard<std::mutex> lock(pimpl_->db_mutex);
    std::vector<DbCandidate> list;
    const char* sql = "SELECT id, created_ts, screen, instrument_id, entry_zone_low, entry_zone_high, suggested_stop, rr_target, notes, status FROM candidates ORDER BY created_ts DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(pimpl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DbCandidate cand;
            cand.id = sqlite3_column_int64(stmt, 0);
            cand.created_ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            cand.screen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            cand.instrument_id = sqlite3_column_int64(stmt, 3);
            cand.entry_zone_low = sqlite3_column_double(stmt, 4);
            cand.entry_zone_high = sqlite3_column_double(stmt, 5);
            cand.suggested_stop = sqlite3_column_double(stmt, 6);
            cand.rr_target = sqlite3_column_double(stmt, 7);
            const char* notes = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            cand.notes = notes ? notes : "";
            cand.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            list.push_back(cand);
        }
    }
    sqlite3_finalize(stmt);
    return list;
}

void SQLiteStore::add_instrument_async(const DbInstrument& inst) {
    pimpl_->enqueue_write([inst](sqlite3* db) {
        const char* sql = "INSERT OR REPLACE INTO instruments (id, symbol, asset_class, exchange, saxo_uic, metadata_json) VALUES (?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (inst.id > 0) {
                sqlite3_bind_int64(stmt, 1, inst.id);
            } else {
                sqlite3_bind_null(stmt, 1);
            }
            sqlite3_bind_text(stmt, 2, inst.symbol.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, inst.asset_class.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, inst.exchange.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 5, inst.saxo_uic);
            sqlite3_bind_text(stmt, 6, inst.metadata_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    });
}

void SQLiteStore::add_bar_daily_async(const DbBarDaily& bar) {
    pimpl_->enqueue_write([bar](sqlite3* db) {
        const char* sql = "INSERT OR REPLACE INTO bars_daily (instrument_id, date, open, high, low, close, volume) VALUES (?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, bar.instrument_id);
            sqlite3_bind_text(stmt, 2, bar.date.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, bar.open);
            sqlite3_bind_double(stmt, 4, bar.high);
            sqlite3_bind_double(stmt, 5, bar.low);
            sqlite3_bind_double(stmt, 6, bar.close);
            sqlite3_bind_double(stmt, 7, bar.volume);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    });
}

void SQLiteStore::add_alert_async(const DbAlert& alert) {
    pimpl_->enqueue_write([alert](sqlite3* db) {
        const char* sql = "INSERT INTO alerts (ts, screen, instrument_id, tier, payload_json, regime_at_alert, acted_on) VALUES (?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, alert.ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, alert.screen.c_str(), -1, SQLITE_TRANSIENT);
            if (alert.instrument_id > 0) {
                sqlite3_bind_int64(stmt, 3, alert.instrument_id);
            } else {
                sqlite3_bind_null(stmt, 3);
            }
            sqlite3_bind_text(stmt, 4, alert.tier.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, alert.payload_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, alert.regime_at_alert.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, alert.acted_on);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    });
}

void SQLiteStore::add_regime_log_async(const DbRegimeLog& log) {
    pimpl_->enqueue_write([log](sqlite3* db) {
        const char* sql = "INSERT OR REPLACE INTO regime_log (ts, regime, vix, breadth, hy_oas, spx_vs_200ma, detail_json) VALUES (?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, log.ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, log.regime.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, log.vix);
            sqlite3_bind_double(stmt, 4, log.breadth);
            sqlite3_bind_double(stmt, 5, log.hy_oas);
            sqlite3_bind_double(stmt, 6, log.spx_vs_200ma);
            sqlite3_bind_text(stmt, 7, log.detail_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    });
}

void SQLiteStore::add_candidate_async(const DbCandidate& cand) {
    pimpl_->enqueue_write([cand](sqlite3* db) {
        const char* sql = "INSERT INTO candidates (created_ts, screen, instrument_id, entry_zone_low, entry_zone_high, suggested_stop, rr_target, notes, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, cand.created_ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, cand.screen.c_str(), -1, SQLITE_TRANSIENT);
            if (cand.instrument_id > 0) {
                sqlite3_bind_int64(stmt, 3, cand.instrument_id);
            } else {
                sqlite3_bind_null(stmt, 3);
            }
            sqlite3_bind_double(stmt, 4, cand.entry_zone_low);
            sqlite3_bind_double(stmt, 5, cand.entry_zone_high);
            sqlite3_bind_double(stmt, 6, cand.suggested_stop);
            sqlite3_bind_double(stmt, 7, cand.rr_target);
            sqlite3_bind_text(stmt, 8, cand.notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, cand.status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    });
}

} // namespace persistence
} // namespace trader
