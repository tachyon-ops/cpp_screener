#include "trader/core/alert_dispatcher.hpp"
#include "trader/core/regime_classifier.hpp"
#include <httplib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <ctime>

namespace trader {
namespace core {

// Helper function to get Europe/Zurich time tm struct from steady_clock or system_clock
struct tm get_zurich_time_now() {
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm utc_tm = *std::gmtime(&t);
    int year = utc_tm.tm_year + 1900;
    
    // Europe/Zurich DST offsets: CET (UTC+1), CEST (UTC+2)
    // Starts: last Sunday of March, Ends: last Sunday of October
    int offset = 1; // Default CET
    if (utc_tm.tm_mon > 2 && utc_tm.tm_mon < 9) { // April to September
        offset = 2;
    } else if (utc_tm.tm_mon == 2) { // March
        struct tm temp = {};
        temp.tm_year = year - 1900;
        temp.tm_mon = 2;
        temp.tm_mday = 31;
        mktime(&temp);
        int last_sunday = 31 - temp.tm_wday;
        if (utc_tm.tm_mday > last_sunday || (utc_tm.tm_mday == last_sunday && utc_tm.tm_hour >= 1)) {
            offset = 2;
        }
    } else if (utc_tm.tm_mon == 9) { // October
        struct tm temp = {};
        temp.tm_year = year - 1900;
        temp.tm_mon = 9;
        temp.tm_mday = 31;
        mktime(&temp);
        int last_sunday = 31 - temp.tm_wday;
        if (utc_tm.tm_mday < last_sunday || (utc_tm.tm_mday == last_sunday && utc_tm.tm_hour < 1)) {
            offset = 2;
        }
    }
    
    t += offset * 3600;
    return *std::gmtime(&t);
}

// Parses clusters.yaml manually to avoid external dependency issues
std::vector<ClusterDefinition> parse_clusters_yaml(const std::string& path) {
    std::vector<ClusterDefinition> result;
    std::ifstream infile(path);
    if (!infile.is_open()) {
        std::cerr << "[AlertDispatcher] Warning: Could not open clusters file: " << path << std::endl;
        return result;
    }
    
    std::string line;
    ClusterDefinition current;
    bool in_cluster = false;
    
    auto trim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), s.end());
    };
    
    while (std::getline(infile, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        
        if (line.rfind("- id:", 0) == 0) {
            if (in_cluster) {
                result.push_back(current);
                current = ClusterDefinition();
            }
            in_cluster = true;
            current.id = line.substr(5);
            trim(current.id);
        } else if (line.rfind("id:", 0) == 0) {
            if (in_cluster) {
                result.push_back(current);
                current = ClusterDefinition();
            }
            in_cluster = true;
            current.id = line.substr(3);
            trim(current.id);
        } else if (line.rfind("leader:", 0) == 0) {
            current.leader = line.substr(7);
            trim(current.leader);
        } else if (line.rfind("asset_class:", 0) == 0) {
            current.asset_class = line.substr(12);
            trim(current.asset_class);
        } else if (line.rfind("members:", 0) == 0) {
            std::string mems_str = line.substr(8);
            trim(mems_str);
            size_t start = mems_str.find('[');
            size_t end = mems_str.find(']');
            if (start != std::string::npos && end != std::string::npos && end > start) {
                std::string list_content = mems_str.substr(start + 1, end - start - 1);
                std::stringstream ss(list_content);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    trim(item);
                    if (!item.empty()) {
                        current.members.push_back(item);
                    }
                }
            }
        }
    }
    if (in_cluster && !current.id.empty()) {
        result.push_back(current);
    }
    return result;
}

// Check if Pushover is enabled in config.yaml
bool parse_pushover_enabled_yaml() {
    std::ifstream infile("./config/config.yaml");
    if (!infile.is_open()) return false;
    std::string line;
    bool in_pushover_section = false;
    while (std::getline(infile, line)) {
        if (line.find("pushover:") != std::string::npos) {
            in_pushover_section = true;
            continue;
        }
        if (in_pushover_section) {
            if (line.find(":") != std::string::npos && line.find("pushover") == std::string::npos && line.front() != ' ' && line.front() != '\t') {
                in_pushover_section = false;
            } else {
                size_t pos = line.find("enabled:");
                if (pos != std::string::npos) {
                    std::string val = line.substr(pos + 8);
                    val.erase(val.begin(), std::find_if(val.begin(), val.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                    val.erase(std::find_if(val.rbegin(), val.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), val.end());
                    return val == "true";
                }
            }
        }
    }
    return false;
}

AlertDispatcher::AlertDispatcher(
    std::shared_ptr<persistence::SQLiteStore> store,
    std::shared_ptr<broker::BrokerAdapter> broker,
    std::shared_ptr<TelegramBot> tg_bot
) : store_(store), broker_(broker), tg_bot_(tg_bot) {
    reload_clusters();
}

AlertDispatcher::~AlertDispatcher() {
    stop();
}

void AlertDispatcher::reload_clusters() {
    clusters_ = parse_clusters_yaml("./config/clusters.yaml");
    std::cout << "[AlertDispatcher] Loaded " << clusters_.size() << " cluster definitions from YAML." << std::endl;
}

void AlertDispatcher::start() {
    if (running_) return;
    running_ = true;
    worker_thread_ = std::thread(&AlertDispatcher::process_loop, this);
    digest_thread_ = std::thread(&AlertDispatcher::digest_check_loop, this);
    std::cout << "[AlertDispatcher] Alert Dispatcher threads started." << std::endl;
}

void AlertDispatcher::stop() {
    if (!running_) return;
    running_ = false;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
    if (digest_thread_.joinable()) digest_thread_.join();
    std::cout << "[AlertDispatcher] Alert Dispatcher threads stopped." << std::endl;
}

void AlertDispatcher::dispatch(const Alert& alert) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(alert);
    queue_cv_.notify_one();
}

void AlertDispatcher::dispatch_telegram_message(const std::string& message) {
    auto enabled_val = store_->get_setting("telegram_enabled");
    bool telegram_enabled = !enabled_val || (*enabled_val == "true");
    if (telegram_enabled && tg_bot_) {
        std::string chat_id = tg_bot_->get_chat_id("premium");
        if (!chat_id.empty()) {
            tg_bot_->send_message(chat_id, message);
        }
    }
}

void AlertDispatcher::set_alert_callback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    alert_cb_ = cb;
}

namespace {
nlohmann::json serialize_size(const Alert::PositionSize& s) {
    nlohmann::json sz;
    sz["units"] = s.units;
    sz["cost"] = s.cost;
    sz["pct_account"] = s.pct_account;
    sz["capped"] = s.capped;
    return sz;
}

void add_position_sizes_to_payload(nlohmann::json& payload, const Alert& alert) {
    payload["size_1pct"] = serialize_size(alert.size_1pct);
    payload["size_2pct"] = serialize_size(alert.size_2pct);
    payload["size_5pct"] = serialize_size(alert.size_5pct);
}

nlohmann::json build_alert_payload(const Alert& alert, const std::string& default_trigger = "") {
    nlohmann::json payload;
    payload["symbol"] = alert.symbol;
    payload["suggested_entry_low"] = alert.suggested_entry_low;
    payload["suggested_entry_high"] = alert.suggested_entry_high;
    payload["suggested_stop"] = alert.suggested_stop;
    payload["target_1"] = alert.target_1;
    payload["target_2"] = alert.target_2;
    payload["target_3"] = alert.target_3;
    payload["rr_to_target_1"] = alert.rr_to_target_1;
    payload["price"] = alert.suggested_entry_high;
    
    if (!default_trigger.empty()) {
        payload["trigger"] = default_trigger;
    } else {
        if (alert.screen == "A") {
            payload["trigger"] = "Intraday Mean Reversion: " + alert.news_summary;
        } else if (alert.screen == "B") {
            payload["trigger"] = "Swing Pullback setup found";
        } else {
            payload["trigger"] = "200-day MA crossover detected";
        }
    }
    
    add_position_sizes_to_payload(payload, alert);
    
    if (!alert.confluence_factors.empty()) {
        payload["confluence_factors"] = alert.confluence_factors;
    }
    if (!alert.news_summary.empty()) {
        payload["news_summary"] = alert.news_summary;
    }
    if (!alert.extra.empty()) {
        for (auto it = alert.extra.begin(); it != alert.extra.end(); ++it) {
            payload[it.key()] = it.value();
        }
    }
    
    return payload;
}
}

void AlertDispatcher::calculate_position_sizes(Alert& alert) {
    double equity = 1000000.0; // Fallback 1M
    auto acc_res = broker_->get_account();
    if (acc_res.is_ok()) {
        equity = acc_res.value().equity;
    }
    
    double entry = alert.suggested_entry_high;
    double stop = alert.suggested_stop;
    double risk_per_unit = entry - stop;
    
    if (risk_per_unit <= 0.0) {
        alert.size_1pct = {0, 0, 0, false};
        alert.size_2pct = {0, 0, 0, false};
        alert.size_5pct = {0, 0, 0, false};
        return;
    }

    double cap_pct = 0.30; // default 30%
    if (store_) {
        auto opt_inst = store_->get_instrument_by_symbol(alert.symbol);
        if (opt_inst) {
            std::string ac = opt_inst->asset_class;
            // Convert to lowercase for safety
            std::transform(ac.begin(), ac.end(), ac.begin(), ::tolower);
            if (ac == "crypto") {
                cap_pct = 0.30;
            } else if (ac == "equity" || ac == "equities" || ac == "stock") {
                cap_pct = 0.20;
            } else if (ac == "fx" || ac == "forex" || ac == "currency") {
                cap_pct = 0.50;
            } else if (ac == "commodity" || ac == "commodities") {
                cap_pct = 0.25;
            } else if (ac == "index" || ac == "indices") {
                cap_pct = 0.30;
            }
        }
    }
    
    auto calculate_tier = [equity, entry, risk_per_unit, cap_pct](double risk_pct) -> Alert::PositionSize {
        double risk_amt = equity * risk_pct;
        double units = risk_amt / risk_per_unit;
        double cost = units * entry;
        double max_cost = equity * cap_pct;
        bool capped = false;
        if (cost > max_cost) {
            units = max_cost / entry;
            cost = max_cost;
            capped = true;
        }
        double pct_acct = (cost / equity) * 100.0;
        return {units, cost, pct_acct, capped};
    };

    alert.size_1pct = calculate_tier(0.01);
    alert.size_2pct = calculate_tier(0.02);
    alert.size_5pct = calculate_tier(0.05);
}

std::string AlertDispatcher::get_cluster_id_for_symbol(const std::string& symbol) {
    // Priority A: manual clusters
    for (const auto& c : clusters_) {
        if (c.leader == symbol) return c.id;
        for (const auto& m : c.members) {
            if (m == symbol) return c.id;
        }
    }
    
    // Priority B: Sector ETF membership from instrument metadata
    auto inst = store_->get_instrument_by_symbol(symbol);
    if (inst && !inst->metadata_json.empty()) {
        try {
            auto meta = nlohmann::json::parse(inst->metadata_json);
            if (meta.contains("sector_etf_symbol")) {
                std::string sector_etf = meta["sector_etf_symbol"].get<std::string>();
                return "sector_" + sector_etf;
            }
        } catch (...) {}
    }
    
    return ""; // No cluster
}

bool AlertDispatcher::check_throttle_limit(const std::string& tier, Regime regime) {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    auto now = std::chrono::system_clock::now();
    
    int limit = 9999;
    std::chrono::minutes window(60);
    
    if (tier == "premium") {
        limit = (regime == Regime::Crisis) ? 6 : 3;
        window = std::chrono::minutes(30);
    } else if (tier == "opportunity") {
        limit = (regime == Regime::Crisis) ? 20 : 10;
        window = std::chrono::minutes(60);
    } else {
        return true; // No throttle limit for Interesting
    }
    
    // Filter timestamps within window
    auto& list = sent_alert_times_[tier];
    list.erase(std::remove_if(list.begin(), list.end(), [now, window](const std::chrono::system_clock::time_point& tp) {
        return (now - tp) > window;
    }), list.end());
    
    if (list.size() >= (size_t)limit) {
        return false; // Limit exceeded
    }
    
    list.push_back(now);
    return true;
}

void AlertDispatcher::process_loop() {
    std::cout << "[AlertDispatcher] Started processing worker thread." << std::endl;
    while (running_) {
        std::vector<Alert> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return !queue_.empty() || !running_; });
            if (!running_) break;
            
            // Gather alerts. Sleep 500ms to allow screens to dispatch all signals in the current tick.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            batch = std::move(queue_);
            queue_.clear();
        }
        
        if (!batch.empty()) {
            process_queued_alerts(batch);
        }
    }
}

void AlertDispatcher::process_queued_alerts(std::vector<Alert>& alerts) {
    // Get active regime to apply throttling limits
    Regime regime = Regime::Chop;
    auto logs = store_->get_regime_log(1);
    if (!logs.empty()) {
        regime = string_to_regime(logs[0].regime);
    }
    
    // 1. Calculate suggested sizes for all alerts
    for (auto& alert : alerts) {
        calculate_position_sizes(alert);
        // Default conviction score if not set
        if (alert.conviction_score == 0.0) {
            alert.conviction_score = alert.rr_to_target_1;
        }
    }

    // 2. Group alerts by cluster
    std::map<std::string, std::vector<Alert>> clustered;
    std::vector<Alert> standalone;
    
    for (const auto& alert : alerts) {
        std::string cluster_id = get_cluster_id_for_symbol(alert.symbol);
        if (!cluster_id.empty()) {
            clustered[cluster_id].push_back(alert);
        } else {
            standalone.push_back(alert);
        }
    }

    auto now = std::chrono::steady_clock::now();

    // 3. Process clusters
    for (auto& [cluster_id, list] : clustered) {
        // Check cooldown
        bool in_cooldown = false;
        {
            std::lock_guard<std::mutex> lock(cooldowns_mutex_);
            if (cluster_cooldowns_.count(cluster_id) && cluster_cooldowns_[cluster_id] > now) {
                in_cooldown = true;
            }
        }

        if (in_cooldown) {
            // Demote all to interesting, log to DB, don't push
            std::cout << "[AlertDispatcher] Cluster " << cluster_id << " is in cooldown. Demoting alerts to Interesting." << std::endl;
            for (auto& alert : list) {
                alert.tier = "interesting";
                persistence::DbAlert db_a;
                db_a.ts = alert.ts;
                db_a.screen = alert.screen;
                db_a.instrument_id = alert.instrument_id;
                db_a.tier = alert.tier;
                db_a.regime_at_alert = alert.regime_at_alert;
                
                nlohmann::json payload = build_alert_payload(alert, "Consolidation (demoted due to cluster cooldown)");
                db_a.payload_json = payload.dump();
                int64_t db_id = store_->add_alert(db_a);
                {
                    std::lock_guard<std::mutex> cb_lock(cb_mutex_);
                    if (alert_cb_) {
                        nlohmann::json obj;
                        obj["id"] = db_id;
                        obj["ts"] = db_a.ts;
                        obj["screen"] = db_a.screen;
                        obj["instrument_id"] = db_a.instrument_id;
                        obj["tier"] = db_a.tier;
                        obj["regime_at_alert"] = db_a.regime_at_alert;
                        obj["acted_on"] = 0;
                        obj["payload"] = payload;
                        alert_cb_(obj.dump());
                    }
                }
            }
            continue;
        }

        if (list.size() >= 3) {
            // Collapse cluster!
            // Pick leader
            Alert leader;
            std::string leader_symbol = "";
            for (const auto& c : clusters_) {
                if (c.id == cluster_id) {
                    leader_symbol = c.leader;
                    break;
                }
            }
            if (leader_symbol.empty() && cluster_id.rfind("sector_", 0) == 0) {
                // Sector auto-cluster leader is the ETF symbol itself
                leader_symbol = cluster_id.substr(7);
            }

            bool leader_found = false;
            for (const auto& alert : list) {
                if (alert.symbol == leader_symbol) {
                    leader = alert;
                    leader_found = true;
                    break;
                }
            }

            // Fallback leader selection: highest conviction
            if (!leader_found) {
                std::sort(list.begin(), list.end(), [](const Alert& a, const Alert& b) {
                    return a.conviction_score > b.conviction_score;
                });
                leader = list[0];
            }

            // Promote leader to premium
            leader.tier = "premium";
            
            // Format collapse context
            std::stringstream ss;
            ss << "🟢 <b>PREMIUM — " << leader.symbol << " (" << cluster_id << " cluster, " << list.size() << " names)</b>\n"
               << "Screen " << leader.screen << ", Regime: <b>" << leader.regime_at_alert << "</b>\n\n"
               << "<b>Leader signal details:</b>\n"
               << "  Entry: " << leader.suggested_entry_low << " – " << leader.suggested_entry_high << "\n"
               << "  Stop:  " << leader.suggested_stop << " (" << std::fixed << std::setprecision(1) << ((leader.suggested_stop - leader.suggested_entry_high) / leader.suggested_entry_high * 100.0) << "%)\n"
               << "  T1:    " << leader.target_1 << " (" << std::setprecision(1) << leader.rr_to_target_1 << "R)\n\n"
               << "<b>Cluster context (also wicking/triggered):</b>\n";
               
            for (const auto& alert : list) {
                if (alert.symbol != leader.symbol) {
                    ss << "  • " << alert.symbol << " @ " << alert.suggested_entry_high << " (Stop: " << alert.suggested_stop << ")\n";
                }
            }
            
            ss << "\n<b>Suggested play:</b>\n"
               << "  - Lead with " << leader.symbol << " for size, liquidity\n"
               << "  - OR pick 2-3 individuals with deepest moves\n"
               << "  - DO NOT take all " << list.size() << " — correlation = same trade";

            // Register cooldown
            {
                std::lock_guard<std::mutex> lock(cooldowns_mutex_);
                cluster_cooldowns_[cluster_id] = now + std::chrono::hours(4);
            }

            // Save leader to DB
            persistence::DbAlert db_leader;
            db_leader.ts = leader.ts;
            db_leader.screen = leader.screen;
            db_leader.instrument_id = leader.instrument_id;
            db_leader.tier = "premium";
            db_leader.regime_at_alert = leader.regime_at_alert;
            
            nlohmann::json payload = build_alert_payload(leader, "Collapsed cluster leader for " + cluster_id);
            db_leader.payload_json = payload.dump();
            int64_t leader_id = store_->add_alert(db_leader);
            leader.id = leader_id;
            {
                std::lock_guard<std::mutex> cb_lock(cb_mutex_);
                if (alert_cb_) {
                    nlohmann::json obj;
                    obj["id"] = leader_id;
                    obj["ts"] = db_leader.ts;
                    obj["screen"] = db_leader.screen;
                    obj["instrument_id"] = db_leader.instrument_id;
                    obj["tier"] = db_leader.tier;
                    obj["regime_at_alert"] = db_leader.regime_at_alert;
                    obj["acted_on"] = 0;
                    obj["payload"] = payload;
                    alert_cb_(obj.dump());
                }
            }

            // Generate dynamic inline buttons
            nlohmann::json btn_lead = nlohmann::json::object({{"text", "Lead with " + leader.symbol}, {"callback_data", "acting:" + std::to_string(leader_id)}});
            nlohmann::json btn_skip = nlohmann::json::object({{"text", "Skip Cluster"}, {"callback_data", "skip:" + std::to_string(leader_id)}});
            nlohmann::json inline_kb;
            inline_kb["inline_keyboard"] = nlohmann::json::array({
                nlohmann::json::array({btn_lead, btn_skip})
            });

            // Send to Premium telegram channel
            send_to_channels(leader, ss.str(), inline_kb.dump());
            
            // Demote other members to interesting and save silently
            for (const auto& alert : list) {
                if (alert.symbol != leader.symbol) {
                    persistence::DbAlert db_member;
                    db_member.ts = alert.ts;
                    db_member.screen = alert.screen;
                    db_member.instrument_id = alert.instrument_id;
                    db_member.tier = "interesting";
                    db_member.regime_at_alert = alert.regime_at_alert;
                    
                    nlohmann::json p = build_alert_payload(alert, "Demoted cluster member of " + cluster_id + " (leader: " + leader.symbol + ")");
                    db_member.payload_json = p.dump();
                    int64_t db_id = store_->add_alert(db_member);
                    {
                        std::lock_guard<std::mutex> cb_lock(cb_mutex_);
                        if (alert_cb_) {
                            nlohmann::json obj;
                            obj["id"] = db_id;
                            obj["ts"] = db_member.ts;
                            obj["screen"] = db_member.screen;
                            obj["instrument_id"] = db_member.instrument_id;
                            obj["tier"] = db_member.tier;
                            obj["regime_at_alert"] = db_member.regime_at_alert;
                            obj["acted_on"] = 0;
                            obj["payload"] = p;
                            alert_cb_(obj.dump());
                        }
                    }
                }
            }
        } else {
            // Under 3 names in cluster, treat as standalone alerts
            for (const auto& alert : list) {
                standalone.push_back(alert);
            }
        }
    }

    // 4. Process standalone alerts (including throtlling check)
    for (auto& alert : standalone) {
        // Enforce throttle limits. If limit is hit, we demote.
        bool allowed = check_throttle_limit(alert.tier, regime);
        if (!allowed) {
            std::cout << "[AlertDispatcher] Throttle limit hit for tier: " << alert.tier 
                      << ". Demoting alert for " << alert.symbol << std::endl;
            if (alert.tier == "premium") {
                alert.tier = "opportunity";
                // Check if opportunity has space
                if (!check_throttle_limit("opportunity", regime)) {
                    alert.tier = "interesting";
                }
            } else if (alert.tier == "opportunity") {
                alert.tier = "interesting";
            }
        }
        
        // Save to DB
        persistence::DbAlert db_a;
        db_a.ts = alert.ts;
        db_a.screen = alert.screen;
        db_a.instrument_id = alert.instrument_id;
        db_a.tier = alert.tier;
        db_a.regime_at_alert = alert.regime_at_alert;
        
        nlohmann::json payload = build_alert_payload(alert);
        db_a.payload_json = payload.dump();
        
        int64_t db_id = store_->add_alert(db_a);
        alert.id = db_id;
        {
            std::lock_guard<std::mutex> cb_lock(cb_mutex_);
            if (alert_cb_) {
                nlohmann::json obj;
                obj["id"] = db_id;
                obj["ts"] = db_a.ts;
                obj["screen"] = db_a.screen;
                obj["instrument_id"] = db_a.instrument_id;
                obj["tier"] = db_a.tier;
                obj["regime_at_alert"] = db_a.regime_at_alert;
                obj["acted_on"] = 0;
                obj["payload"] = payload;
                alert_cb_(obj.dump());
            }
        }

        // If interesting, save only (no push)
        if (alert.tier == "interesting") {
            continue;
        }

        // Format standalone rich text message
        std::stringstream ss;
        std::string color_dot = (alert.tier == "premium" ? "🟢" : "🟡");
        std::string tier_upper = alert.tier;
        std::transform(tier_upper.begin(), tier_upper.end(), tier_upper.begin(), ::toupper);

        ss << color_dot << " <b>" << tier_upper << " — " << alert.symbol << "</b>\n"
           << "Screen " << alert.screen << " (" << (alert.screen == "B" ? "Swing Pullback" : "200MA Crossover") << ")\n"
           << "Regime: <b>" << alert.regime_at_alert << "</b>\n\n"
           << "Entry: " << std::fixed << std::setprecision(2) << alert.suggested_entry_low << " – " << alert.suggested_entry_high << "\n"
           << "Stop:  " << alert.suggested_stop << " (" << std::setprecision(1) << ((alert.suggested_stop - alert.suggested_entry_high) / alert.suggested_entry_high * 100.0) << "%)\n"
           << "T1:    " << alert.target_1 << " (" << std::setprecision(1) << alert.rr_to_target_1 << "R)\n";
           
        if (alert.target_2 > 0.0) {
            ss << "T2:    " << alert.target_2 << " (" << std::setprecision(1) << (alert.rr_to_target_1 * 2.0) << "R)\n";
        }

        if (!alert.confluence_factors.empty()) {
            ss << "\nConfluence: ";
            for (size_t k = 0; k < alert.confluence_factors.size(); ++k) {
                ss << alert.confluence_factors[k] << (k + 1 < alert.confluence_factors.size() ? " + " : "");
            }
            ss << "\n";
        }

        ss << "\nSize for 1% risk: " << std::setprecision(2) << alert.size_1pct.units << " units ($" << (int)alert.size_1pct.cost << ", " << std::setprecision(1) << alert.size_1pct.pct_account << "% account)" << (alert.size_1pct.capped ? " — <b>CAPPED</b>" : "") << "\n"
           << "Size for 2% risk: " << alert.size_2pct.units << " units ($" << (int)alert.size_2pct.cost << ", " << alert.size_2pct.pct_account << "% account)" << (alert.size_2pct.capped ? " — <b>CAPPED</b>" : "") << "\n"
           << "Size for 5% risk: " << alert.size_5pct.units << " units ($" << (int)alert.size_5pct.cost << ", " << alert.size_5pct.pct_account << "% account)" << (alert.size_5pct.capped ? " — <b>CAPPED</b>" : "") << "\n";

        nlohmann::json btn_saw   = nlohmann::json::object({{"text", "👀 Saw it"}, {"callback_data", "saw_it:" + std::to_string(db_id)}});
        nlohmann::json btn_act   = nlohmann::json::object({{"text", "⚡ Acting"}, {"callback_data", "acting:" + std::to_string(db_id)}});
        nlohmann::json btn_skip  = nlohmann::json::object({{"text", "❌ Skip"}, {"callback_data", "skip:" + std::to_string(db_id)}});
        nlohmann::json btn_defer = nlohmann::json::object({{"text", "⏸ Defer"}, {"callback_data", "defer:" + std::to_string(db_id)}});
        nlohmann::json btn_note  = nlohmann::json::object({{"text", "📝 Note"}, {"callback_data", "note:" + std::to_string(db_id)}});
        nlohmann::json markup;
        markup["inline_keyboard"] = nlohmann::json::array({
            nlohmann::json::array({btn_saw, btn_act}),
            nlohmann::json::array({btn_skip, btn_defer, btn_note})
        });

        send_to_channels(alert, ss.str(), markup.dump());
    }
}

void AlertDispatcher::send_to_channels(const Alert& alert, const std::string& custom_text, const std::string& custom_markup) {
    auto enabled_val = store_->get_setting("telegram_enabled");
    bool telegram_enabled = !enabled_val || (*enabled_val == "true");
    if (telegram_enabled) {
        std::string chat_id = tg_bot_->get_chat_id(alert.tier);
        if (!chat_id.empty()) {
            tg_bot_->send_message(chat_id, custom_text, custom_markup);
        }
    }
    
    // Premium only Pushover dual-channel push
    if (alert.tier == "premium" && parse_pushover_enabled_yaml()) {
        const char* app_token = std::getenv("PUSHOVER_APP_TOKEN");
        const char* user_key = std::getenv("PUSHOVER_USER_KEY");
        if (app_token && user_key) {
            std::stringstream ss;
            ss << "🟢 PREMIUM — " << alert.symbol << " setup at " << alert.suggested_entry_high;
            send_pushover_message(ss.str());
        }
    }
}

void AlertDispatcher::send_pushover_message(const std::string& message) {
    const char* app_token = std::getenv("PUSHOVER_APP_TOKEN");
    const char* user_key = std::getenv("PUSHOVER_USER_KEY");
    if (!app_token || !user_key) return;

    httplib::Client cli("https://api.pushover.net");
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(5));

    nlohmann::json body = {
        {"token", app_token},
        {"user", user_key},
        {"message", message}
    };

    auto res = cli.Post("/1/messages.json", body.dump(), "application/json");
    if (!res || res->status != 200) {
        std::cerr << "[Pushover] API request failed." << std::endl;
    }
}

void AlertDispatcher::digest_check_loop() {
    std::cout << "[AlertDispatcher] Started digest monitor thread." << std::endl;
    while (running_) {
        // Sleep 30 seconds
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!running_) break;

        struct tm local_t = get_zurich_time_now();
        
        // EOD digest fires at 22:30 local Zurich time
        if (local_t.tm_hour == 22 && local_t.tm_min >= 30) {
            std::string today_date = std::to_string(local_t.tm_year + 1900) + "-" + 
                                     (local_t.tm_mon + 1 < 10 ? "0" : "") + std::to_string(local_t.tm_mon + 1) + "-" + 
                                     (local_t.tm_mday < 10 ? "0" : "") + std::to_string(local_t.tm_mday);
            
            bool run = false;
            {
                std::lock_guard<std::mutex> lock(digest_mutex_);
                if (last_digest_day_ != today_date) {
                    last_digest_day_ = today_date;
                    run = true;
                }
            }
            
            if (run) {
                std::cout << "[AlertDispatcher] Triggering Europe/Zurich EOD daily digest job..." << std::endl;
                send_daily_digest();
            }
        }
    }
}

void AlertDispatcher::send_daily_digest() {
    auto enabled_val = store_->get_setting("telegram_enabled");
    bool telegram_enabled = !enabled_val || (*enabled_val == "true");
    if (!telegram_enabled) return;

    std::string chat_id = tg_bot_->get_chat_id("digest");
    if (chat_id.empty()) {
        std::cerr << "[AlertDispatcher] Cannot send daily digest. Digest Chat ID is empty." << std::endl;
        return;
    }

    struct tm local_t = get_zurich_time_now();
    std::stringstream date_ss;
    date_ss << std::setfill('0') << std::setw(4) << (local_t.tm_year + 1900) << "-" 
            << std::setw(2) << (local_t.tm_mon + 1) << "-" << std::setw(2) << local_t.tm_mday;
    std::string today_date = date_ss.str();

    // 1. Get latest market regime
    std::string regime_str = "CHOP";
    auto regime_logs = store_->get_regime_log(1);
    if (!regime_logs.empty()) {
        regime_str = regime_logs[0].regime;
        std::transform(regime_str.begin(), regime_str.end(), regime_str.begin(), ::toupper);
    }

    // 2. Fetch candidates watchlist
    auto cands = store_->get_candidates();
    std::vector<persistence::DbCandidate> active_cands;
    for (const auto& c : cands) {
        if (c.status == "active") {
            active_cands.push_back(c);
        }
    }

    // 3. Fetch interesting signals generated today
    auto alerts = store_->get_alerts(100);
    std::vector<persistence::DbAlert> today_alerts;
    for (const auto& a : alerts) {
        if (a.ts.substr(0, 10) == today_date) {
            today_alerts.push_back(a);
        }
    }

    std::stringstream ss;
    ss << "📅 <b>Daily Digest — " << today_date << "</b>\n"
       << "Market Regime: <b>" << regime_str << "</b>\n\n";

    // Sector rotation summary (we look up sectors from DB bar daily)
    ss << "<b>Sector Rotation (Top Relative Strength):</b>\n";
    
    // Sort sectors by return or rs_percentile from instruments
    auto instruments = store_->get_instruments();
    struct ETFInfo {
        std::string symbol;
        double return_12m = 0.0;
        double price = 0.0;
    };
    std::vector<ETFInfo> etfs;
    for (const auto& inst : instruments) {
        if (inst.asset_class == "ETF") {
            auto bars = store_->get_bars_daily_range(inst.id, "1900-01-01", today_date);
            if (bars.size() >= 253) {
                double close = bars.back().close;
                double prev = bars[bars.size() - 253].close;
                double ret = (close - prev) / prev;
                etfs.push_back({inst.symbol, ret, close});
            }
        }
    }
    std::sort(etfs.begin(), etfs.end(), [](const ETFInfo& a, const ETFInfo& b) {
        return a.return_12m > b.return_12m;
    });

    size_t top_limit = std::min(etfs.size(), (size_t)3);
    for (size_t i = 0; i < top_limit; ++i) {
        ss << "  • #" << (i+1) << " <b>" << etfs[i].symbol << "</b>: 12M Return: " 
           << std::fixed << std::setprecision(1) << (etfs[i].return_12m * 100.0) << "% (Price: $" << etfs[i].price << ")\n";
    }

    ss << "\n<b>New Watchlist Candidates (" << active_cands.size() << " active):</b>\n";
    if (active_cands.empty()) {
        ss << "  <i>No active candidates.</i>\n";
    } else {
        for (const auto& c : active_cands) {
            std::string sym = "UNKNOWN";
            for (const auto& inst : instruments) {
                if (inst.id == c.instrument_id) {
                    sym = inst.symbol;
                    break;
                }
            }
            ss << "  • <b>" << sym << "</b> (Screen " << c.screen << "): Entry zone: " 
               << c.entry_zone_low << " – " << c.entry_zone_high << ", Stop: " << c.suggested_stop << "\n";
        }
    }

    ss << "\n<b>Interesting Signals (Today):</b>\n";
    if (today_alerts.empty()) {
        ss << "  <i>No signals triggered today.</i>\n";
    } else {
        size_t print_limit = std::min(today_alerts.size(), (size_t)8);
        for (size_t i = 0; i < print_limit; ++i) {
            std::string sym = "UNKNOWN";
            for (const auto& inst : instruments) {
                if (inst.id == today_alerts[i].instrument_id) {
                    sym = inst.symbol;
                    break;
                }
            }
            
            try {
                auto p = nlohmann::json::parse(today_alerts[i].payload_json);
                std::string trigger = p.value("trigger", "Signal triggered");
                ss << "  • <b>" << sym << "</b> (" << today_alerts[i].tier << "): " << trigger << "\n";
            } catch (...) {
                ss << "  • <b>" << sym << "</b> (" << today_alerts[i].tier << "): Screen " << today_alerts[i].screen << " signal\n";
            }
        }
        if (today_alerts.size() > 8) {
            ss << "  <i>...and " << (today_alerts.size() - 8) << " more.</i>\n";
        }
    }

    tg_bot_->send_message(chat_id, ss.str());
}

} // namespace core
} // namespace trader
