#pragma once

#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>
#include "trader/persistence/sqlite_store.hpp"
#include "trader/broker/broker_adapter.hpp"

namespace trader {
namespace core {

class TelegramBot {
public:
    TelegramBot(
        std::shared_ptr<persistence::SQLiteStore> store,
        std::shared_ptr<broker::BrokerAdapter> broker,
        const std::string& token = ""
    );
    ~TelegramBot();

    // Disable copy
    TelegramBot(const TelegramBot&) = delete;
    TelegramBot& operator=(const TelegramBot&) = delete;

    void start();
    void stop();

    // Outbound messages
    bool send_message(const std::string& chat_id, const std::string& text, const std::string& reply_markup_json = "");
    bool edit_message_text(const std::string& chat_id, int64_t message_id, const std::string& text, const std::string& reply_markup_json = "");
    bool edit_message_reply_markup(const std::string& chat_id, int64_t message_id, const std::string& reply_markup_json);
    bool answer_callback_query(const std::string& callback_query_id, const std::string& text = "");

    // Configuration / Chat ID lookup
    std::string get_chat_id(const std::string& tier) const;
    void reload_chat_ids();

private:
    void poll_loop();
    void handle_update(const nlohmann::json& update);
    void handle_callback_query(const nlohmann::json& callback);
    void handle_text_command(const std::string& chat_id, const std::string& text, int64_t message_id);

    std::shared_ptr<persistence::SQLiteStore> store_;
    std::shared_ptr<broker::BrokerAdapter> broker_;
    std::string bot_token_;
    
    std::string chat_premium_;
    std::string chat_opportunity_;
    std::string chat_digest_;

    std::thread poll_thread_;
    std::atomic<bool> running_{false};
    int64_t last_update_id_{0};
};

} // namespace core
} // namespace trader
