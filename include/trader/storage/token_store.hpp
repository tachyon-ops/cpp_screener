#pragma once

#include <string>
#include <optional>

namespace trader {
namespace storage {

struct SaxoTokens {
    std::string access_token;
    std::string open_api_base;
    std::string app_key;
    std::string app_secret;
    std::string redirect_url;
    std::string auth_base;
    std::string refresh_token;
    uint64_t token_expiry = 0; // ms timestamp
};

class TokenStore {
public:
    // db_path: path to SQLite tokens.db
    // hex_key: 64 hex characters (32 bytes key)
    TokenStore(const std::string& db_path, const std::string& hex_key);
    ~TokenStore();

    // Loads tokens for a user. Returns std::nullopt if not found or decryption fails.
    std::optional<SaxoTokens> load(const std::string& user_id);

    // Saves tokens for a user, encrypting them using AES-256-GCM.
    bool save(const std::string& user_id, const SaxoTokens& tokens);

    // Deletes tokens for a user.
    bool remove(const std::string& user_id);

private:
    std::string db_path_;
    std::string raw_key_; // 32 bytes binary key
};

} // namespace storage
} // namespace trader
