#pragma once

#include "trader/persistence/sqlite_store.hpp"
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <string>

namespace trader {
namespace storage {
    class TimeSeriesStore;
}
namespace core {
    class ISupportResistanceProvider;
    class AlertDispatcher;

class PositionManager {
public:
    PositionManager(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<storage::TimeSeriesStore> ts_store,
        std::shared_ptr<core::ISupportResistanceProvider> sr_provider,
        std::shared_ptr<core::AlertDispatcher> dispatcher = nullptr
    );
    ~PositionManager();

    // Disable copy
    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    // Start background thread checking open positions
    void start();
    
    // Stop background thread
    void stop();

    // Force run a check immediately (for testing/synchronous evaluation)
    void check_positions();

private:
    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<storage::TimeSeriesStore> ts_store_;
    std::shared_ptr<core::ISupportResistanceProvider> sr_provider_;
    std::shared_ptr<core::AlertDispatcher> dispatcher_;

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
};

} // namespace core
} // namespace trader
