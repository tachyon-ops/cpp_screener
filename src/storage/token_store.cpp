#include "trader/storage/token_store.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <chrono>

namespace trader {
namespace storage {

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t) strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

TokenStore::TokenStore(const std::string& db_path, const std::string& hex_key)
    : db_path_(db_path) {
    if (hex_key.length() != 64) {
        throw std::runtime_error("TOKEN_ENCRYPTION_KEY must be 64 hex characters (32 bytes)");
    }
    std::vector<uint8_t> key_bytes = hex_to_bytes(hex_key);
    raw_key_ = std::string(reinterpret_cast<char*>(key_bytes.data()), key_bytes.size());

    // Open/Create database and ensure table schema
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[TokenStore] Failed to open database: " << sqlite3_errmsg(db) << std::endl;
        if (db) sqlite3_close(db);
        return;
    }

    const char* schema = 
        "CREATE TABLE IF NOT EXISTS tokens ("
        "  id TEXT PRIMARY KEY,"
        "  iv BLOB NOT NULL,"
        "  auth_tag BLOB NOT NULL,"
        "  ciphertext BLOB NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");";
    
    char* err_msg = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "[TokenStore] Schema creation failed: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    sqlite3_close(db);
}

TokenStore::~TokenStore() = default;

static bool decrypt_gcm(
    const uint8_t* ciphertext, int ciphertext_len,
    const uint8_t* key,
    const uint8_t* iv, int iv_len,
    const uint8_t* tag, int tag_len,
    std::vector<uint8_t>& plaintext) 
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, iv_len, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    plaintext.resize(ciphertext_len + EVP_CIPHER_block_size(EVP_aes_256_gcm()));
    int len = 0;
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext, ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    int plaintext_len = len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, tag_len, const_cast<uint8_t*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret > 0) {
        plaintext_len += len;
        plaintext.resize(plaintext_len);
        return true;
    }
    return false;
}

static bool encrypt_gcm(
    const uint8_t* plaintext, int plaintext_len,
    const uint8_t* key,
    const uint8_t* iv, int iv_len,
    std::vector<uint8_t>& ciphertext,
    std::vector<uint8_t>& tag) 
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, iv_len, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    ciphertext.resize(plaintext_len + EVP_CIPHER_block_size(EVP_aes_256_gcm()));
    int len = 0;
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext, plaintext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    int ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    ciphertext_len += len;
    ciphertext.resize(ciphertext_len);

    tag.resize(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, tag.size(), tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

std::optional<SaxoTokens> TokenStore::load(const std::string& user_id) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return std::nullopt;
    }

    const char* query = "SELECT iv, auth_tag, ciphertext FROM tokens WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<SaxoTokens> result = std::nullopt;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* iv_data = sqlite3_column_blob(stmt, 0);
        int iv_len = sqlite3_column_bytes(stmt, 0);
        
        const void* tag_data = sqlite3_column_blob(stmt, 1);
        int tag_len = sqlite3_column_bytes(stmt, 1);
        
        const void* ct_data = sqlite3_column_blob(stmt, 2);
        int ct_len = sqlite3_column_bytes(stmt, 2);

        if (iv_len == 12 && tag_len == 16) {
            std::vector<uint8_t> plaintext;
            bool success = decrypt_gcm(
                reinterpret_cast<const uint8_t*>(ct_data), ct_len,
                reinterpret_cast<const uint8_t*>(raw_key_.data()),
                reinterpret_cast<const uint8_t*>(iv_data), iv_len,
                reinterpret_cast<const uint8_t*>(tag_data), tag_len,
                plaintext
            );

            if (success) {
                try {
                    std::string plain_str(reinterpret_cast<char*>(plaintext.data()), plaintext.size());
                    auto json = nlohmann::json::parse(plain_str);
                    
                    SaxoTokens tokens;
                    if (json.contains("accessToken")) tokens.access_token = json["accessToken"];
                    if (json.contains("openApiBase")) tokens.open_api_base = json["openApiBase"];
                    if (json.contains("appKey")) tokens.app_key = json["appKey"];
                    if (json.contains("appSecret")) tokens.app_secret = json["appSecret"];
                    if (json.contains("redirectUrl")) tokens.redirect_url = json["redirectUrl"];
                    if (json.contains("authBase")) tokens.auth_base = json["authBase"];
                    if (json.contains("refreshToken")) tokens.refresh_token = json["refreshToken"];
                    if (json.contains("tokenExpiry")) tokens.token_expiry = json["tokenExpiry"];

                    result = tokens;
                } catch (const std::exception& e) {
                    std::cerr << "[TokenStore] JSON parsing failed: " << e.what() << std::endl;
                }
            } else {
                std::cerr << "[TokenStore] AES-GCM Decryption failed for user: " << user_id << std::endl;
            }
        } else {
            std::cerr << "[TokenStore] Invalid IV or tag sizes in DB" << std::endl;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

bool TokenStore::save(const std::string& user_id, const SaxoTokens& tokens) {
    nlohmann::json json;
    json["accessToken"] = tokens.access_token;
    json["openApiBase"] = tokens.open_api_base;
    json["appKey"] = tokens.app_key;
    json["appSecret"] = tokens.app_secret;
    json["redirectUrl"] = tokens.redirect_url;
    json["authBase"] = tokens.auth_base;
    json["refreshToken"] = tokens.refresh_token;
    json["tokenExpiry"] = tokens.token_expiry;

    std::string plaintext = json.dump();

    std::vector<uint8_t> iv(12);
    if (RAND_bytes(iv.data(), iv.size()) != 1) {
        return false;
    }

    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> tag;
    bool success = encrypt_gcm(
        reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size(),
        reinterpret_cast<const uint8_t*>(raw_key_.data()),
        iv.data(), iv.size(),
        ciphertext,
        tag
    );

    if (!success) return false;

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    const char* query = 
        "INSERT INTO tokens (id, iv, auth_tag, ciphertext, updated_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  iv = excluded.iv, "
        "  auth_tag = excluded.auth_tag, "
        "  ciphertext = excluded.ciphertext, "
        "  updated_at = excluded.updated_at;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, iv.data(), iv.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, tag.data(), tag.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 4, ciphertext.data(), ciphertext.size(), SQLITE_TRANSIENT);
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    sqlite3_bind_int64(stmt, 5, now);

    bool save_success = (sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return save_success;
}

bool TokenStore::remove(const std::string& user_id) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    const char* query = "DELETE FROM tokens WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);

    bool del_success = (sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return del_success;
}

} // namespace storage
} // namespace trader
