#pragma once

#include "trader/core/types.hpp"

namespace trader {
namespace broker {

using namespace trader::core;

class BrokerAdapter {
public:
    virtual ~BrokerAdapter() = default;

    // Authentication
    virtual Result<void> authenticate() = 0;
    virtual bool is_authenticated() const = 0;

    // Reference data
    virtual Result<InstrumentInfo> lookup_symbol(const std::string& sym) = 0;
    virtual Result<std::vector<InstrumentInfo>> search(const std::string& query) = 0;

    // Historical
    virtual Result<std::vector<Bar>> get_bars(
        InstrumentId id, Resolution r, Timestamp from, Timestamp to) = 0;

    // Streaming
    virtual Result<SubscriptionId> subscribe_quotes(
        InstrumentId id, std::function<void(const Tick&)> on_tick) = 0;
    virtual Result<void> unsubscribe(SubscriptionId id) = 0;

    // Account / orders
    virtual Result<AccountInfo> get_account() = 0;
    virtual Result<OrderId> place_order(const OrderRequest& req) = 0;
    virtual Result<void> cancel_order(OrderId id) = 0;
};

} // namespace broker
} // namespace trader
