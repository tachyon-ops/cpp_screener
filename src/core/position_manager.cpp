#include "trader/core/position_manager.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/core/sr_provider.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <iomanip>

namespace trader {
namespace core {

namespace {
std::string get_current_utc_time_str() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}
}

PositionManager::PositionManager(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<storage::TimeSeriesStore> ts_store,
    std::shared_ptr<core::ISupportResistanceProvider> sr_provider,
    std::shared_ptr<core::AlertDispatcher> dispatcher
) : store_(store), ts_store_(ts_store), sr_provider_(sr_provider), dispatcher_(dispatcher) {}

PositionManager::~PositionManager() {
    stop();
}

void PositionManager::start() {
    if (running_.exchange(true)) return;
    worker_thread_ = std::thread([this]() {
        std::cout << "[PositionManager] Background thread started." << std::endl;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            try {
                check_positions();
            } catch (const std::exception& e) {
                std::cerr << "[PositionManager] Error in worker thread: " << e.what() << std::endl;
            }
        }
    });
}

void PositionManager::stop() {
    if (!running_.exchange(false)) return;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    std::cout << "[PositionManager] Background thread stopped." << std::endl;
}

void PositionManager::check_positions() {
    if (!store_ || !ts_store_) return;

    auto all_positions = store_->get_positions();
    std::vector<persistence::DbPosition> open_positions;
    for (const auto& pos : all_positions) {
        if (pos.status == "open" || pos.status.empty()) {
            open_positions.push_back(pos);
        }
    }

    if (open_positions.empty()) return;

    // Load instruments map
    auto instruments = store_->get_instruments();
    std::unordered_map<int64_t, std::string> inst_map;
    for (const auto& inst : instruments) {
        inst_map[inst.id] = inst.symbol;
    }

    for (auto& pos : open_positions) {
        auto it = inst_map.find(pos.instrument_id);
        if (it == inst_map.end()) {
            std::cerr << "[PositionManager] Warning: Instrument ID " << pos.instrument_id << " not found for position ID " << pos.id << std::endl;
            continue;
        }
        std::string symbol = it->second;

        auto ts = ts_store_->get(symbol);
        if (!ts) continue;

        double latest_price = ts->get_latest_price();
        if (latest_price <= 0.0) continue;

        double entry = pos.entry_price;
        double init_stop = pos.initial_stop;
        double curr_stop = pos.current_stop;
        double r_size = (pos.direction == "short") ? (init_stop - entry) : (entry - init_stop);

        if (r_size <= 0.0) continue;

        bool updated = false;

        // 1. Check Breakeven Migration (+0.3R trigger)
        if (pos.direction == "long" || pos.direction.empty()) {
            if (latest_price >= entry + 0.3 * r_size && curr_stop < entry) {
                pos.current_stop = entry;
                pos.notes += " [Stop migrated to BE at +0.3R]";
                updated = true;
                std::cout << "[PositionManager] Migrated long position for " << symbol << " to BE ($" << entry << ") at price $" << latest_price << std::endl;
                if (dispatcher_) {
                    dispatcher_->dispatch_telegram_message("Position stop for " + symbol + " migrated to BE ($" + std::to_string(entry) + ") as price reached +0.3R ($" + std::to_string(latest_price) + ")");
                }
            }
        } else if (pos.direction == "short") {
            if (latest_price <= entry - 0.3 * r_size && curr_stop > entry) {
                pos.current_stop = entry;
                pos.notes += " [Stop migrated to BE at +0.3R]";
                updated = true;
                std::cout << "[PositionManager] Migrated short position for " << symbol << " to BE ($" << entry << ") at price $" << latest_price << std::endl;
                if (dispatcher_) {
                    dispatcher_->dispatch_telegram_message("Position stop for short " + symbol + " migrated to BE ($" + std::to_string(entry) + ") as price reached +0.3R ($" + std::to_string(latest_price) + ")");
                }
            }
        }

        // 2. Check Structural Trailing Stop (Nearest support below, for long position only)
        if (sr_provider_ && (pos.direction == "long" || pos.direction.empty())) {
            auto levels = sr_provider_->get_levels(symbol);
            double highest_support = pos.current_stop;
            for (const auto& lvl : levels) {
                double stop_candidate = lvl.price * 0.995; // Stop is placed 0.5% below support
                if (stop_candidate < latest_price && stop_candidate > highest_support) {
                    highest_support = stop_candidate;
                }
            }
            if (highest_support > pos.current_stop) {
                pos.current_stop = highest_support;
                pos.notes += " [Stop trailed to " + std::to_string(highest_support) + " below support]";
                updated = true;
                std::cout << "[PositionManager] Trailed stop for " << symbol << " to $" << highest_support << std::endl;
                if (dispatcher_) {
                    dispatcher_->dispatch_telegram_message("Position stop for " + symbol + " trailed to $" + std::to_string(highest_support) + " below support level");
                }
            }
        }

        // 3. Check Stop Breach (Exit)
        bool stopped_out = false;
        if (pos.direction == "long" || pos.direction.empty()) {
            if (latest_price <= pos.current_stop) {
                stopped_out = true;
            }
        } else if (pos.direction == "short") {
            if (latest_price >= pos.current_stop) {
                stopped_out = true;
            }
        }

        if (stopped_out) {
            pos.exit_price = latest_price;
            pos.exit_ts = get_current_utc_time_str();
            pos.exit_reason = "trail_stop_hit";
            
            double R = 0.0;
            if (pos.direction == "long" || pos.direction.empty()) {
                R = (pos.exit_price - entry) / r_size;
            } else {
                R = (entry - pos.exit_price) / r_size;
            }
            pos.r_realized = R;
            
            // Determine status based on actual realized R
            pos.status = (R == 0.0) ? "closed_be" : (R > 0.0) ? "closed_winner" : "closed_loser";
            
            pos.notes += " [Stopped out at " + std::to_string(latest_price) + " R=" + std::to_string(R).substr(0, 4) + "]";
            
            store_->update_position(pos);
            std::cout << "[PositionManager] Exited position for " << symbol << " at $" << latest_price 
                      << " due to stop breach. Realized R: " << R << std::endl;
            if (dispatcher_) {
                dispatcher_->dispatch_telegram_message("Position EXITED for " + symbol + " at $" + std::to_string(latest_price) + " (Stop breached). Realized R-multiple: " + std::to_string(R).substr(0, 4));
            }
        } else if (updated) {
            store_->update_position(pos);
        }
    }
}

} // namespace core
} // namespace trader
