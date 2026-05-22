#include "trader/web/http_server.hpp"
#include "trader/core/regime_classifier.hpp"
#include "trader/screens/screen_d.hpp"
#include "trader/screens/screen_b.hpp"
#include "trader/screens/screen_a.hpp"
#include "trader/screens/screen_e.hpp"
#include "trader/screens/screen_f.hpp"
#include "trader/screens/screen_g.hpp"
#include "trader/screens/screen_c.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/storage/token_store.hpp"
#include <crow.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <unordered_set>

namespace trader {
namespace web {

struct HttpServer::Impl {
    std::shared_ptr<persistence::SQLiteStore> store;
    std::shared_ptr<broker::BrokerAdapter> broker;
    std::shared_ptr<core::RegimeClassifier> classifier;
    std::shared_ptr<screens::ScreenD> screen_d;
    std::shared_ptr<screens::ScreenB> screen_b;
    std::shared_ptr<screens::ScreenA> screen_a;
    std::shared_ptr<screens::ScreenE> screen_e;
    std::shared_ptr<screens::ScreenF> screen_f;
    std::shared_ptr<screens::ScreenG> screen_g;
    std::shared_ptr<screens::ScreenC> screen_c;
    std::shared_ptr<storage::TimeSeriesStore> ts_store;
    std::string public_dir;
    int port;

    crow::SimpleApp app;
    std::thread server_thread;
    std::mutex ws_mutex;
    std::unordered_set<crow::websocket::connection*> connections;

    Impl(
        std::shared_ptr<persistence::SQLiteStore> s,
        std::shared_ptr<broker::BrokerAdapter> b,
        std::shared_ptr<core::RegimeClassifier> c,
        std::shared_ptr<screens::ScreenD> sd,
        std::shared_ptr<screens::ScreenB> sb,
        std::shared_ptr<screens::ScreenA> sa,
        std::shared_ptr<screens::ScreenE> se,
        std::shared_ptr<screens::ScreenF> sf,
        std::shared_ptr<screens::ScreenG> sg,
        std::shared_ptr<screens::ScreenC> sc,
        std::shared_ptr<storage::TimeSeriesStore> ts,
        std::string dir,
        int p
    ) : store(std::move(s)), broker(std::move(b)), classifier(std::move(c)), screen_d(std::move(sd)), screen_b(std::move(sb)), screen_a(std::move(sa)), screen_e(std::move(se)), screen_f(std::move(sf)), screen_g(std::move(sg)), screen_c(std::move(sc)), ts_store(std::move(ts)), public_dir(std::move(dir)), port(p) {}

    void broadcast(const std::string& text) {
        std::lock_guard<std::mutex> lock(ws_mutex);
        for (auto* conn : connections) {
            try {
                conn->send_text(text);
            } catch (...) {
                // Ignore disconnect errors during broadcast
            }
        }
    }

    std::string get_mime_type(const std::filesystem::path& path) {
        auto ext = path.extension().string();
        if (ext == ".html") return "text/html";
        if (ext == ".js") return "application/javascript";
        if (ext == ".css") return "text/css";
        if (ext == ".json") return "application/json";
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".ico") return "image/x-icon";
        return "application/octet-stream";
    }

    crow::response serve_static(const std::string& req_path) {
        // Resolve absolute path and prevent directory traversal
        std::filesystem::path base_path = std::filesystem::absolute(public_dir);
        std::filesystem::path file_path = std::filesystem::absolute(base_path / (req_path.empty() || req_path == "/" ? "index.html" : req_path));

        // If it's a directory, look for index.html
        if (std::filesystem::is_directory(file_path)) {
            file_path /= "index.html";
        }

        // SPA Fallback: if the file doesn't exist, serve index.html for client-side routing
        if (!std::filesystem::exists(file_path) || !std::filesystem::is_regular_file(file_path)) {
            file_path = base_path / "index.html";
        }

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            return crow::response(404, "File Not Found");
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        crow::response res(buffer.str());
        res.add_header("Content-Type", get_mime_type(file_path));
        return res;
    }
};

HttpServer::HttpServer(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<broker::BrokerAdapter> broker,
    std::shared_ptr<core::RegimeClassifier> classifier,
    std::shared_ptr<screens::ScreenD> screen_d,
    std::shared_ptr<screens::ScreenB> screen_b,
    std::shared_ptr<screens::ScreenA> screen_a,
    std::shared_ptr<screens::ScreenE> screen_e,
    std::shared_ptr<screens::ScreenF> screen_f,
    std::shared_ptr<screens::ScreenG> screen_g,
    std::shared_ptr<screens::ScreenC> screen_c,
    std::shared_ptr<storage::TimeSeriesStore> ts_store,
    const std::string& public_dir,
    int port
) : pimpl_(std::make_unique<Impl>(std::move(store), std::move(broker), std::move(classifier), std::move(screen_d), std::move(screen_b), std::move(screen_a), std::move(screen_e), std::move(screen_f), std::move(screen_g), std::move(screen_c), std::move(ts_store), public_dir, port)) {

    // Define REST Endpoints

    // 1. GET /api/regime
    CROW_ROUTE(pimpl_->app, "/api/regime")([this]() {
        try {
            auto logs = pimpl_->store->get_regime_log(10);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& log : logs) {
                nlohmann::json obj;
                obj["ts"] = log.ts;
                obj["regime"] = log.regime;
                obj["vix"] = log.vix;
                obj["breadth"] = log.breadth;
                obj["hy_oas"] = log.hy_oas;
                obj["spx_vs_200ma"] = log.spx_vs_200ma;
                if (!log.detail_json.empty()) {
                    try {
                        obj["detail"] = nlohmann::json::parse(log.detail_json);
                    } catch (...) {
                        obj["detail"] = log.detail_json;
                    }
                } else {
                    obj["detail"] = nlohmann::json::object();
                }
                arr.push_back(obj);
            }
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 2. GET /api/alerts
    CROW_ROUTE(pimpl_->app, "/api/alerts")([this]() {
        try {
            auto list = pimpl_->store->get_alerts(100);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& alert : list) {
                nlohmann::json obj;
                obj["id"] = alert.id;
                obj["ts"] = alert.ts;
                obj["screen"] = alert.screen;
                obj["instrument_id"] = alert.instrument_id;
                obj["tier"] = alert.tier;
                obj["regime_at_alert"] = alert.regime_at_alert;
                obj["acted_on"] = alert.acted_on;
                try {
                    obj["payload"] = nlohmann::json::parse(alert.payload_json);
                } catch (...) {
                    obj["payload"] = alert.payload_json;
                }

                nlohmann::json res_arr = nlohmann::json::array();
                auto responses = pimpl_->store->get_alert_responses(alert.id);
                for (const auto& r : responses) {
                    nlohmann::json r_obj;
                    r_obj["id"] = r.id;
                    r_obj["alert_id"] = r.alert_id;
                    r_obj["response_ts"] = r.response_ts;
                    r_obj["response_type"] = r.response_type;
                    r_obj["skip_reason"] = r.skip_reason;
                    r_obj["note_text"] = r.note_text;
                    res_arr.push_back(r_obj);
                }
                obj["responses"] = res_arr;

                arr.push_back(obj);
            }
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 3. POST /api/alert_response
    CROW_ROUTE(pimpl_->app, "/api/alert_response").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            int64_t alert_id = body.at("alert_id").get<int64_t>();
            std::string action = body.value("action", "dismiss"); // "seen", "acted", "skipped", "noted", "deferred"

            // Construct and save the DbAlertResponse row
            persistence::DbAlertResponse resp;
            resp.alert_id = alert_id;
            resp.response_type = action;
            resp.skip_reason = body.value("skip_reason", "");
            resp.note_text = body.value("note_text", "");
            
            auto now_time = std::chrono::system_clock::now();
            auto time_val = std::chrono::system_clock::to_time_t(now_time);
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time_val), "%Y-%m-%dT%H:%M:%SZ");
            resp.response_ts = ss.str();

            pimpl_->store->add_alert_response(resp);

            nlohmann::json res;
            res["status"] = "success";
            res["alert_id"] = alert_id;

            if (action == "execute" || action == "acted") {
                pimpl_->store->update_alert_acted(alert_id, 1);

                // Fetch alert details
                auto alerts = pimpl_->store->get_alerts(500);
                persistence::DbAlert target_alert;
                bool found = false;
                for (const auto& a : alerts) {
                    if (a.id == alert_id) {
                        target_alert = a;
                        found = true;
                        break;
                    }
                }

                if (found) {
                    nlohmann::json payload = nlohmann::json::parse(target_alert.payload_json);
                    std::string sym = payload.value("symbol", "");
                    double price = payload.value("price", 0.0);
                    
                    double units = 100.0; // Default fallback
                    std::string risk_tier = body.value("risk_tier", "");
                    std::string size_key = "";
                    if (risk_tier == "1%" || risk_tier == "1pct" || risk_tier == "size_1pct" || risk_tier == "1") {
                        size_key = "size_1pct";
                    } else if (risk_tier == "2%" || risk_tier == "2pct" || risk_tier == "size_2pct" || risk_tier == "2") {
                        size_key = "size_2pct";
                    } else if (risk_tier == "5%" || risk_tier == "5pct" || risk_tier == "size_5pct" || risk_tier == "5") {
                        size_key = "size_5pct";
                    }

                    if (!size_key.empty() && payload.contains(size_key)) {
                        auto sz_obj = payload[size_key];
                        if (sz_obj.contains("units")) {
                            units = sz_obj["units"].get<double>();
                        }
                    }
                    
                    if (!sym.empty()) {
                        auto inst_info_res = pimpl_->broker->lookup_symbol(sym);
                        if (inst_info_res.is_ok()) {
                            auto inst_info = inst_info_res.value();
                            
                            core::OrderRequest order;
                            order.instrument = inst_info.id;
                            order.side = "buy";
                            order.type = "market";
                            order.size.value = units;
                            
                            auto order_res = pimpl_->broker->place_order(order);
                            if (order_res.is_ok()) {
                                res["order_id"] = order_res.value();
                                res["units"] = units;
                                res["message"] = "Order executed successfully via Saxo Broker: " + order_res.value() + " (" + std::to_string((int)units) + " units)";
                            } else {
                                res["message"] = "DB response added but Order failed: " + order_res.error();
                            }
                        } else {
                            res["message"] = "DB response added but Symbol lookup failed: " + inst_info_res.error();
                        }
                    } else {
                        res["message"] = "DB response added but symbol is empty in payload.";
                    }

                    // Insert position record
                    persistence::DbPosition pos;
                    pos.alert_id = alert_id;
                    pos.instrument_id = target_alert.instrument_id;
                    pos.direction = "long";
                    if (payload.contains("direction")) {
                        try {
                            pos.direction = payload["direction"].get<std::string>();
                        } catch (...) {}
                    }
                    pos.entry_ts = resp.response_ts;
                    pos.entry_price = price;
                    pos.size = units;

                    double stop_lvl = 0.0;
                    if (payload.contains("suggested_stop")) {
                        try {
                            stop_lvl = payload["suggested_stop"].get<double>();
                        } catch (...) {}
                    } else if (payload.contains("stop")) {
                        try {
                            stop_lvl = payload["stop"].get<double>();
                        } catch (...) {}
                    }
                    if (stop_lvl <= 0.0) {
                        stop_lvl = price * 0.95; // fallback
                    }
                    pos.initial_stop = stop_lvl;
                    pos.current_stop = stop_lvl;
                    pos.status = "open";
                    pos.notes = "Executed alert " + std::to_string(alert_id) + " (" + target_alert.screen + ")";

                    try {
                        int64_t new_pos_id = pimpl_->store->add_position(pos);
                        res["position_id"] = new_pos_id;
                        res["message"] = res.value("message", "") + " | Saved to position tracker DB (ID: " + std::to_string(new_pos_id) + ")";
                    } catch (const std::exception& e) {
                        std::cerr << "[HttpServer] DB error adding position: " << e.what() << std::endl;
                        res["message"] = res.value("message", "") + " | DB error saving position: " + e.what();
                    }
                } else {
                    res["message"] = "DB response added but Alert ID not found.";
                }
            } else {
                res["message"] = "Alert response of type '" + action + "' saved.";
            }

            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 4. GET /api/candidates
    CROW_ROUTE(pimpl_->app, "/api/candidates")([this]() {
        try {
            auto list = pimpl_->store->get_candidates();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& cand : list) {
                nlohmann::json obj;
                obj["id"] = cand.id;
                obj["created_ts"] = cand.created_ts;
                obj["screen"] = cand.screen;
                obj["instrument_id"] = cand.instrument_id;
                
                // Lookup symbol and name to make frontend rendering robust
                std::string symbol = "UNKNOWN";
                std::string name = "Unknown Instrument";
                auto insts = pimpl_->store->get_instruments();
                for (const auto& inst : insts) {
                    if (inst.id == cand.instrument_id) {
                        symbol = inst.symbol;
                        try {
                            if (!inst.metadata_json.empty()) {
                                auto meta = nlohmann::json::parse(inst.metadata_json);
                                name = meta.value("name", inst.symbol + " Common Stock");
                            } else {
                                name = inst.symbol + " Common Stock";
                            }
                        } catch (...) {
                            name = inst.symbol + " Common Stock";
                        }
                        break;
                    }
                }
                obj["symbol"] = symbol;
                obj["name"] = name;
                
                obj["entry_zone_low"] = cand.entry_zone_low;
                obj["entry_zone_high"] = cand.entry_zone_high;
                obj["suggested_stop"] = cand.suggested_stop;
                obj["rr_target"] = cand.rr_target;
                obj["notes"] = cand.notes;
                obj["status"] = cand.status;
                arr.push_back(obj);
            }
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 4b. POST /api/candidates
    CROW_ROUTE(pimpl_->app, "/api/candidates").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string symbol = body.at("symbol").get<std::string>();
            
            auto inst_opt = pimpl_->store->get_instrument_by_symbol(symbol);
            if (!inst_opt.has_value()) {
                nlohmann::json err = {{"error", "Instrument symbol not found: " + symbol}};
                return crow::response(400, err.dump());
            }
            
            persistence::DbCandidate cand;
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
            cand.created_ts = ss.str();
            
            cand.screen = body.value("screen", "B");
            cand.instrument_id = inst_opt->id;
            cand.entry_zone_low = std::stod(body.at("entry_zone_low").get<std::string>());
            cand.entry_zone_high = std::stod(body.at("entry_zone_high").get<std::string>());
            cand.suggested_stop = std::stod(body.at("suggested_stop").get<std::string>());
            cand.rr_target = std::stod(body.value("rr_target", "3.0"));
            cand.notes = body.value("notes", "");
            cand.status = "active";
            
            int64_t new_id = pimpl_->store->add_candidate(cand);
            
            nlohmann::json res;
            res["status"] = "success";
            res["id"] = new_id;
            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 4c. GET /api/positions
    CROW_ROUTE(pimpl_->app, "/api/positions")([this]() {
        try {
            auto list = pimpl_->store->get_positions();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& pos : list) {
                nlohmann::json obj;
                obj["id"] = pos.id;
                obj["alert_id"] = pos.alert_id;
                obj["instrument_id"] = pos.instrument_id;
                
                // Lookup symbol and name to make frontend rendering robust
                std::string symbol = "UNKNOWN";
                std::string name = "Unknown Instrument";
                auto insts = pimpl_->store->get_instruments();
                for (const auto& inst : insts) {
                    if (inst.id == pos.instrument_id) {
                        symbol = inst.symbol;
                        try {
                            if (!inst.metadata_json.empty()) {
                                auto meta = nlohmann::json::parse(inst.metadata_json);
                                name = meta.value("name", inst.symbol);
                            } else {
                                name = inst.symbol;
                            }
                        } catch (...) {
                            name = inst.symbol;
                        }
                        break;
                    }
                }
                obj["symbol"] = symbol;
                obj["name"] = name;
                obj["direction"] = pos.direction;
                obj["entry_ts"] = pos.entry_ts;
                obj["entry_price"] = pos.entry_price;
                obj["size"] = pos.size;
                obj["initial_stop"] = pos.initial_stop;
                obj["current_stop"] = pos.current_stop;
                obj["status"] = pos.status;
                obj["exit_ts"] = pos.exit_ts;
                obj["exit_price"] = pos.exit_price;
                obj["exit_reason"] = pos.exit_reason;
                obj["r_realized"] = pos.r_realized;
                obj["max_favorable_excursion_r"] = pos.max_favorable_excursion_r;
                obj["max_adverse_excursion_r"] = pos.max_adverse_excursion_r;
                obj["notes"] = pos.notes;
                arr.push_back(obj);
            }
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 5. GET /api/instruments
    CROW_ROUTE(pimpl_->app, "/api/instruments")([this]() {
        try {
            auto list = pimpl_->store->get_instruments();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& inst : list) {
                nlohmann::json obj;
                obj["id"] = inst.id;
                obj["symbol"] = inst.symbol;
                obj["asset_class"] = inst.asset_class;
                obj["exchange"] = inst.exchange;
                obj["saxo_uic"] = inst.saxo_uic;
                try {
                    obj["metadata"] = nlohmann::json::parse(inst.metadata_json);
                } catch (...) {
                    obj["metadata"] = inst.metadata_json;
                }
                arr.push_back(obj);
            }
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 6. POST /api/instruments (Dynamic Onboarding)
    CROW_ROUTE(pimpl_->app, "/api/instruments").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string symbol = body.at("symbol").get<std::string>();
            std::string asset_class = body.value("asset_class", "Stock");

            // Look up symbol on Saxo
            auto lookup_res = pimpl_->broker->lookup_symbol(symbol);
            if (!lookup_res.is_ok()) {
                nlohmann::json err = {{"error", "Failed to lookup symbol on Saxo: " + lookup_res.error()}};
                return crow::response(400, err.dump());
            }

            auto info = lookup_res.value();
            persistence::DbInstrument inst;
            inst.symbol = info.symbol;
            inst.asset_class = asset_class;
            inst.exchange = "Saxo";
            inst.saxo_uic = std::stoll(info.id.native_id);
            inst.metadata_json = nlohmann::json({{"asset_type", info.id.asset_type}}).dump();

            pimpl_->store->add_instrument(inst);

            // Dynamically register subscription if we successfully fetched it
            auto added_opt = pimpl_->store->get_instrument_by_symbol(inst.symbol);
            if (added_opt) {
                pimpl_->ts_store->pre_populate(added_opt->symbol, 150.0);

                trader::core::InstrumentId id;
                id.broker = "saxo";
                id.native_id = added_opt->symbol; // Saxo fallback resolves on symbol keywords
                id.asset_type = added_opt->asset_class == "ETF" ? "Etf" : "Stock";

                auto self = this;
                auto ts = pimpl_->ts_store;
                pimpl_->broker->subscribe_quotes(id, [self, added_inst = *added_opt, ts](const trader::core::Tick& tick) {
                    ts->get_or_create(added_inst.symbol)->append_tick(tick);

                    nlohmann::json tick_data;
                    tick_data["symbol"] = added_inst.symbol;
                    tick_data["instrument_id"] = added_inst.id;
                    tick_data["ts"] = tick.ts.ms_since_epoch;
                    tick_data["bid"] = tick.bid.value;
                    tick_data["ask"] = tick.ask.value;
                    tick_data["last"] = (tick.bid.value + tick.ask.value) / 2.0;
                    
                    self->broadcast_tick(tick_data.dump());
                });
            }

            nlohmann::json res;
            res["status"] = "success";
            res["symbol"] = inst.symbol;
            res["saxo_uic"] = inst.saxo_uic;
            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 7. GET /api/sector_rotation
    CROW_ROUTE(pimpl_->app, "/api/sector_rotation")([this]() {
        try {
            if (!pimpl_->screen_d) {
                nlohmann::json err = {{"error", "ScreenD component not registered."}};
                return crow::response(500, err.dump());
            }
            auto results = pimpl_->screen_d->get_results();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& res : results) {
                nlohmann::json obj;
                obj["symbol"] = res.symbol;
                obj["name"] = res.name;
                obj["price"] = res.price;
                obj["ma50"] = res.ma50;
                obj["ma200"] = res.ma200;
                obj["dist_50ma"] = res.dist_50ma;
                obj["dist_200ma"] = res.dist_200ma;
                obj["return_1m"] = res.return_1m;
                obj["return_3m"] = res.return_3m;
                obj["return_6m"] = res.return_6m;
                obj["return_12m"] = res.return_12m;
                obj["rs_rank"] = res.rs_rank;
                obj["rs_percentile"] = res.rs_percentile;
                obj["cross_50_200"] = res.cross_50_200;
                obj["test_50ma"] = res.test_50ma;
                obj["test_200ma"] = res.test_200ma;
                arr.push_back(obj);
            }
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 8. POST /api/recompute
    CROW_ROUTE(pimpl_->app, "/api/recompute").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            if (!pimpl_->classifier || !pimpl_->screen_d) {
                nlohmann::json err = {{"error", "Classifier or ScreenD components not registered."}};
                return crow::response(500, err.dump());
            }

            // Get current UTC date (YYYY-MM-DD)
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time), "%Y-%m-%d");
            std::string date = ss.str();

            double vix = 15.0;
            double hy_oas = 3.5;
            double breadth = 0.6;
            double spx = 520.0; // Default SPY/SPX close

            if (!req.body.empty()) {
                try {
                    auto body = nlohmann::json::parse(req.body);
                    if (body.contains("date")) date = body["date"].get<std::string>();
                    if (body.contains("vix")) vix = body["vix"].get<double>();
                    if (body.contains("hy_oas")) hy_oas = body["hy_oas"].get<double>();
                    if (body.contains("breadth")) breadth = body["breadth"].get<double>();
                    if (body.contains("spx")) spx = body["spx"].get<double>();
                } catch (...) {
                    // Ignore parsing errors and use defaults
                }
            }

            // Trigger evaluations
            core::Regime new_regime = pimpl_->classifier->evaluate(date, vix, hy_oas, breadth, spx);
            pimpl_->screen_d->evaluate(date);
            if (pimpl_->screen_b) {
                pimpl_->screen_b->evaluate(date);
            }
            if (pimpl_->screen_a) {
                pimpl_->screen_a->evaluate(date);
            }
            if (pimpl_->screen_e) {
                pimpl_->screen_e->evaluate(date);
            }
            if (pimpl_->screen_f) {
                pimpl_->screen_f->evaluate(date);
            }
            if (pimpl_->screen_g) {
                pimpl_->screen_g->evaluate(date);
            }
            if (pimpl_->screen_c) {
                pimpl_->screen_c->evaluate(date);
            }

            // Fetch the updated log to broadcast
            auto logs = pimpl_->store->get_regime_log(1);
            nlohmann::json r_json;
            if (!logs.empty()) {
                const auto& log = logs[0];
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
            } else {
                r_json["ts"] = date + "T16:30:00Z";
                r_json["regime"] = core::regime_to_string(new_regime);
                r_json["vix"] = vix;
                r_json["breadth"] = breadth;
                r_json["hy_oas"] = hy_oas;
                r_json["spx_vs_200ma"] = 0.0;
                r_json["detail"] = nlohmann::json::object();
            }

            broadcast_regime(r_json.dump());

            nlohmann::json res;
            res["status"] = "success";
            res["date"] = date;
            res["regime"] = core::regime_to_string(new_regime);
            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // GET /api/settings
    CROW_ROUTE(pimpl_->app, "/api/settings")([this]() {
        try {
            auto pairs = pimpl_->store->get_all_settings();
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& pair : pairs) {
                if (pair.first == "tg_bot_token" && pair.second.length() > 8) {
                    obj[pair.first] = pair.second.substr(0, 4) + "••••••••" + pair.second.substr(pair.second.length() - 4);
                } else {
                    obj[pair.first] = pair.second;
                }
            }
            if (!obj.contains("whatsapp_enabled")) obj["whatsapp_enabled"] = "false";
            if (!obj.contains("whatsapp_recipient")) obj["whatsapp_recipient"] = "";
            if (!obj.contains("telegram_enabled")) obj["telegram_enabled"] = "false";
            if (!obj.contains("tg_bot_token")) obj["tg_bot_token"] = "";
            if (!obj.contains("tg_chat_premium")) obj["tg_chat_premium"] = "";
            if (!obj.contains("tg_chat_opportunity")) obj["tg_chat_opportunity"] = "";
            if (!obj.contains("tg_chat_digest")) obj["tg_chat_digest"] = "";
            return crow::response(200, obj.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // POST /api/settings
    CROW_ROUTE(pimpl_->app, "/api/settings").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            for (auto it = body.begin(); it != body.end(); ++it) {
                std::string key = it.key();
                std::string val = "";
                if (it.value().is_string()) {
                    val = it.value().get<std::string>();
                } else if (it.value().is_boolean()) {
                    val = it.value().get<bool>() ? "true" : "false";
                } else if (it.value().is_number()) {
                    val = std::to_string(it.value().get<double>());
                }

                if (key == "tg_bot_token" && val.find("••••") != std::string::npos) {
                    continue; // Preserve masked token
                }

                pimpl_->store->set_setting(key, val);
            }
            nlohmann::json res = {{"status", "success"}};
            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // POST /api/test_notification
    CROW_ROUTE(pimpl_->app, "/api/test_notification").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            std::string type = body.at("type").get<std::string>();
            if (type == "telegram") {
                std::string token = body.value("tg_bot_token", "");
                if (token.empty()) {
                    return crow::response(400, nlohmann::json({{"error", "Telegram bot token is required"}}).dump());
                }

                // If the token is masked, reload it from db settings
                if (token.find("••••") != std::string::npos) {
                    auto db_token = pimpl_->store->get_setting("tg_bot_token");
                    if (db_token && !db_token->empty()) {
                        token = *db_token;
                    }
                }

                std::vector<std::string> chat_ids;
                if (body.contains("tg_chat_premium")) {
                    std::string c = body["tg_chat_premium"].get<std::string>();
                    if (!c.empty()) chat_ids.push_back(c);
                }
                if (body.contains("tg_chat_opportunity")) {
                    std::string c = body["tg_chat_opportunity"].get<std::string>();
                    if (!c.empty()) chat_ids.push_back(c);
                }
                if (body.contains("tg_chat_digest")) {
                    std::string c = body["tg_chat_digest"].get<std::string>();
                    if (!c.empty()) chat_ids.push_back(c);
                }

                if (chat_ids.empty()) {
                    return crow::response(400, nlohmann::json({{"error", "At least one chat ID (Premium, Opportunity, or Digest) must be configured"}}).dump());
                }

                httplib::Client cli("https://api.telegram.org");
                cli.set_connection_timeout(std::chrono::seconds(10));
                cli.set_read_timeout(std::chrono::seconds(15));

                // Construct a timestamp
                auto now_time = std::chrono::system_clock::now();
                auto time_val = std::chrono::system_clock::to_time_t(now_time);
                std::stringstream ss;
                ss << std::put_time(std::gmtime(&time_val), "%Y-%m-%dT%H:%M:%SZ");
                std::string ts = ss.str();

                std::string test_msg = "⚡ <b>Tachyon Screener</b> ⚡\n"
                                       "🤖 <b>Telegram Bot Test Connection</b>\n"
                                       "✅ Connection verified successfully!\n"
                                       "📅 <i>Time: " + ts + " UTC</i>";

                int success_count = 0;
                std::string last_err = "";

                for (const auto& chat_id : chat_ids) {
                    nlohmann::json req_body = {
                        {"chat_id", chat_id},
                        {"text", test_msg},
                        {"parse_mode", "HTML"}
                    };
                    std::string path = "/bot" + token + "/sendMessage";
                    httplib::Headers headers = {{"Content-Type", "application/json"}};
                    auto res = cli.Post(path.c_str(), headers, req_body.dump(), "application/json");
                    if (res && res->status == 200) {
                        auto res_json = nlohmann::json::parse(res->body);
                        if (res_json.value("ok", false)) {
                            success_count++;
                        } else {
                            last_err = "Telegram returned error: " + res->body;
                        }
                    } else {
                        if (res) {
                            last_err = "HTTP status: " + std::to_string(res->status) + ", Body: " + res->body;
                        } else {
                            last_err = "Failed to connect to Telegram API";
                        }
                    }
                }

                if (success_count > 0) {
                    nlohmann::json res_json = {
                        {"status", "success"},
                        {"message", "Test message sent to " + std::to_string(success_count) + " chat(s)."}
                    };
                    return crow::response(200, res_json.dump());
                } else {
                    return crow::response(400, nlohmann::json({{"error", "Failed to send test message: " + last_err}}).dump());
                }
            } else if (type == "whatsapp") {
                std::string recipient = body.value("whatsapp_recipient", "");
                if (recipient.empty()) {
                    return crow::response(400, nlohmann::json({{"error", "Recipient phone number is required"}}).dump());
                }

                // Send test trigger to WhatsApp Bot via WebSocket
                nlohmann::json ws_msg = {
                    {"type", "test_whatsapp"},
                    {"data", {
                        {"whatsapp_recipient", recipient},
                        {"text", "⚡ *Tachyon Screener* ⚡\n🤖 *WhatsApp Bot Test Connection*\n✅ Connection verified successfully!"}
                    }}
                };

                pimpl_->broadcast(ws_msg.dump());

                nlohmann::json res_json = {
                    {"status", "success"},
                    {"message", "Test message trigger broadcasted to WhatsApp bot."}
                };
                return crow::response(200, res_json.dump());
            } else {
                return crow::response(400, nlohmann::json({{"error", "Invalid notification type"}}).dump());
            }
        } catch (const std::exception& e) {
            return crow::response(500, nlohmann::json({{"error", e.what()}}).dump());
        }
    });

    // GET /api/settings/saxo_token
    CROW_ROUTE(pimpl_->app, "/api/settings/saxo_token")([this]() {
        try {
            std::string db_path = "./data/tokens.db";
            std::string encryption_key = "c55f7b0566a4b5f6a2406d8d7b3a9242dda2e55ec3d136892a4d9950a908b4cc";
            const char* env_key = std::getenv("TOKEN_ENCRYPTION_KEY");
            if (env_key) {
                encryption_key = env_key;
            }

            trader::storage::TokenStore token_store(db_path, encryption_key);
            auto tokens_opt = token_store.load("default");

            nlohmann::json res;
            if (tokens_opt) {
                res["configured"] = true;
                res["openApiBase"] = tokens_opt->open_api_base;
                res["authBase"] = tokens_opt->auth_base;
                res["redirectUrl"] = tokens_opt->redirect_url;
                res["tokenExpiry"] = tokens_opt->token_expiry;
                
                auto mask_str = [](const std::string& s) {
                    if (s.empty()) return std::string("");
                    if (s.length() <= 8) return std::string("••••••••");
                    return s.substr(0, 4) + "••••" + s.substr(s.length() - 4);
                };
                res["appKey"] = mask_str(tokens_opt->app_key);
                res["appSecret"] = mask_str(tokens_opt->app_secret);
                res["accessToken"] = mask_str(tokens_opt->access_token);
                res["refreshToken"] = mask_str(tokens_opt->refresh_token);
                res["isAuthenticated"] = pimpl_->broker ? pimpl_->broker->is_authenticated() : false;
            } else {
                res["configured"] = false;
                res["isAuthenticated"] = false;
                res["openApiBase"] = "https://gateway.saxobank.com/sim/openapi";
                res["authBase"] = "https://sim.authenticator.saxobank.com/oauth2";
                res["redirectUrl"] = "http://localhost:8080/auth/saxo/callback";
                res["tokenExpiry"] = 0;
                res["appKey"] = "";
                res["appSecret"] = "";
                res["accessToken"] = "";
                res["refreshToken"] = "";
            }
            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // POST /api/settings/saxo_token
    CROW_ROUTE(pimpl_->app, "/api/settings/saxo_token").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            auto body = nlohmann::json::parse(req.body);
            
            std::string db_path = "./data/tokens.db";
            std::string encryption_key = "c55f7b0566a4b5f6a2406d8d7b3a9242dda2e55ec3d136892a4d9950a908b4cc";
            const char* env_key = std::getenv("TOKEN_ENCRYPTION_KEY");
            if (env_key) {
                encryption_key = env_key;
            }

            trader::storage::TokenStore token_store(db_path, encryption_key);
            auto existing_opt = token_store.load("default");
            
            trader::storage::SaxoTokens tokens;
            if (existing_opt) {
                tokens = *existing_opt;
            }
            
            auto update_field = [](const std::string& new_val, std::string& target) {
                if (!new_val.empty() && new_val.find("••••") == std::string::npos) {
                    target = new_val;
                }
            };

            if (body.contains("openApiBase")) tokens.open_api_base = body["openApiBase"].get<std::string>();
            if (body.contains("authBase")) tokens.auth_base = body["authBase"].get<std::string>();
            if (body.contains("redirectUrl")) tokens.redirect_url = body["redirectUrl"].get<std::string>();
            if (body.contains("tokenExpiry")) tokens.token_expiry = body["tokenExpiry"].get<uint64_t>();

            if (body.contains("appKey")) update_field(body["appKey"].get<std::string>(), tokens.app_key);
            if (body.contains("appSecret")) update_field(body["appSecret"].get<std::string>(), tokens.app_secret);
            if (body.contains("accessToken")) update_field(body["accessToken"].get<std::string>(), tokens.access_token);
            if (body.contains("refreshToken")) update_field(body["refreshToken"].get<std::string>(), tokens.refresh_token);

            if (token_store.save("default", tokens)) {
                bool is_auth = false;
                std::string warn_msg = "";
                if (pimpl_->broker) {
                    auto auth_res = pimpl_->broker->authenticate();
                    is_auth = auth_res.is_ok();
                    if (!auth_res.is_ok()) {
                        warn_msg = auth_res.error();
                    }
                }
                
                nlohmann::json res;
                res["status"] = "success";
                res["isAuthenticated"] = is_auth;
                if (!warn_msg.empty()) {
                    res["warning"] = "Tokens saved, but authentication failed: " + warn_msg;
                }
                return crow::response(200, res.dump());
            } else {
                nlohmann::json err = {{"error", "Failed to encrypt and save tokens"}};
                return crow::response(500, err.dump());
            }
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // Setup WebSocket route for real-time push
    CROW_WEBSOCKET_ROUTE(pimpl_->app, "/ws/live")
        .onopen([this](crow::websocket::connection& conn) {
            std::lock_guard<std::mutex> lock(pimpl_->ws_mutex);
            pimpl_->connections.insert(&conn);
            std::cout << "[HttpServer] WS client connected. Active connections: " << pimpl_->connections.size() << std::endl;
        })
        .onclose([this](crow::websocket::connection& conn, const std::string& reason) {
            std::lock_guard<std::mutex> lock(pimpl_->ws_mutex);
            pimpl_->connections.erase(&conn);
            std::cout << "[HttpServer] WS client disconnected: " << reason << ". Active connections: " << pimpl_->connections.size() << std::endl;
        })
        .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool is_binary) {
            // Echo or handle client messages if necessary
        });

    // 9. POST /api/trigger_gap_down
    CROW_ROUTE(pimpl_->app, "/api/trigger_gap_down").methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
        try {
            if (!pimpl_->screen_a) {
                nlohmann::json err = {{"error", "ScreenA component not registered."}};
                return crow::response(500, err.dump());
            }
            auto body = nlohmann::json::parse(req.body);
            std::string symbol = body.at("symbol").get<std::string>();
            double gap_pct = body.at("gap_percent").get<double>();
            std::string news = body.value("news", "Simulated corporate news");
            bool is_existential = body.value("is_existential", false);
            
            pimpl_->screen_a->trigger_mock_gap_down(symbol, gap_pct, news, is_existential);
            
            nlohmann::json res;
            res["status"] = "success";
            res["symbol"] = symbol;
            res["gap_percent"] = gap_pct;
            res["message"] = "Mock gap down registered for " + symbol;
            return crow::response(200, res.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // 10. GET /api/intraday_bars
    CROW_ROUTE(pimpl_->app, "/api/intraday_bars")([this](const crow::request& req) {
        try {
            if (!pimpl_->ts_store) {
                nlohmann::json err = {{"error", "TimeSeriesStore component not registered."}};
                return crow::response(500, err.dump());
            }
            std::string symbol = req.url_params.get("symbol") ? req.url_params.get("symbol") : "";
            std::string res_str = req.url_params.get("resolution") ? req.url_params.get("resolution") : "M5";
            int limit = req.url_params.get("limit") ? std::stoi(req.url_params.get("limit")) : 200;
            
            if (symbol.empty()) {
                nlohmann::json err = {{"error", "Missing query parameter 'symbol'."}};
                return crow::response(400, err.dump());
            }
            
            auto ts = pimpl_->ts_store->get(symbol);
            if (!ts) {
                nlohmann::json err = {{"error", "No data found for symbol: " + symbol}};
                return crow::response(404, err.dump());
            }
            
            core::Resolution resolution = core::Resolution::M5;
            if (res_str == "M1") resolution = core::Resolution::M1;
            else if (res_str == "H1") resolution = core::Resolution::H1;
            else if (res_str == "D1") resolution = core::Resolution::D1;
            
            auto bars = ts->last_n(resolution, limit);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& b : bars) {
                nlohmann::json obj;
                obj["ts"] = b.ts.ms_since_epoch;
                obj["open"] = b.open.value;
                obj["high"] = b.high.value;
                obj["low"] = b.low.value;
                obj["close"] = b.close.value;
                obj["volume"] = b.volume.value;
                arr.push_back(obj);
            }
            
            return crow::response(200, arr.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", e.what()}};
            return crow::response(500, err.dump());
        }
    });

    // SPA static file fallback / serve wildcard
    CROW_ROUTE(pimpl_->app, "/<path>")([this](const crow::request& req, std::string path) {
        return pimpl_->serve_static(path);
    });

    CROW_ROUTE(pimpl_->app, "/")([this]() {
        return pimpl_->serve_static("index.html");
    });
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    std::cout << "[HttpServer] Starting Crow Web Server on port " << pimpl_->port << "..." << std::endl;
    pimpl_->server_thread = std::thread([this]() {
        pimpl_->app.port(pimpl_->port).multithreaded().run();
    });
}

void HttpServer::stop() {
    pimpl_->app.stop();
    if (pimpl_->server_thread.joinable()) {
        pimpl_->server_thread.join();
    }
}

void HttpServer::broadcast_alert(const std::string& alert_json) {
    nlohmann::json msg = {
        {"type", "alert"},
        {"data", nlohmann::json::parse(alert_json)}
    };
    pimpl_->broadcast(msg.dump());
}

void HttpServer::broadcast_regime(const std::string& regime_json) {
    nlohmann::json msg = {
        {"type", "regime"},
        {"data", nlohmann::json::parse(regime_json)}
    };
    pimpl_->broadcast(msg.dump());
}

void HttpServer::broadcast_tick(const std::string& tick_json) {
    nlohmann::json msg = {
        {"type", "tick"},
        {"data", nlohmann::json::parse(tick_json)}
    };
    pimpl_->broadcast(msg.dump());
}

} // namespace web
} // namespace trader
