#include "trader/web/http_server.hpp"
#include "trader/core/regime_classifier.hpp"
#include "trader/screens/screen_d.hpp"
#include <crow.h>
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
        std::string dir,
        int p
    ) : store(std::move(s)), broker(std::move(b)), classifier(std::move(c)), screen_d(std::move(sd)), public_dir(std::move(dir)), port(p) {}

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
    const std::string& public_dir,
    int port
) : pimpl_(std::make_unique<Impl>(std::move(store), std::move(broker), std::move(classifier), std::move(screen_d), public_dir, port)) {

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
            std::string action = body.value("action", "dismiss"); // "execute" or "dismiss"

            pimpl_->store->update_alert_acted(alert_id, 1);

            nlohmann::json res;
            res["status"] = "success";
            res["alert_id"] = alert_id;

            if (action == "execute") {
                // Fetch alert details
                auto alerts = pimpl_->store->get_alerts(100);
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
                    // Try to execute simulated trade
                    nlohmann::json payload = nlohmann::json::parse(target_alert.payload_json);
                    std::string sym = payload.value("symbol", "");
                    double price = payload.value("price", 0.0);
                    
                    if (!sym.empty()) {
                        auto inst_info_res = pimpl_->broker->lookup_symbol(sym);
                        if (inst_info_res.is_ok()) {
                            auto inst_info = inst_info_res.value();
                            
                            core::OrderRequest order;
                            order.instrument = inst_info.id;
                            order.side = "buy";
                            order.type = "market";
                            order.size.value = 100; // Default amount
                            
                            auto order_res = pimpl_->broker->place_order(order);
                            if (order_res.is_ok()) {
                                res["order_id"] = order_res.value();
                                res["message"] = "Order executed successfully via Saxo Broker: " + order_res.value();
                            } else {
                                res["message"] = "DB updated but Order failed: " + order_res.error();
                            }
                        } else {
                            res["message"] = "DB updated but Symbol lookup failed: " + inst_info_res.error();
                        }
                    }
                }
            } else {
                res["message"] = "Alert marked as dismissed.";
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
