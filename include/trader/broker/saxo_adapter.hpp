#pragma once

#include "trader/broker/broker_adapter.hpp"
#include <string>
#include <memory>

namespace trader {
namespace broker {

struct SaxoBrokerConfig {
    std::string user_id = "default";
    std::string token_db_path = "./data/tokens.db";
    std::string token_encryption_key;
};

class SaxoBrokerAdapter : public BrokerAdapter {
public:
    explicit SaxoBrokerAdapter(SaxoBrokerConfig config = {});
    ~SaxoBrokerAdapter() override;

    // BrokerAdapter overrides
    Result<void> authenticate() override;
    bool is_authenticated() const override;

    Result<InstrumentInfo> lookup_symbol(const std::string& sym) override;
    Result<std::vector<InstrumentInfo>> search(const std::string& query) override;

    Result<std::vector<Bar>> get_bars(
        InstrumentId id, Resolution r, Timestamp from, Timestamp to) override;

    Result<SubscriptionId> subscribe_quotes(
        InstrumentId id, std::function<void(const Tick&)> on_tick) override;
    Result<void> unsubscribe(SubscriptionId id) override;

    Result<AccountInfo> get_account() override;
    Result<OrderId> place_order(const OrderRequest& req) override;
    Result<void> cancel_order(OrderId id) override;

private:
    // PIMPL idiom to hide cpp-httplib and websocket dependencies from the header
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace broker
} // namespace trader
