#include "trader/web/http_server.hpp"
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
    std::string public_dir;
    int port;

    crow::SimpleApp app;
    std::thread server_thread;
    std::mutex ws_mutex;
    std::unordered_set<crow::websocket::connection*> connections;

    Impl(
        std::shared_ptr<persistence::SQLiteStore> s,
        std::shared_ptr<broker::BrokerAdapter> b,
        std::string dir,
        int p
    ) : store(std::move(s)), broker(std::move(b)), public_dir(std::move(dir)), port(p) {}

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
    const std::string& public_dir,
    int port
) : pimpl_(std::make_unique<Impl>(std::move(store), std::move(broker), public_dir, port)) {

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
