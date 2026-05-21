#pragma once

#include <memory>
#include <string>
#include "trader/persistence/sqlite_store.hpp"
#include "trader/broker/broker_adapter.hpp"

namespace trader {

namespace core {
    class RegimeClassifier;
}

namespace screens {
    class ScreenD;
}

namespace web {

class HttpServer {
public:
    HttpServer(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<broker::BrokerAdapter> broker,
        std::shared_ptr<core::RegimeClassifier> classifier,
        std::shared_ptr<screens::ScreenD> screen_d,
        const std::string& public_dir = "./ui/dist",
        int port = 8080
    );
    ~HttpServer();

    // Disable copy/move
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();
    void stop();

    // Methods to broadcast updates to WebSocket clients
    void broadcast_alert(const std::string& alert_json);
    void broadcast_regime(const std::string& regime_json);
    void broadcast_tick(const std::string& tick_json);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace web
} // namespace trader
