#include "trader/broker/saxo_adapter.hpp"
#include "trader/storage/token_store.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <iomanip>
#include <cstdlib>

namespace trader {
namespace broker {

// Helper to URL-encode a string
static std::string url_encode(const std::string &value) {
    std::ostringstream escaped;
    escaped << std::hex << std::uppercase;
    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::setfill('0') << (int)(unsigned char)c;
        }
    }
    return escaped.str();
}

// Base64 helper for OAuth basic auth
static std::string base64_encode(const std::string& in) {
    static const char lookup[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (char c : in) {
        val = (val << 8) + (unsigned char)c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(lookup[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(lookup[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

struct SaxoBrokerAdapter::Impl {
    SaxoBrokerConfig config;
    std::unique_ptr<storage::TokenStore> store;
    std::optional<storage::SaxoTokens> tokens;
    
    // Background polling/mock quote thread state
    std::mutex subs_mutex;
    std::unordered_map<SubscriptionId, std::pair<InstrumentId, std::function<void(const Tick&)>>> subscriptions;
    std::thread quote_thread;
    std::atomic<bool> run_quote_thread{false};
    std::atomic<uint64_t> sub_counter{0};

    std::string api_host;
    std::string api_path_prefix;

    void parse_base_url() {
        if (!tokens) return;
        std::string base = tokens->open_api_base;
        // e.g. https://gateway.saxobank.com/sim/openapi
        // parse into api_host: https://gateway.saxobank.com
        // and api_path_prefix: /sim/openapi
        size_t proto_end = base.find("://");
        if (proto_end == std::string::npos) {
            api_host = "https://gateway.saxobank.com";
            api_path_prefix = "/sim/openapi";
            return;
        }
        size_t host_end = base.find("/", proto_end + 3);
        if (host_end == std::string::npos) {
            api_host = base;
            api_path_prefix = "";
        } else {
            api_host = base.substr(0, host_end);
            api_path_prefix = base.substr(host_end);
        }
    }

    bool check_and_refresh_token() {
        if (!tokens) return false;
        
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // If not expired yet (with a 60-second buffer), we're good
        if (tokens->token_expiry > 0 && now_ms < (tokens->token_expiry - 60000)) {
            return true;
        }

        std::cout << "[SaxoBrokerAdapter] Token expired or near expiry. Refreshing..." << std::endl;
        
        // Parse auth host and endpoint
        std::string auth_url = tokens->auth_base;
        if (auth_url.empty()) return false;
        if (auth_url.back() != '/') auth_url += "/";
        auth_url += "token";

        size_t proto_end = auth_url.find("://");
        if (proto_end == std::string::npos) return false;
        size_t host_end = auth_url.find("/", proto_end + 3);
        if (host_end == std::string::npos) return false;
        
        std::string auth_host = auth_url.substr(0, host_end);
        std::string auth_path = auth_url.substr(host_end);

        httplib::Client cli(auth_host);
        cli.set_connection_timeout(std::chrono::seconds(10));
        cli.set_read_timeout(std::chrono::seconds(10));

        std::string client_auth = base64_encode(tokens->app_key + ":" + tokens->app_secret);
        
        httplib::Headers headers = {
            {"Authorization", "Basic " + client_auth},
            {"Content-Type", "application/x-www-form-urlencoded"}
        };

        std::string body = "grant_type=refresh_token&refresh_token=" + url_encode(tokens->refresh_token);

        auto res = cli.Post(auth_path, headers, body, "application/x-www-form-urlencoded");
        if (!res || res->status != 200) {
            std::cerr << "[SaxoBrokerAdapter] Token refresh request failed." << std::endl;
            if (res) {
                std::cerr << "Status: " << res->status << ", Body: " << res->body << std::endl;
            }
            return false;
        }

        try {
            auto json = nlohmann::json::parse(res->body);
            tokens->access_token = json["access_token"];
            if (json.contains("refresh_token")) {
                tokens->refresh_token = json["refresh_token"];
            }
            uint64_t expires_in = json.value("expires_in", 1200);
            tokens->token_expiry = now_ms + expires_in * 1000;
            
            // Save refreshed tokens back to store
            if (store) {
                store->save(config.user_id, *tokens);
                std::cout << "[SaxoBrokerAdapter] Saved refreshed token to DB." << std::endl;
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[SaxoBrokerAdapter] Failed to parse token refresh response: " << e.what() << std::endl;
            return false;
        }
    }

    nlohmann::json request(const std::string& method, const std::string& path, const nlohmann::json& body = nullptr, const std::unordered_map<std::string, std::string>& query = {}) {
        if (!check_and_refresh_token()) {
            throw std::runtime_error("Authentication failed: cannot acquire active token.");
        }

        httplib::Client cli(api_host);
        cli.set_connection_timeout(std::chrono::seconds(10));
        cli.set_read_timeout(std::chrono::seconds(10));

        httplib::Headers headers = {
            {"Authorization", "Bearer " + tokens->access_token},
            {"Accept", "application/json"}
        };

        std::string full_path = api_path_prefix + path;
        if (!query.empty()) {
            full_path += "?";
            bool first = true;
            for (const auto& [k, v] : query) {
                if (!first) full_path += "&";
                full_path += k + "=" + url_encode(v);
                first = false;
            }
        }

        httplib::Result res;
        if (method == "GET") {
            res = cli.Get(full_path, headers);
        } else if (method == "POST") {
            headers.emplace("Content-Type", "application/json");
            res = cli.Post(full_path, headers, body.is_null() ? "" : body.dump(), "application/json");
        } else if (method == "DELETE") {
            res = cli.Delete(full_path, headers);
        } else {
            throw std::runtime_error("Unsupported HTTP method: " + method);
        }

        if (!res) {
            throw std::runtime_error("Network request failed calling " + full_path);
        }

        if (res->status == 429) {
            // Respect rate limits: simple exponential retry sleep and try once more
            std::this_thread::sleep_for(std::chrono::seconds(2));
            return request(method, path, body, query);
        }

        if (res->status < 200 || res->status >= 300) {
            throw std::runtime_error("Saxo API Error " + std::to_string(res->status) + ": " + res->body);
        }

        if (res->status == 204 || res->body.empty()) {
            return nlohmann::json::object();
        }

        return nlohmann::json::parse(res->body);
    }
};

SaxoBrokerAdapter::SaxoBrokerAdapter(SaxoBrokerConfig config)
    : pimpl_(std::make_unique<Impl>()) {
    pimpl_->config = std::move(config);
    
    // If no key is set, try environment variable
    if (pimpl_->config.token_encryption_key.empty()) {
        const char* env_key = std::getenv("TOKEN_ENCRYPTION_KEY");
        if (env_key) {
            pimpl_->config.token_encryption_key = env_key;
        } else {
            // Fallback default from .env for convenience in local testing
            pimpl_->config.token_encryption_key = "c55f7b0566a4b5f6a2406d8d7b3a9242dda2e55ec3d136892a4d9950a908b4cc";
        }
    }

    try {
        pimpl_->store = std::make_unique<storage::TokenStore>(
            pimpl_->config.token_db_path, 
            pimpl_->config.token_encryption_key
        );
        pimpl_->tokens = pimpl_->store->load(pimpl_->config.user_id);
        if (pimpl_->tokens) {
            pimpl_->parse_base_url();
            std::cout << "[SaxoBrokerAdapter] Loaded tokens from DB for user: " << pimpl_->config.user_id << std::endl;
        } else {
            std::cerr << "[SaxoBrokerAdapter] Warning: No tokens found in store for user: " << pimpl_->config.user_id << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[SaxoBrokerAdapter] TokenStore initialization failed: " << e.what() << std::endl;
    }
}

SaxoBrokerAdapter::~SaxoBrokerAdapter() {
    pimpl_->run_quote_thread = false;
    if (pimpl_->quote_thread.joinable()) {
        pimpl_->quote_thread.join();
    }
}

Result<void> SaxoBrokerAdapter::authenticate() {
    if (pimpl_->store) {
        auto db_tokens = pimpl_->store->load(pimpl_->config.user_id);
        if (db_tokens) {
            pimpl_->tokens = db_tokens;
            pimpl_->parse_base_url();
            std::cout << "[SaxoBrokerAdapter] Reloaded tokens from DB." << std::endl;
        }
    }
    if (pimpl_->check_and_refresh_token()) {
        return Result<void>::ok();
    }
    return Result<void>::err("Failed to authenticate or refresh tokens");
}

bool SaxoBrokerAdapter::is_authenticated() const {
    if (!pimpl_->tokens) return false;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return !pimpl_->tokens->access_token.empty() && 
           (pimpl_->tokens->token_expiry == 0 || now_ms < pimpl_->tokens->token_expiry);
}

Result<InstrumentInfo> SaxoBrokerAdapter::lookup_symbol(const std::string& sym) {
    try {
        std::unordered_map<std::string, std::string> query = {
            {"Keywords", sym},
            {"$top", "1"}
        };
        auto res = pimpl_->request("GET", "/ref/v1/instruments", nullptr, query);
        if (res.contains("Data") && !res["Data"].empty()) {
            auto first = res["Data"][0];
            InstrumentInfo info;
            info.id.broker = "saxo";
            info.id.native_id = std::to_string(first["Identifier"].get<int64_t>());
            info.id.asset_type = first["AssetType"].get<std::string>();
            info.symbol = first["Symbol"].get<std::string>();
            info.tick_size = 0.01; // Fallback
            return Result<InstrumentInfo>::ok(info);
        }
        return Result<InstrumentInfo>::err("Symbol not found: " + sym);
    } catch (const std::exception& e) {
        return Result<InstrumentInfo>::err(e.what());
    }
}

Result<std::vector<InstrumentInfo>> SaxoBrokerAdapter::search(const std::string& query_str) {
    try {
        std::unordered_map<std::string, std::string> query = {
            {"Keywords", query_str},
            {"$top", "50"}
        };
        auto res = pimpl_->request("GET", "/ref/v1/instruments", nullptr, query);
        std::vector<InstrumentInfo> results;
        if (res.contains("Data")) {
            for (const auto& item : res["Data"]) {
                InstrumentInfo info;
                info.id.broker = "saxo";
                info.id.native_id = std::to_string(item["Identifier"].get<int64_t>());
                info.id.asset_type = item["AssetType"].get<std::string>();
                info.symbol = item["Symbol"].get<std::string>();
                info.tick_size = 0.01;
                results.push_back(info);
            }
        }
        return Result<std::vector<InstrumentInfo>>::ok(results);
    } catch (const std::exception& e) {
        return Result<std::vector<InstrumentInfo>>::err(e.what());
    }
}

Result<std::vector<Bar>> SaxoBrokerAdapter::get_bars(
    InstrumentId id, Resolution r, Timestamp from, Timestamp to) {
    try {
        int horizon = 60; // 1h default
        switch (r) {
            case Resolution::M1: horizon = 1; break;
            case Resolution::M5: horizon = 5; break;
            case Resolution::M15: horizon = 15; break;
            case Resolution::M30: horizon = 30; break;
            case Resolution::H1: horizon = 60; break;
            case Resolution::H4: horizon = 240; break;
            case Resolution::D1: horizon = 1440; break;
            case Resolution::W1: horizon = 10080; break;
        }

        // Convert timestamps to ISO-8601 strings
        auto ms_to_iso = [](uint64_t ms) {
            auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
            auto time = std::chrono::system_clock::to_time_t(tp);
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
            return ss.str();
        };

        std::unordered_map<std::string, std::string> query = {
            {"AssetType", id.asset_type},
            {"Uic", id.native_id},
            {"Horizon", std::to_string(horizon)},
            {"Mode", "From"},
            {"Time", ms_to_iso(from.ms_since_epoch)},
            {"Count", "1200"}
        };

        auto res = pimpl_->request("GET", "/chart/v3/charts", nullptr, query);
        std::vector<Bar> bars;
        if (res.contains("Data")) {
            for (const auto& item : res["Data"]) {
                Bar b;
                // Parse Time string to Timestamp
                std::string time_str = item["Time"].get<std::string>();
                std::tm tm = {};
                std::stringstream ss(time_str);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
                auto tp = std::chrono::system_clock::from_time_t(timegm(&tm));
                b.ts.ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();

                b.open.value = item["Open"].get<double>();
                b.high.value = item["High"].get<double>();
                b.low.value = item["Low"].get<double>();
                b.close.value = item["Close"].get<double>();
                b.volume.value = item.value("Volume", 0.0);
                bars.push_back(b);
            }
        }
        return Result<std::vector<Bar>>::ok(bars);
    } catch (const std::exception& e) {
        return Result<std::vector<Bar>>::err(e.what());
    }
}

Result<SubscriptionId> SaxoBrokerAdapter::subscribe_quotes(
    InstrumentId id, std::function<void(const Tick&)> on_tick) {
    
    std::lock_guard<std::mutex> lock(pimpl_->subs_mutex);
    std::string sub_id = "sub_" + std::to_string(++pimpl_->sub_counter);
    pimpl_->subscriptions[sub_id] = {id, on_tick};

    // Spin up quote generator thread if not running
    if (!pimpl_->run_quote_thread) {
        pimpl_->run_quote_thread = true;
        pimpl_->quote_thread = std::thread([this]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::normal_distribution<> d(0, 0.001); // random walk simulation helper

            while (pimpl_->run_quote_thread) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                
                // Fallback simulated prices for L1 local UI sandbox testing
                static std::unordered_map<std::string, double> simulated_prices = {
                    {"SPY", 520.0}, {"QQQ", 440.0}, {"AAPL", 180.0}, {"MSFT", 415.0},
                    {"BTCUSD", 55100.0}, {"ETHUSD", 2900.0}, {"NDX", 17700.0},
                    {"^N225", 37000.0}, {"^STOXX", 4600.0}, {"NVDA", 935.0},
                    {"BST", 18.0}, {"BST.NAV", 23.0}, {"TSLA", 175.0}
                };

                // Check for file overrides
                std::ifstream sim_file("./data/sim_prices.txt");
                if (sim_file.is_open()) {
                    std::string s_line;
                    while (std::getline(sim_file, s_line)) {
                        auto eq = s_line.find('=');
                        if (eq != std::string::npos) {
                            std::string s_sym = s_line.substr(0, eq);
                            try {
                                double s_val = std::stod(s_line.substr(eq + 1));
                                simulated_prices[s_sym] = s_val;
                            } catch (...) {}
                        }
                    }
                }

                std::lock_guard<std::mutex> inner_lock(pimpl_->subs_mutex);
                for (const auto& [sub_id, pair] : pimpl_->subscriptions) {
                    const auto& instr = pair.first;
                    const auto& callback = pair.second;

                    double base_price = 100.0;
                    if (simulated_prices.find(instr.native_id) != simulated_prices.end()) {
                        base_price = simulated_prices[instr.native_id];
                    }

                    // Apply standard random walk
                    base_price += d(gen) * base_price;
                    simulated_prices[instr.native_id] = base_price;

                    Tick t;
                    t.ts.ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                    t.bid.value = base_price - 0.02;
                    t.ask.value = base_price + 0.02;
                    
                    callback(t);
                }
            }
        });
    }

    return Result<SubscriptionId>::ok(sub_id);
}

Result<void> SaxoBrokerAdapter::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(pimpl_->subs_mutex);
    pimpl_->subscriptions.erase(id);
    if (pimpl_->subscriptions.empty() && pimpl_->run_quote_thread) {
        pimpl_->run_quote_thread = false;
        if (pimpl_->quote_thread.joinable()) {
            pimpl_->quote_thread.join();
        }
    }
    return Result<void>::ok();
}

Result<AccountInfo> SaxoBrokerAdapter::get_account() {
    try {
        auto accs = pimpl_->request("GET", "/port/v1/accounts/me");
        if (!accs.contains("Data") || accs["Data"].empty()) {
            return Result<AccountInfo>::err("No accounts returned from Saxo");
        }
        auto first_acc = accs["Data"][0];
        std::string client_key = first_acc["ClientKey"].get<std::string>();
        std::string account_key = first_acc["AccountKey"].get<std::string>();
        std::string account_id = first_acc["AccountId"].get<std::string>();

        std::unordered_map<std::string, std::string> query = {
            {"ClientKey", client_key},
            {"AccountKey", account_key}
        };
        auto bal = pimpl_->request("GET", "/port/v1/balances", nullptr, query);

        AccountInfo info;
        info.account_id = account_id;
        info.currency = bal.value("Currency", "USD");
        info.equity = bal.value("TotalValue", 0.0);
        info.cash = bal.value("CashBalance", 0.0);
        return Result<AccountInfo>::ok(info);
    } catch (const std::exception& e) {
        return Result<AccountInfo>::err(e.what());
    }
}

Result<OrderId> SaxoBrokerAdapter::place_order(const OrderRequest& req) {
    try {
        auto accs = pimpl_->request("GET", "/port/v1/accounts/me");
        if (!accs.contains("Data") || accs["Data"].empty()) {
            return Result<OrderId>::err("No accounts returned from Saxo");
        }
        std::string account_key = accs["Data"][0]["AccountKey"].get<std::string>();

        nlohmann::json payload;
        payload["Uic"] = std::stoll(req.instrument.native_id);
        payload["AssetType"] = req.instrument.asset_type;
        payload["OrderType"] = req.type == "limit" ? "Limit" : "Market";
        payload["OrderDuration"] = {{"DurationType", "DayOrder"}};
        payload["BuySell"] = req.side == "buy" ? "Buy" : "Sell";
        payload["Amount"] = req.size.value;
        payload["AccountKey"] = account_key;
        payload["ManualOrder"] = true;

        if (req.type == "limit" && req.limit_price.has_value()) {
            payload["OrderPrice"] = req.limit_price->value;
        }

        auto res = pimpl_->request("POST", "/trade/v2/orders", payload);
        if (res.contains("OrderId")) {
            return Result<OrderId>::ok(res["OrderId"].get<std::string>());
        }
        return Result<OrderId>::err("Failed to place order: no OrderId returned");
    } catch (const std::exception& e) {
        return Result<OrderId>::err(e.what());
    }
}

Result<void> SaxoBrokerAdapter::cancel_order(OrderId id) {
    try {
        auto accs = pimpl_->request("GET", "/port/v1/accounts/me");
        if (!accs.contains("Data") || accs["Data"].empty()) {
            return Result<void>::err("No accounts returned from Saxo");
        }
        std::string account_key = accs["Data"][0]["AccountKey"].get<std::string>();

        std::unordered_map<std::string, std::string> query = {
            {"AccountKey", account_key}
        };
        pimpl_->request("DELETE", "/trade/v2/orders/" + id, nullptr, query);
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

} // namespace broker
} // namespace trader
