#include "trader/core/telegram.hpp"
#include <httplib.h>
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace trader {
namespace core {

namespace {
// Private helper to post JSON payload to Telegram API
nlohmann::json post_telegram(const std::string& method, const nlohmann::json& body, const std::string& token) {
    httplib::Client cli("https://api.telegram.org");
    cli.set_connection_timeout(std::chrono::seconds(10));
    cli.set_read_timeout(std::chrono::seconds(45)); // long-poll friendly timeout
    
    std::string path = "/bot" + token + "/" + method;
    httplib::Headers headers = {
        {"Content-Type", "application/json"}
    };
    
    auto res = cli.Post(path.c_str(), headers, body.dump(), "application/json");
    if (!res || res->status != 200) {
        std::cerr << "[Telegram] API request '" << method << "' failed." << std::endl;
        if (res) {
            std::cerr << "Status: " << res->status << ", Body: " << res->body << std::endl;
            try {
                return nlohmann::json::parse(res->body);
            } catch (...) {}
        }
        return nlohmann::json();
    }
    try {
        return nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        std::cerr << "[Telegram] Failed to parse response: " << e.what() << std::endl;
        return nlohmann::json();
    }
}
} // namespace

TelegramBot::TelegramBot(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<broker::BrokerAdapter> broker,
    const std::string& token
) : store_(store), broker_(broker), bot_token_(token) {
    
    // Load token from environment if empty
    if (bot_token_.empty()) {
        const char* env_token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (env_token) {
            bot_token_ = env_token;
        }
    }
    
    reload_chat_ids();
}

TelegramBot::~TelegramBot() {
    stop();
}

void TelegramBot::reload_chat_ids() {
    // 1. Check settings table in database
    auto get_db_setting = [this](const std::string& key, const std::string& env_fallback_name) {
        auto val = store_->get_setting(key);
        if (val && !val->empty()) {
            return *val;
        }
        const char* env_val = std::getenv(env_fallback_name.c_str());
        return env_val ? std::string(env_val) : std::string("");
    };

    std::string old_token = bot_token_;
    bot_token_ = get_db_setting("tg_bot_token", "TELEGRAM_BOT_TOKEN");
    if (bot_token_ != old_token) {
        std::cout << "[TelegramBot] Bot token updated: " 
                  << (bot_token_.empty() ? "[EMPTY]" : (bot_token_.substr(0, std::min<size_t>(4, bot_token_.size())) + "..." + bot_token_.substr(bot_token_.size() > 4 ? bot_token_.size() - 4 : 0))) 
                  << std::endl;
    }

    chat_premium_ = get_db_setting("tg_chat_premium", "TG_CHAT_PREMIUM");
    chat_opportunity_ = get_db_setting("tg_chat_opportunity", "TG_CHAT_OPPORTUNITY");
    chat_digest_ = get_db_setting("tg_chat_digest", "TG_CHAT_DIGEST");
    
    std::cout << "[TelegramBot] Loaded chat configurations:" << std::endl;
    std::cout << "  Premium Chat ID:     " << (chat_premium_.empty() ? "[EMPTY]" : chat_premium_) << std::endl;
    std::cout << "  Opportunity Chat ID: " << (chat_opportunity_.empty() ? "[EMPTY]" : chat_opportunity_) << std::endl;
    std::cout << "  Digest Chat ID:      " << (chat_digest_.empty() ? "[EMPTY]" : chat_digest_) << std::endl;
}

std::string TelegramBot::get_chat_id(const std::string& tier) const {
    if (tier == "premium") return chat_premium_;
    if (tier == "opportunity") return chat_opportunity_;
    return chat_digest_; // Fallback to digest/interesting
}

void TelegramBot::start() {
    if (running_) return;
    
    running_ = true;
    poll_thread_ = std::thread(&TelegramBot::poll_loop, this);
}

void TelegramBot::stop() {
    if (!running_) return;
    
    running_ = false;
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

bool TelegramBot::send_message(const std::string& chat_id, const std::string& text, const std::string& reply_markup_json) {
    if (chat_id.empty() || bot_token_.empty()) return false;
    nlohmann::json body = {
        {"chat_id", chat_id},
        {"text", text},
        {"parse_mode", "HTML"}
    };
    if (!reply_markup_json.empty()) {
        try {
            body["reply_markup"] = nlohmann::json::parse(reply_markup_json);
        } catch (...) {
            std::cerr << "[TelegramBot] Failed to parse reply_markup_json." << std::endl;
        }
    }
    auto res = post_telegram("sendMessage", body, bot_token_);
    return res.value("ok", false);
}

bool TelegramBot::edit_message_text(const std::string& chat_id, int64_t message_id, const std::string& text, const std::string& reply_markup_json) {
    if (chat_id.empty() || bot_token_.empty()) return false;
    nlohmann::json body = {
        {"chat_id", chat_id},
        {"message_id", message_id},
        {"text", text},
        {"parse_mode", "HTML"}
    };
    if (!reply_markup_json.empty()) {
        try {
            body["reply_markup"] = nlohmann::json::parse(reply_markup_json);
        } catch (...) {
            body["reply_markup"] = nlohmann::json::object();
        }
    } else {
        body["reply_markup"] = nlohmann::json::object();
    }
    auto res = post_telegram("editMessageText", body, bot_token_);
    return res.value("ok", false);
}

bool TelegramBot::edit_message_reply_markup(const std::string& chat_id, int64_t message_id, const std::string& reply_markup_json) {
    if (chat_id.empty() || bot_token_.empty()) return false;
    nlohmann::json body = {
        {"chat_id", chat_id},
        {"message_id", message_id}
    };
    if (!reply_markup_json.empty()) {
        try {
            body["reply_markup"] = nlohmann::json::parse(reply_markup_json);
        } catch (...) {
            body["reply_markup"] = nlohmann::json::object();
        }
    } else {
        body["reply_markup"] = nlohmann::json::object();
    }
    auto res = post_telegram("editMessageReplyMarkup", body, bot_token_);
    return res.value("ok", false);
}

bool TelegramBot::answer_callback_query(const std::string& callback_query_id, const std::string& text) {
    if (bot_token_.empty()) return false;
    nlohmann::json body = {
        {"callback_query_id", callback_query_id}
    };
    if (!text.empty()) {
        body["text"] = text;
    }
    auto res = post_telegram("answerCallbackQuery", body, bot_token_);
    return res.value("ok", false);
}

void TelegramBot::poll_loop() {
    std::cout << "[TelegramBot] Polling thread spawned." << std::endl;
    auto last_reload = std::chrono::steady_clock::now();
    bool first_loop = true;
    while (running_) {
        try {
            auto now = std::chrono::steady_clock::now();
            if (first_loop || std::chrono::duration_cast<std::chrono::seconds>(now - last_reload).count() >= 5) {
                reload_chat_ids();
                last_reload = now;
                first_loop = false;
            }

            if (bot_token_.empty()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            nlohmann::json body = {
                {"timeout", 30},
                {"allowed_updates", {"message", "callback_query"}}
            };
            if (last_update_id_ > 0) {
                body["offset"] = last_update_id_ + 1;
            }
            
            auto res = post_telegram("getUpdates", body, bot_token_);
            if (res.value("ok", false) && res.contains("result") && res["result"].is_array()) {
                for (const auto& update : res["result"]) {
                    last_update_id_ = update["update_id"].get<int64_t>();
                    handle_update(update);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[TelegramBot] Exception in polling loop: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void TelegramBot::handle_update(const nlohmann::json& update) {
    if (update.contains("message")) {
        auto msg = update["message"];
        std::string chat_id = std::to_string(msg["chat"]["id"].get<int64_t>());
        int64_t message_id = msg["message_id"].get<int64_t>();
        std::string text = msg.value("text", "");
        
        std::cout << "[TelegramBot] Received message: \"" << text << "\" from Chat ID: " << chat_id << std::endl;
        
        if (!text.empty() && text[0] == '/') {
            handle_text_command(chat_id, text, message_id);
        }
    } else if (update.contains("callback_query")) {
        auto callback = update["callback_query"];
        std::string callback_id = callback.value("id", "");
        std::string data = callback.value("data", "");
        std::cout << "[TelegramBot] Received callback: \"" << data << "\" from Callback ID: " << callback_id << std::endl;
        handle_callback_query(callback);
    }
}

void TelegramBot::handle_text_command(const std::string& chat_id, const std::string& text, int64_t message_id) {
    if (text == "/set_premium") {
        store_->set_setting("tg_chat_premium", chat_id);
        reload_chat_ids();
        send_message(chat_id, "<b>[System]</b> This chat has been successfully registered for 🟢 <b>PREMIUM</b> alerts.");
    } else if (text == "/set_opportunity") {
        store_->set_setting("tg_chat_opportunity", chat_id);
        reload_chat_ids();
        send_message(chat_id, "<b>[System]</b> This chat has been successfully registered for 🟡 <b>OPPORTUNITY</b> alerts.");
    } else if (text == "/set_digest") {
        store_->set_setting("tg_chat_digest", chat_id);
        reload_chat_ids();
        send_message(chat_id, "<b>[System]</b> This chat has been successfully registered for ⚪ <b>DIGEST/INTERESTING</b> alerts.");
    } else if (text == "/unset_premium") {
        store_->set_setting("tg_chat_premium", "");
        reload_chat_ids();
        send_message(chat_id, "<b>[System]</b> 🟢 <b>PREMIUM</b> alerts have been unbound from this chat.");
    } else if (text == "/unset_opportunity") {
        store_->set_setting("tg_chat_opportunity", "");
        reload_chat_ids();
        send_message(chat_id, "<b>[System]</b> 🟡 <b>OPPORTUNITY</b> alerts have been unbound from this chat.");
    } else if (text == "/unset_digest") {
        store_->set_setting("tg_chat_digest", "");
        reload_chat_ids();
        send_message(chat_id, "<b>[System]</b> ⚪ <b>DIGEST</b> alerts have been unbound from this chat.");
    } else if (text == "/stop" || text == "/unsubscribe") {
        store_->set_setting("tg_chat_premium", "");
        store_->set_setting("tg_chat_opportunity", "");
        store_->set_setting("tg_chat_digest", "");
        store_->set_setting("telegram_enabled", "false");
        reload_chat_ids();
        send_message(chat_id, "❌ <b>Telegram Notifications Disabled</b>\n\nAll alert bindings have been removed and notifications are turned off.\nSend /start to re-enable.");
    } else if (text == "/regime") {
        auto logs = store_->get_regime_log(1);
        if (logs.empty()) {
            send_message(chat_id, "📊 No regime data available yet.");
        } else {
            auto& r = logs[0];
            std::stringstream ss;
            ss << "📊 <b>Current Market Regime</b>\n\n"
               << "Regime: <b>" << r.regime << "</b>\n"
               << "VIX: " << std::fixed << std::setprecision(2) << r.vix << "\n"
               << "Breadth: " << std::setprecision(3) << r.breadth << "\n"
               << "HY OAS: " << std::setprecision(2) << r.hy_oas << "\n"
               << "SPX vs 200MA: " << std::setprecision(2) << r.spx_vs_200ma << "\n"
               << "As of: <code>" << r.ts << "</code>";
            send_message(chat_id, ss.str());
        }
    } else if (text == "/candidates") {
        auto candidates = store_->get_candidates();
        std::vector<persistence::DbCandidate> active;
        for (const auto& c : candidates) {
            if (c.status == "active") active.push_back(c);
        }
        if (active.empty()) {
            send_message(chat_id, "🎯 No active swing setups in watchlist.");
        } else {
            std::stringstream ss;
            ss << "🎯 <b>Active Swing Watchlist (" << active.size() << ")</b>\n";
            for (const auto& c : active) {
                // Look up symbol by instrument_id
                std::string sym = "ID:" + std::to_string(c.instrument_id);
                auto instruments = store_->get_instruments();
                for (const auto& i : instruments) {
                    if (i.id == c.instrument_id) { sym = i.symbol; break; }
                }
                ss << "\n• <b>" << sym << "</b> (Screen " << c.screen << ")\n"
                   << "  Entry: $" << std::fixed << std::setprecision(2) << c.entry_zone_low << " – $" << c.entry_zone_high << "\n"
                   << "  Stop: $" << c.suggested_stop << " | R:R: " << std::setprecision(1) << c.rr_target << "\n";
                if (!c.notes.empty()) {
                    ss << "  Notes: " << c.notes << "\n";
                }
            }
            send_message(chat_id, ss.str());
        }
    } else if (text == "/alerts") {
        auto alerts = store_->get_alerts(5);
        if (alerts.empty()) {
            send_message(chat_id, "🚨 No screener alerts recorded.");
        } else {
            std::stringstream ss;
            ss << "🚨 <b>Recent Screener Alerts (Top 5)</b>\n";
            for (const auto& a : alerts) {
                std::string sym = "Unknown";
                try {
                    auto payload = nlohmann::json::parse(a.payload_json);
                    sym = payload.value("symbol", "Unknown");
                } catch (...) {}
                ss << "\n• <b>ID " << a.id << "</b> — " << sym << " (<code>" << a.ts << "</code>)\n"
                   << "  Screen " << a.screen << " | Regime: " << a.regime_at_alert
                   << " | Acted: " << (a.acted_on ? "Yes" : "No") << "\n";
            }
            send_message(chat_id, ss.str());
        }
    } else if (text == "/status") {
        std::stringstream ss;
        ss << "<b>[Tachyon Status]</b>\n"
           << "Premium Chat ID: <code>" << chat_premium_ << "</code>\n"
           << "Opportunity Chat ID: <code>" << chat_opportunity_ << "</code>\n"
           << "Digest Chat ID: <code>" << chat_digest_ << "</code>\n"
           << "Active Positions: " << store_->get_positions().size() << "\n"
           << "Active Candidates: " << store_->get_candidates().size();
        send_message(chat_id, ss.str());
    } else if (text == "/start") {
        // Auto-register this chat for all three tiers on /start
        store_->set_setting("tg_chat_premium", chat_id);
        store_->set_setting("tg_chat_opportunity", chat_id);
        store_->set_setting("tg_chat_digest", chat_id);
        store_->set_setting("telegram_enabled", "true");
        reload_chat_ids();

        std::stringstream ss;
        ss << "⚡ <b>Tachyon Trading Screener</b> ⚡\n\n"
           << "✅ <b>Registration complete!</b>\n"
           << "This chat is now bound to all alert tiers:\n"
           << "  🟢 Premium  •  🟡 Opportunity  •  ⚪ Digest\n\n"
           << "<b>Available Commands:</b>\n"
           << "<code>/regime</code> — Current market regime\n"
           << "<code>/candidates</code> — List active swing setups\n"
           << "<code>/alerts</code> — List recent signals\n"
           << "<code>/set_premium</code> — Bind Premium alerts to a chat\n"
           << "<code>/set_opportunity</code> — Bind Opportunity alerts\n"
           << "<code>/set_digest</code> — Bind Digest alerts\n"
           << "<code>/unset_premium</code> — Remove Premium binding\n"
           << "<code>/unset_opportunity</code> — Remove Opportunity binding\n"
           << "<code>/unset_digest</code> — Remove Digest binding\n"
           << "<code>/stop</code> — Unsubscribe from all alerts\n"
           << "<code>/status</code> — Show engine status\n"
           << "<code>/help</code> — Show this help menu";
        send_message(chat_id, ss.str());
    } else if (text == "/help") {
        std::stringstream ss;
        ss << "🤖 <b>Tachyon Screener Bot Commands</b> 🤖\n\n"
           << "<b>Data Queries:</b>\n"
           << "<code>/regime</code> — Current market regime\n"
           << "<code>/candidates</code> — List active swing setups\n"
           << "<code>/alerts</code> — List recent signals\n\n"
           << "<b>Subscriptions:</b>\n"
           << "<code>/set_premium</code> — Bind Premium alerts\n"
           << "<code>/set_opportunity</code> — Bind Opportunity alerts\n"
           << "<code>/set_digest</code> — Bind Digest alerts\n"
           << "<code>/unset_premium</code> — Remove Premium binding\n"
           << "<code>/unset_opportunity</code> — Remove Opportunity binding\n"
           << "<code>/unset_digest</code> — Remove Digest binding\n"
           << "<code>/stop</code> — Unsubscribe from all alerts\n\n"
           << "<b>System:</b>\n"
           << "<code>/status</code> — Show engine status & stats\n"
           << "<code>/help</code> — Show this help menu";
        send_message(chat_id, ss.str());
    }
}


void TelegramBot::handle_callback_query(const nlohmann::json& callback) {
    std::string callback_id = callback["id"];
    std::string data = callback["data"];
    
    nlohmann::json message = callback["message"];
    std::string chat_id = std::to_string(message["chat"]["id"].get<int64_t>());
    int64_t message_id = message["message_id"].get<int64_t>();
    std::string original_text = message.value("text", "");
    
    // Parse callback data: <action>:<alert_id> or <action>:<extra>:<alert_id>
    size_t colon_pos = data.find(':');
    if (colon_pos == std::string::npos) {
        answer_callback_query(callback_id, "Invalid action");
        return;
    }
    
    std::string action = data.substr(0, colon_pos);
    std::string rest = data.substr(colon_pos + 1);
    
    int64_t alert_id = 0;
    std::string extra_reason = "";
    
    size_t second_colon = rest.find(':');
    if (second_colon != std::string::npos) {
        extra_reason = rest.substr(0, second_colon);
        alert_id = std::stoll(rest.substr(second_colon + 1));
    } else {
        alert_id = std::stoll(rest);
    }
    
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    std::string ts = ss.str();
    
    // Log response
    persistence::DbAlertResponse resp;
    resp.alert_id = alert_id;
    resp.response_ts = ts;
    resp.response_type = action;
    
    std::string feedback_msg = "";
    bool edit_needed = true;
    std::string new_markup = "";
    
    if (action == "saw_it") {
        resp.response_type = "seen";
        store_->add_alert_response(resp);
        store_->update_alert_acted(alert_id, 1);
        feedback_msg = "Acknowledged";
        answer_callback_query(callback_id, "Marked as Seen");
        new_markup = "";
    } 
    else if (action == "acting") {
        resp.response_type = "acted";
        store_->add_alert_response(resp);
        store_->update_alert_acted(alert_id, 1);
        feedback_msg = "Executing trade...";
        answer_callback_query(callback_id, "Taking Trade");
        
        // Execute simulated trade directly
        auto alerts = store_->get_alerts(100);
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
            try {
                auto payload = nlohmann::json::parse(target_alert.payload_json);
                std::string sym = payload.value("symbol", "");
                double entry_price = payload.value("suggested_entry_high", 0.0);
                double stop_price = payload.value("suggested_stop", 0.0);
                
                if (!sym.empty()) {
                    auto lookup_res = broker_->lookup_symbol(sym);
                    if (lookup_res.is_ok()) {
                        auto inst_info = lookup_res.value();
                        
                        core::OrderRequest order;
                        order.instrument = inst_info.id;
                        order.side = "buy";
                        order.type = "market";
                        order.size.value = 100;
                        
                        auto order_res = broker_->place_order(order);
                        if (order_res.is_ok()) {
                            feedback_msg = "Trade Placed: " + order_res.value();
                            
                            persistence::DbPosition pos;
                            pos.alert_id = alert_id;
                            
                            auto db_inst = store_->get_instrument_by_symbol(sym);
                            int64_t inst_id = 0;
                            if (db_inst) {
                                inst_id = db_inst->id;
                            } else {
                                persistence::DbInstrument inst;
                                inst.symbol = inst_info.symbol;
                                inst.asset_class = "Stock";
                                inst.exchange = "Saxo";
                                inst.saxo_uic = std::stoll(inst_info.id.native_id);
                                inst.metadata_json = nlohmann::json({{"asset_type", inst_info.id.asset_type}}).dump();
                                store_->add_instrument(inst);
                                auto created = store_->get_instrument_by_symbol(sym);
                                if (created) {
                                    inst_id = created->id;
                                }
                            }
                            pos.instrument_id = inst_id;
                            pos.direction = "long";
                            pos.entry_ts = ts;
                            pos.entry_price = entry_price > 0 ? entry_price : (payload.value("price", 100.0));
                            pos.size = 100;
                            pos.initial_stop = stop_price;
                            pos.current_stop = stop_price;
                            pos.status = "open";
                            store_->add_position(pos);
                        } else {
                            feedback_msg = "Order error: " + order_res.error();
                        }
                    } else {
                        feedback_msg = "Symbol lookup failed";
                    }
                }
            } catch (const std::exception& ex) {
                feedback_msg = std::string("Execution failed: ") + ex.what();
            }
        }
        new_markup = "";
    } 
    else if (action == "skip") {
        nlohmann::json btn_regime = nlohmann::json::object({{"text", "Wrong Regime"}, {"callback_data", "skip_reason:Wrong Regime:" + std::to_string(alert_id)}});
        nlohmann::json btn_news   = nlohmann::json::object({{"text", "Bad News"}, {"callback_data", "skip_reason:Bad News:" + std::to_string(alert_id)}});
        nlohmann::json btn_size   = nlohmann::json::object({{"text", "Size Too Large"}, {"callback_data", "skip_reason:Size Too Large:" + std::to_string(alert_id)}});
        nlohmann::json btn_corr   = nlohmann::json::object({{"text", "Correlated Pos"}, {"callback_data", "skip_reason:Correlated Pos:" + std::to_string(alert_id)}});
        nlohmann::json btn_trust  = nlohmann::json::object({{"text", "Don't Trust"}, {"callback_data", "skip_reason:Don't Trust:" + std::to_string(alert_id)}});
        nlohmann::json btn_other  = nlohmann::json::object({{"text", "Other"}, {"callback_data", "skip_reason:Other:" + std::to_string(alert_id)}});
        nlohmann::json skip_keyboard;
        skip_keyboard["inline_keyboard"] = nlohmann::json::array({
            nlohmann::json::array({btn_regime, btn_news}),
            nlohmann::json::array({btn_size, btn_corr}),
            nlohmann::json::array({btn_trust, btn_other})
        });
        edit_message_reply_markup(chat_id, message_id, skip_keyboard.dump());
        answer_callback_query(callback_id, "Select skip reason");
        edit_needed = false;
    } 
    else if (action == "skip_reason") {
        resp.response_type = "skipped";
        resp.skip_reason = extra_reason;
        store_->add_alert_response(resp);
        store_->update_alert_acted(alert_id, 1);
        feedback_msg = "Skipped (" + extra_reason + ")";
        answer_callback_query(callback_id, "Skip logged");
        new_markup = "";
    } 
    else if (action == "note") {
        resp.response_type = "noted";
        resp.note_text = "Noted from Telegram";
        store_->add_alert_response(resp);
        feedback_msg = "Noted - Update details in Web UI";
        answer_callback_query(callback_id, "Noted successfully");
        new_markup = "";
    } 
    else if (action == "defer") {
        resp.response_type = "deferred";
        store_->add_alert_response(resp);
        feedback_msg = "Deferred (watching)";
        answer_callback_query(callback_id, "Deferred alert");
        new_markup = "";
    }
    
    if (edit_needed) {
        std::string footer = "\n\n<b>[Status: " + feedback_msg + "]</b>";
        edit_message_text(chat_id, message_id, original_text + footer, new_markup);
    }
}

} // namespace core
} // namespace trader
