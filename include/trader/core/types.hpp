#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <variant>

// Minimal core types for placeholders
namespace trader {
namespace core {

struct Timestamp {
    uint64_t ms_since_epoch;
};

struct Price { double value; };
struct Quantity { double value; };

struct InstrumentId {
    std::string broker;
    std::string native_id;
    std::string asset_type;
};

struct InstrumentInfo {
    InstrumentId id;
    std::string symbol;
    double tick_size;
};

struct Bar {
    Timestamp ts;
    Price open;
    Price high;
    Price low;
    Price close;
    Quantity volume;
};

struct Tick {
    Timestamp ts;
    Price bid;
    Price ask;
};

enum class Resolution {
    M1, M5, M15, M30, H1, H4, D1, W1
};

struct AccountInfo {
    std::string account_id;
    std::string currency;
    double equity;
    double cash;
};

struct OrderRequest {
    InstrumentId instrument;
    std::string side; // "buy" or "sell"
    std::string type; // "market", "limit", etc.
    Quantity size;
    std::optional<Price> limit_price;
    std::optional<Price> stop_price;
};

using SubscriptionId = std::string;
using OrderId = std::string;

// A simple Result type stub
template <typename T, typename E = std::string>
struct Result {
    std::variant<T, E> data;
    bool is_ok() const { return data.index() == 0; }
    T value() const { return std::get<0>(data); }
    E error() const { return std::get<1>(data); }
    
    static Result ok(T v) { Result r; r.data = std::variant<T, E>(std::in_place_index<0>, std::move(v)); return r; }
    static Result err(E e) { Result r; r.data = std::variant<T, E>(std::in_place_index<1>, std::move(e)); return r; }
};

// Void specialization
template <typename E>
struct Result<void, E> {
    std::optional<E> _error;
    bool is_ok() const { return !_error.has_value(); }
    E error() const { return *_error; }

    static Result ok() { return Result{}; }
    static Result err(E e) { Result r; r._error = std::move(e); return r; }
};

} // namespace core
} // namespace trader
