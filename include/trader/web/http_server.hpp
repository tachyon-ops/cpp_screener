#pragma once

#include <memory>
#include <string>
#include "trader/persistence/sqlite_store.hpp"
#include "trader/broker/broker_adapter.hpp"

namespace trader {

namespace core {
    class RegimeClassifier;
}

namespace storage {
    class TimeSeriesStore;
}

namespace screens {
    class ScreenD;
    class ScreenB;
    class ScreenA;
    class ScreenE;
    class ScreenF;
}

namespace web {

class HttpServer {
public:
    HttpServer(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<broker::BrokerAdapter> broker,
        std::shared_ptr<core::RegimeClassifier> classifier,
        std::shared_ptr<screens::ScreenD> screen_d,
        std::shared_ptr<screens::ScreenB> screen_b,
        std::shared_ptr<screens::ScreenA> screen_a,
        std::shared_ptr<screens::ScreenE> screen_e,
        std::shared_ptr<screens::ScreenF> screen_f,
        std::shared_ptr<storage::TimeSeriesStore> ts_store,
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
