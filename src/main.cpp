#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <random>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include "trader/persistence/sqlite_store.hpp"
#include "trader/broker/saxo_adapter.hpp"
#include "trader/web/http_server.hpp"

using namespace trader;

std::atomic<bool> run_engine{true};

void signal_handler(int) {
    std::cout << "\n[Engine] Signal received. Shutting down..." << std::endl;
    run_engine = false;
}

// Helper to get current ISO 8601 UTC timestamp
std::string get_current_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

int main() {
    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==================================================" << std::endl;
    std::cout << "        TACHYON TRADING ENGINE (C++)              " << std::endl;
    std::cout << "==================================================" << std::endl;

    // 1. Initialize SQLite persistence store
    auto store = std::make_shared<persistence::SQLiteStore>("./data/screener.db");
    try {
        store->init_schema();
        std::cout << "[Engine] SQLite Persistence database initialized." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Engine] SQLite initialization failed: " << e.what() << std::endl;
        return 1;
    }

    // 2. Populate default instruments if empty
    auto existing_instruments = store->get_instruments();
    if (existing_instruments.empty()) {
        std::cout << "[Engine] Seeding initial instrument universe..." << std::endl;
        std::vector<persistence::DbInstrument> initial_universe = {
            {0, "SPY", "ETF", "ARCA", 9001, "{\"asset_type\":\"Etf\"}"},
            {0, "QQQ", "ETF", "NASDAQ", 9002, "{\"asset_type\":\"Etf\"}"},
            {0, "AAPL", "Stock", "NASDAQ", 9003, "{\"asset_type\":\"Stock\"}"},
            {0, "MSFT", "Stock", "NASDAQ", 9004, "{\"asset_type\":\"Stock\"}"},
            {0, "TSLA", "Stock", "NASDAQ", 9005, "{\"asset_type\":\"Stock\"}"},
            {0, "NVDA", "Stock", "NASDAQ", 9006, "{\"asset_type\":\"Stock\"}"}
        };
        for (const auto& inst : initial_universe) {
            store->add_instrument(inst);
        }
        existing_instruments = store->get_instruments();
    }

    // 3. Initialize Saxo Broker Adapter
    broker::SaxoBrokerConfig broker_config;
    broker_config.token_db_path = "./data/tokens.db";
    // Default encryption key matching original Node.js decryption settings
    broker_config.token_encryption_key = "c55f7b0566a4b5f6a2406d8d7b3a9242dda2e55ec3d136892a4d9950a908b4cc";

    auto saxo_adapter = std::make_shared<broker::SaxoBrokerAdapter>(broker_config);
    std::cout << "[Engine] Authenticating Saxo Bank API..." << std::endl;
    auto auth_res = saxo_adapter->authenticate();
    if (auth_res.is_ok()) {
        std::cout << "[Engine] Saxo Bank API successfully authenticated." << std::endl;
    } else {
        std::cout << "[Engine] Warning: Saxo auth failed (" << auth_res.error() 
                  << "). Running in simulated-quote mode." << std::endl;
    }

    // 4. Initialize and start HTTP & WebSocket Server
    // Serving built React files from "./ui/dist"
    auto http_server = std::make_shared<web::HttpServer>(store, saxo_adapter, "./ui/dist", 8080);
    http_server->start();

    // 5. Seed sample regime & alerts if tables are empty, so the UI is immediately wowed
    if (store->get_alerts(1).empty()) {
        std::cout << "[Engine] Seeding initial alert logs..." << std::endl;
        
        persistence::DbRegimeLog r_log;
        r_log.ts = get_current_utc_timestamp();
        r_log.regime = "bull";
        r_log.vix = 14.2;
        r_log.breadth = 0.76;
        r_log.hy_oas = 3.25;
        r_log.spx_vs_200ma = 0.045;
        r_log.detail_json = "{\"comment\":\"Broad market indices in steady uptrend\"}";
        store->add_regime_log(r_log);

        persistence::DbAlert sample_alert1;
        sample_alert1.ts = get_current_utc_timestamp();
        sample_alert1.screen = "B";
        sample_alert1.instrument_id = existing_instruments[0].id; // SPY
        sample_alert1.tier = "premium";
        sample_alert1.payload_json = "{\"symbol\":\"SPY\",\"price\":520.45,\"trigger\":\"Pullback to 20-day EMA in structural uptrend\",\"ema_20\":519.80,\"rsi_14\":43.2}";
        sample_alert1.regime_at_alert = "bull";
        sample_alert1.acted_on = 0;
        store->add_alert(sample_alert1);

        persistence::DbAlert sample_alert2;
        sample_alert2.ts = get_current_utc_timestamp();
        sample_alert2.screen = "F";
        sample_alert2.instrument_id = existing_instruments[2].id; // AAPL
        sample_alert2.tier = "opportunity";
        sample_alert2.payload_json = "{\"symbol\":\"AAPL\",\"price\":182.10,\"trigger\":\"Darvas Box upper boundary breakout\",\"box_top\":181.50,\"box_bottom\":175.20,\"vol_ratio\":1.85}";
        sample_alert2.regime_at_alert = "bull";
        sample_alert2.acted_on = 0;
        store->add_alert(sample_alert2);

        persistence::DbCandidate cand1;
        cand1.created_ts = get_current_utc_timestamp();
        cand1.screen = "B";
        cand1.instrument_id = existing_instruments[0].id;
        cand1.entry_zone_low = 518.0;
        cand1.entry_zone_high = 521.0;
        cand1.suggested_stop = 512.5;
        cand1.rr_target = 3.0;
        cand1.notes = "SPY Pullback entry. Risk-reward targets 3:1.";
        cand1.status = "active";
        store->add_candidate(cand1);
    }

    // 6. Subscribe to live quotes on seeded instruments to simulate screener data feed
    std::cout << "[Engine] Connecting quote subscriptions to core screener feed..." << std::endl;
    for (const auto& inst : existing_instruments) {
        core::InstrumentId id;
        id.broker = "saxo";
        id.native_id = inst.symbol; // Saxo fallback resolves on symbol keywords
        id.asset_type = inst.asset_class == "ETF" ? "Etf" : "Stock";

        saxo_adapter->subscribe_quotes(id, [http_server, inst](const core::Tick& tick) {
            nlohmann::json tick_data;
            tick_data["symbol"] = inst.symbol;
            tick_data["instrument_id"] = inst.id;
            tick_data["ts"] = tick.ts.ms_since_epoch;
            tick_data["bid"] = tick.bid.value;
            tick_data["ask"] = tick.ask.value;
            tick_data["last"] = (tick.bid.value + tick.ask.value) / 2.0;
            
            http_server->broadcast_tick(tick_data.dump());
        });
    }

    // 7. Background screener thread to periodically evaluate regimes & screens
    std::thread screener_thread([store, http_server]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> vix_dist(12.0, 22.0);
        std::uniform_real_distribution<> breadth_dist(0.3, 0.9);
        std::vector<std::string> regimes = {"bull", "chop", "stress", "crisis"};
        std::uniform_int_distribution<> regime_dist(0, regimes.size() - 1);
        std::uniform_int_distribution<> alert_chance(1, 10);

        while (run_engine) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (!run_engine) break;

            // Random walk/simulate regime updates
            persistence::DbRegimeLog r;
            r.ts = get_current_utc_timestamp();
            r.regime = regimes[regime_dist(gen)];
            r.vix = vix_dist(gen);
            r.breadth = breadth_dist(gen);
            r.hy_oas = r.vix * 0.25;
            r.spx_vs_200ma = (r.regime == "bull" ? 0.05 : (r.regime == "crisis" ? -0.08 : -0.01));
            r.detail_json = "{\"simulated\":true}";

            store->add_regime_log_async(r);

            nlohmann::json r_json;
            r_json["ts"] = r.ts;
            r_json["regime"] = r.regime;
            r_json["vix"] = r.vix;
            r_json["breadth"] = r.breadth;
            r_json["hy_oas"] = r.hy_oas;
            r_json["spx_vs_200ma"] = r.spx_vs_200ma;
            r_json["detail"] = nlohmann::json::object();
            http_server->broadcast_regime(r_json.dump());

            // Low chance of generating a new screen alert
            if (alert_chance(gen) > 7) {
                auto insts = store->get_instruments();
                if (!insts.empty()) {
                    std::uniform_int_distribution<> inst_dist(0, insts.size() - 1);
                    const auto& target_inst = insts[inst_dist(gen)];

                    std::vector<std::string> screens = {"A", "B", "C", "D", "E", "F", "G"};
                    std::uniform_int_distribution<> screen_dist(0, screens.size() - 1);
                    std::string selected_screen = screens[screen_dist(gen)];

                    persistence::DbAlert a;
                    a.ts = get_current_utc_timestamp();
                    a.screen = selected_screen;
                    a.instrument_id = target_inst.id;
                    a.tier = (selected_screen == "B" || selected_screen == "F" ? "premium" : "opportunity");
                    
                    nlohmann::json payload;
                    payload["symbol"] = target_inst.symbol;
                    payload["price"] = 100.0 + (inst_dist(gen) * 15.2);
                    payload["trigger"] = "Screen " + selected_screen + " event triggered on " + target_inst.symbol;
                    
                    a.payload_json = payload.dump();
                    a.regime_at_alert = r.regime;
                    a.acted_on = 0;

                    int64_t alert_id = store->add_alert(a);
                    a.id = alert_id;

                    nlohmann::json alert_json;
                    alert_json["id"] = a.id;
                    alert_json["ts"] = a.ts;
                    alert_json["screen"] = a.screen;
                    alert_json["instrument_id"] = a.instrument_id;
                    alert_json["tier"] = a.tier;
                    alert_json["payload"] = payload;
                    alert_json["regime_at_alert"] = a.regime_at_alert;
                    alert_json["acted_on"] = a.acted_on;

                    http_server->broadcast_alert(alert_json.dump());
                }
            }
        }
    });

    // 8. Block main thread until shutdown signal
    while (run_engine) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 9. Cleanup and graceful shutdown
    std::cout << "[Engine] Shutting down background components..." << std::endl;
    if (screener_thread.joinable()) {
        screener_thread.join();
    }
    http_server->stop();
    std::cout << "[Engine] Shutdown complete. Goodbye." << std::endl;
    return 0;
}
