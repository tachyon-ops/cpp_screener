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
#include "trader/core/regime_classifier.hpp"
#include "trader/core/telegram.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include "trader/screens/screen_d.hpp"
#include "trader/screens/screen_b.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/screens/screen_a.hpp"
#include "trader/screens/screen_e.hpp"
#include "trader/screens/screen_f.hpp"
#include <webview.h>


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

// Helper to get current UTC date
std::string get_current_utc_date() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%d");
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

    // Update stock seeding metadata with sector ETFs and next earnings dates
    std::cout << "[Engine] Checking/updating Stock metadata for Screen B..." << std::endl;
    std::string today_date = get_current_utc_date();
    
    auto add_days = [](const std::string& date_str, int days) -> std::string {
        struct tm t = {};
        if (sscanf(date_str.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
            t.tm_year -= 1900;
            t.tm_mon -= 1;
            t.tm_mday += days;
            mktime(&t);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
            return buf;
        }
        return date_str;
    };

    for (auto& inst : existing_instruments) {
        if (inst.asset_class == "Stock") {
            nlohmann::json meta = nlohmann::json::object();
            if (!inst.metadata_json.empty()) {
                try {
                    meta = nlohmann::json::parse(inst.metadata_json);
                } catch (...) {}
            }
            meta["asset_type"] = "Stock";
            
            bool updated = false;
            if (inst.symbol == "AAPL") {
                meta["name"] = "Apple Inc.";
                meta["sector_etf_symbol"] = "XLK";
                meta["next_earnings_date"] = add_days(today_date, 30);
                updated = true;
            } else if (inst.symbol == "MSFT") {
                meta["name"] = "Microsoft Corp.";
                meta["sector_etf_symbol"] = "XLK";
                meta["next_earnings_date"] = add_days(today_date, 3); // earnings risk (within 5 trading / 7 calendar days)
                updated = true;
            } else if (inst.symbol == "TSLA") {
                meta["name"] = "Tesla Inc.";
                meta["sector_etf_symbol"] = "XLY";
                meta["next_earnings_date"] = add_days(today_date, 45);
                updated = true;
            } else if (inst.symbol == "NVDA") {
                meta["name"] = "NVIDIA Corp.";
                meta["sector_etf_symbol"] = "XLK";
                meta["next_earnings_date"] = add_days(today_date, 60);
                updated = true;
            }
            
            if (updated) {
                inst.metadata_json = meta.dump();
                store->add_instrument(inst);
                std::cout << "[Engine] Updated metadata for stock " << inst.symbol << ": " << inst.metadata_json << std::endl;
            }
        }
    }
    // Refetch instruments to ensure we have the updated ones
    existing_instruments = store->get_instruments();

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

    // 3.5 Initialize RegimeClassifier, ScreenD & ScreenB
    auto classifier = std::make_shared<core::RegimeClassifier>(store);

    // Initialize Telegram Bot & Alert Dispatcher
    auto tg_bot = std::make_shared<core::TelegramBot>(store, saxo_adapter);
    auto dispatcher = std::make_shared<core::AlertDispatcher>(store, saxo_adapter, tg_bot);

    auto ts_store = std::make_shared<storage::TimeSeriesStore>();
    auto screen_d = std::make_shared<screens::ScreenD>(store, dispatcher);
    auto screen_b = std::make_shared<screens::ScreenB>(store, dispatcher);
    auto screen_a = std::make_shared<screens::ScreenA>(store, ts_store, dispatcher);
    auto screen_e = std::make_shared<screens::ScreenE>(store, dispatcher);
    auto screen_f = std::make_shared<screens::ScreenF>(store, dispatcher);

    // 4. Initialize and start HTTP & WebSocket Server
    // Serving built React files from "./ui/dist"
    auto http_server = std::make_shared<web::HttpServer>(store, saxo_adapter, classifier, screen_d, screen_b, screen_a, screen_e, screen_f, ts_store, "./ui/dist", 8080);
    http_server->start();

    // Register callback for realtime alert propagation
    dispatcher->set_alert_callback([http_server](const std::string& alert_json) {
        http_server->broadcast_alert(alert_json);
    });

    // Start background threads
    tg_bot->start();
    dispatcher->start();

    // Ensure CEF instruments are seeded
    auto bst_opt = store->get_instrument_by_symbol("BST");
    if (!bst_opt) {
        persistence::DbInstrument inst;
        inst.symbol = "BST";
        inst.asset_class = "CEF";
        inst.exchange = "NYSE";
        inst.saxo_uic = 9010;
        inst.metadata_json = "{\"asset_type\":\"Stock\",\"name\":\"BlackRock Science and Technology Trust\",\"leverage_ratio\":0.25}";
        store->add_instrument(inst);
        bst_opt = store->get_instrument_by_symbol("BST");
    }
    auto bst_nav_opt = store->get_instrument_by_symbol("BST.NAV");
    if (!bst_nav_opt) {
        persistence::DbInstrument inst;
        inst.symbol = "BST.NAV";
        inst.asset_class = "CEF_NAV";
        inst.exchange = "NYSE";
        inst.saxo_uic = 9011;
        inst.metadata_json = "{\"asset_type\":\"Stock\",\"name\":\"BlackRock Science and Technology Trust NAV\"}";
        store->add_instrument(inst);
        bst_nav_opt = store->get_instrument_by_symbol("BST.NAV");
    }
    auto nvda_opt = store->get_instrument_by_symbol("NVDA");

    if (bst_opt && bst_nav_opt && nvda_opt) {
        std::cout << "[Engine] Checking/Seeding daily bars for Screen E (BST/BST.NAV) and Screen F (NVDA)..." << std::endl;
        auto bst_bars = store->get_bars_daily(bst_opt->id);
        auto nvda_bars = store->get_bars_daily(nvda_opt->id);

        auto is_weekend_fn = [](const std::string& date_str) -> bool {
            struct tm t = {};
            if (sscanf(date_str.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
                t.tm_year -= 1900;
                t.tm_mon -= 1;
                mktime(&t);
                return (t.tm_wday == 0 || t.tm_wday == 6);
            }
            return false;
        };

        auto get_trading_days_back_fn = [&](const std::string& start_date, int count) -> std::vector<std::string> {
            std::vector<std::string> dates;
            int days_offset = 0;
            while (dates.size() < (size_t)count) {
                std::string d = add_days(start_date, -days_offset);
                if (!is_weekend_fn(d)) {
                    dates.push_back(d);
                }
                days_offset++;
            }
            std::reverse(dates.begin(), dates.end());
            return dates;
        };

        if (bst_bars.size() < 100) {
            std::cout << "[Engine] Seeding mock historical daily bars for BST and BST.NAV..." << std::endl;
            std::vector<std::string> dates = get_trading_days_back_fn(today_date, 100);
            
            std::random_device rd;
            std::mt19937 gen(rd());
            std::normal_distribution<> pct_change(0.0001, 0.008);
            std::uniform_real_distribution<> vol_dist(30000.0, 70000.0);
            
            double nav = 20.0;
            for (size_t i = 0; i < dates.size(); ++i) {
                double ret = pct_change(gen);
                nav *= (1.0 + ret);
                
                double discount = 0.05 + 0.01 * std::sin(static_cast<double>(i) * 0.1);
                
                if (i == dates.size() - 1) {
                    discount = 0.217;
                    nav = 23.00;
                }
                
                double close = nav * (1.0 - discount);
                double open = close * (1.0 - 0.002);
                double high = std::max(nav, close) * (1.0 + 0.003);
                double low = std::min(nav, close) * (1.0 - 0.003);
                double volume = vol_dist(gen);

                persistence::DbBarDaily parent_bar;
                parent_bar.instrument_id = bst_opt->id;
                parent_bar.date = dates[i];
                parent_bar.open = open;
                parent_bar.high = high;
                parent_bar.low = low;
                parent_bar.close = close;
                parent_bar.volume = volume;
                store->add_bar_daily(parent_bar);

                persistence::DbBarDaily nav_bar;
                nav_bar.instrument_id = bst_nav_opt->id;
                nav_bar.date = dates[i];
                nav_bar.open = nav * (1.0 - 0.001);
                nav_bar.high = nav * (1.0 + 0.001);
                nav_bar.low = nav * (1.0 - 0.001);
                nav_bar.close = nav;
                nav_bar.volume = 0;
                store->add_bar_daily(nav_bar);
            }
        }

        if (nvda_bars.empty()) {
            std::cout << "[Engine] Seeding mock daily bars for NVDA..." << std::endl;
            std::vector<std::string> dates = get_trading_days_back_fn(today_date, 100);
            
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<> vol_dist(40000.0, 60000.0);
            
            for (size_t i = 0; i < dates.size(); ++i) {
                double open, high, low, close, volume;
                volume = vol_dist(gen);
                
                if (i == dates.size() - 1) {
                    close = 935.0;
                    open = 922.0;
                    high = 938.0;
                    low = 920.5;
                    volume = 200000.0;
                } else if (i == 20) {
                    high = 920.0;
                    low = 905.0;
                    open = 910.0;
                    close = 912.0;
                } else if (i == 30) {
                    high = 820.0;
                    low = 800.0;
                    open = 815.0;
                    close = 805.0;
                } else if (i >= 79) {
                    std::uniform_real_distribution<> price_dist(890.0, 910.0);
                    close = price_dist(gen);
                    open = price_dist(gen);
                    high = std::max(open, close) + 2.0;
                    low = std::min(open, close) - 2.0;
                } else {
                    std::uniform_real_distribution<> price_dist(830.0, 890.0);
                    close = price_dist(gen);
                    open = price_dist(gen);
                    high = std::max(open, close) + 5.0;
                    low = std::min(open, close) - 5.0;
                }
                
                persistence::DbBarDaily bar;
                bar.instrument_id = nvda_opt->id;
                bar.date = dates[i];
                bar.open = open;
                bar.high = high;
                bar.low = low;
                bar.close = close;
                bar.volume = volume;
                store->add_bar_daily(bar);
            }
        }
    }

    // Prime the data with an initial evaluation
    std::cout << "[Engine] Seeding/Calculating initial EOD regime and Screen D rotation board..." << std::endl;
    classifier->evaluate(today_date, 14.5, 3.25, 0.76, 520.0);
    screen_d->evaluate(today_date);
    screen_b->evaluate(today_date);
    screen_e->evaluate(today_date);
    screen_f->evaluate(today_date);

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

    // 5.5 Pre-populate TimeSeriesStore with historical bars
    std::cout << "[Engine] Pre-populating TimeSeriesStore with mock historical intraday bars..." << std::endl;
    for (const auto& inst : existing_instruments) {
        double base_price = 150.0;
        auto daily_bars = store->get_bars_daily(inst.id);
        if (!daily_bars.empty()) {
            base_price = daily_bars.back().close;
        }
        ts_store->pre_populate(inst.symbol, base_price);
    }

    // 6. Subscribe to live quotes on seeded instruments to simulate screener data feed
    std::cout << "[Engine] Connecting quote subscriptions to core screener feed..." << std::endl;
    existing_instruments = store->get_instruments();
    for (const auto& inst : existing_instruments) {
        core::InstrumentId id;
        id.broker = "saxo";
        id.native_id = inst.symbol; // Saxo fallback resolves on symbol keywords
        id.asset_type = inst.asset_class == "ETF" ? "Etf" : "Stock";

        saxo_adapter->subscribe_quotes(id, [http_server, inst, ts_store](const core::Tick& tick) {
            ts_store->get_or_create(inst.symbol)->append_tick(tick);

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
    std::thread screener_thread([store, http_server, classifier, screen_d, screen_b, dispatcher, screen_a, screen_e, screen_f]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<> vix_walk(0.0, 0.4);
        std::normal_distribution<> oas_walk(0.0, 0.05);
        std::normal_distribution<> breadth_walk(0.0, 0.02);
        std::normal_distribution<> spy_walk(0.1, 1.5);

        double vix = 14.5;
        double hy_oas = 3.25;
        double breadth = 0.76;
        double spy_price = 520.0;

        while (run_engine) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            if (!run_engine) break;

            // Random walk macro updates
            vix += vix_walk(gen);
            if (vix < 9.0) vix = 9.0;
            if (vix > 45.0) vix = 45.0;

            hy_oas += oas_walk(gen);
            if (hy_oas < 1.5) hy_oas = 1.5;
            if (hy_oas > 8.0) hy_oas = 8.0;

            breadth += breadth_walk(gen);
            if (breadth < 0.1) breadth = 0.1;
            if (breadth > 0.99) breadth = 0.99;

            spy_price += spy_walk(gen);
            if (spy_price < 200.0) spy_price = 200.0;

            std::string date = get_current_utc_date();

            // Run calculations
            core::Regime regime = classifier->evaluate(date, vix, hy_oas, breadth, spy_price);
            screen_d->evaluate(date);
            screen_b->evaluate(date);
            screen_a->evaluate(date);
            screen_e->evaluate(date);
            screen_f->evaluate(date);

            // Fetch the updated log to broadcast
            auto logs = store->get_regime_log(1);
            if (!logs.empty()) {
                const auto& log = logs[0];
                nlohmann::json r_json;
                r_json["ts"] = log.ts;
                r_json["regime"] = log.regime;
                r_json["vix"] = log.vix;
                r_json["breadth"] = log.breadth;
                r_json["hy_oas"] = log.hy_oas;
                r_json["spx_vs_200ma"] = log.spx_vs_200ma;
                try {
                    r_json["detail"] = nlohmann::json::parse(log.detail_json);
                } catch (...) {
                    r_json["detail"] = log.detail_json;
                }
                http_server->broadcast_regime(r_json.dump());
            }
        }
    });

    // 8. Block main thread until shutdown signal, or run webview if supported/enabled
    bool gui_enabled = true;
    
    // Check environment variable on Linux for headless setup
#if defined(__linux__)
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
        gui_enabled = false;
    }
#endif

    if (gui_enabled) {
        try {
            std::cout << "[Engine] Opening native GUI window..." << std::endl;
            webview::webview w(true, nullptr);
            w.set_title("Tachyon Trading Dashboard");
            w.set_size(1400, 900, WEBVIEW_HINT_NONE);
            w.navigate("http://localhost:8080");
            w.run();
            // When window is closed:
            run_engine = false;
        } catch (const std::exception& e) {
            std::cerr << "[Engine] GUI window failed to start: " << e.what() << ". Falling back to headless mode..." << std::endl;
            gui_enabled = false;
        }
    }

    if (!gui_enabled) {
        std::cout << "[Engine] Running in headless mode. Press Ctrl+C to terminate." << std::endl;
        while (run_engine) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }


    // 9. Cleanup and graceful shutdown
    std::cout << "[Engine] Shutting down background components..." << std::endl;
    if (screener_thread.joinable()) {
        screener_thread.join();
    }
    dispatcher->stop();
    tg_bot->stop();
    http_server->stop();
    std::cout << "[Engine] Shutdown complete. Goodbye." << std::endl;
    return 0;
}
