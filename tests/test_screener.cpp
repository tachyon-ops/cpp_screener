#include "test_harness.hpp"
#include "trader/persistence/sqlite_store.hpp"
#include "trader/core/regime_classifier.hpp"
#include "trader/screens/screen_b.hpp"
#include "trader/screens/screen_c.hpp"
#include "trader/screens/screen_g.hpp"
#include "trader/core/sr_provider.hpp"
#include "trader/core/position_manager.hpp"
#include "trader/storage/time_series_store.hpp"
#include "trader/core/alert_dispatcher.hpp"
#include <memory>
#include <vector>
#include <cmath>
#include <iostream>

using namespace trader;
using namespace trader::core;
using namespace trader::persistence;
using namespace trader::screens;
using namespace trader::storage;

namespace trader {
namespace core {
inline std::ostream& operator<<(std::ostream& os, Regime r) {
    os << regime_to_string(r);
    return os;
}
}
}

// Mock Support/Resistance Provider
class MockSRProvider : public ISupportResistanceProvider {
public:
    std::vector<SRLevel> levels;
    std::vector<SRLevel> get_levels(const std::string& symbol) override {
        return levels;
    }
};

TEST_CASE(regime_classifier_hysteresis) {
    // 1. Setup in-memory store
    auto store = std::make_shared<SQLiteStore>(":memory:");
    store->init_schema();

    // 2. Initialize classifier
    RegimeClassifier classifier(store);
    
    // Default regime is Chop
    ASSERT_EQ(classifier.current_regime(), Regime::Chop);

    // Setup SPY in database and write daily bars to get positive trend score.
    DbInstrument spy;
    spy.symbol = "SPY";
    spy.asset_class = "Stock";
    store->add_instrument(spy);
    
    auto opt_spy = store->get_instrument_by_symbol("SPY");
    ASSERT_TRUE(opt_spy.has_value());
    int64_t spy_id = opt_spy->id;
    
    // Add 250 bars where SPY is increasing, so MA is trending up and close is above MA
    for (int i = 0; i < 250; ++i) {
        DbBarDaily b;
        b.instrument_id = spy_id;
        // Construct standard dates
        int year = 2025;
        int month = 1 + (i / 20);
        int day = 1 + (i % 20);
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", year, month, day);
        b.date = date_buf;
        b.open = 100.0 + i * 0.5;
        b.high = 101.0 + i * 0.5;
        b.low = 99.0 + i * 0.5;
        b.close = 100.0 + i * 0.5;
        b.volume = 1000000;
        store->add_bar_daily(b);
    }
    
    // Day 1: Bull score calculated, but hysteresis requires 2 consecutive days.
    classifier.evaluate("2026-05-01", 12.0, 3.0, 0.8, 224.5);
    ASSERT_EQ(classifier.current_regime(), Regime::Chop); // remains Chop (1st day candidate)

    // Day 2: Second consecutive Bull score.
    classifier.evaluate("2026-05-02", 12.0, 3.0, 0.8, 225.0);
    ASSERT_EQ(classifier.current_regime(), Regime::Bull); // transitions to Bull!
}

TEST_CASE(regime_classifier_crisis_override) {
    auto store = std::make_shared<SQLiteStore>(":memory:");
    store->init_schema();
    RegimeClassifier classifier(store);
    classifier.set_current_regime(Regime::Bull);

    // Crisis override via VIX >= 35.0 (immediate transition)
    classifier.evaluate("2026-05-01", 36.0, 3.0, 0.5, 200.0);
    ASSERT_EQ(classifier.current_regime(), Regime::Crisis);
    
    // Reset to Bull
    classifier.set_current_regime(Regime::Bull);
    
    // Crisis override via HY OAS jump >= 100 bps
    DbRegimeLog prior;
    prior.ts = "2026-05-01T16:30:00Z";
    prior.regime = "bull";
    prior.hy_oas = 3.0;
    prior.vix = 15.0;
    store->add_regime_log(prior);
    
    // Evaluate with HY OAS = 4.1 (410 bps), jump of 110 bps
    classifier.evaluate("2026-05-02", 20.0, 4.1, 0.5, 200.0);
    ASSERT_EQ(classifier.current_regime(), Regime::Crisis);
}

namespace {
std::string test_subtract_days(const std::string& date_str, int days) {
    struct tm t = {};
    if (sscanf(date_str.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        t.tm_mday -= days;
        mktime(&t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
        return buf;
    }
    return date_str;
}

bool test_is_weekend(const std::string& date_str) {
    struct tm t = {};
    if (sscanf(date_str.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        mktime(&t);
        return (t.tm_wday == 0 || t.tm_wday == 6);
    }
    return false;
}

std::vector<std::string> test_get_trading_days_back(const std::string& start_date, int count) {
    std::vector<std::string> dates;
    int days_offset = 0;
    while (dates.size() < (size_t)count) {
        std::string d = test_subtract_days(start_date, days_offset);
        if (!test_is_weekend(d)) {
            dates.push_back(d);
        }
        days_offset++;
    }
    std::reverse(dates.begin(), dates.end());
    return dates;
}
}

TEST_CASE(screen_b_swing_pullback) {
    auto store = std::make_shared<SQLiteStore>(":memory:");
    store->init_schema();

    // Setup a stock instrument
    DbInstrument stock;
    stock.symbol = "AAPL";
    stock.asset_class = "Stock";
    stock.exchange = "NASDAQ";
    stock.metadata_json = "{\"sector_etf_symbol\":\"SPY\"}";
    store->add_instrument(stock);

    auto opt_aapl = store->get_instrument_by_symbol("AAPL");
    ASSERT_TRUE(opt_aapl.has_value());

    // Seeding deterministic daily bars for stock AAPL
    std::vector<std::string> dates = test_get_trading_days_back("2026-05-22", 265);
    double price = 100.0;
    for (int i = 0; i < 250; ++i) {
        double ret = (i % 2 == 0) ? 0.012 : -0.004;
        price = price * (1.0 + ret);

        DbBarDaily bar;
        bar.instrument_id = opt_aapl->id;
        bar.date = dates[i];
        bar.open = price * 0.998;
        bar.high = price * 1.005;
        bar.low = price * 0.994;
        bar.close = price;
        bar.volume = 1500000;
        store->add_bar_daily(bar);
    }

    for (int i = 250; i < 265; ++i) {
        double ret = (i % 2 == 0) ? 0.004 : -0.009;
        price = price * (1.0 + ret);

        DbBarDaily bar;
        bar.instrument_id = opt_aapl->id;
        bar.date = dates[i];
        bar.open = price * 0.998;
        bar.high = price * 1.005;
        bar.low = price * 0.994;
        bar.close = price;
        bar.volume = 500000; // vol contracting
        store->add_bar_daily(bar);
    }

    ScreenB screen_b(store);
    
    // Run evaluate with pre-seeded history
    screen_b.evaluate("2026-05-22");
    
    // Verify that daily bars were seeded
    auto bars = store->get_bars_daily(opt_aapl->id);
    ASSERT_TRUE(bars.size() >= 253);

    // Verify results and that candidate was added
    auto results = screen_b.get_results();
    ASSERT_TRUE(!results.empty());
    
    auto candidates = store->get_candidates();
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].screen, "B");
    ASSERT_EQ(candidates[0].status, "active");
}

TEST_CASE(screen_g_cross_asset_divergence) {
    auto store = std::make_shared<SQLiteStore>(":memory:");
    store->init_schema();

    // Create Divergent pair instruments in DB
    DbInstrument btc;
    btc.symbol = "BTCUSD";
    btc.asset_class = "crypto";
    store->add_instrument(btc);
    
    DbInstrument ndx;
    ndx.symbol = "NDX";
    ndx.asset_class = "Stock";
    store->add_instrument(ndx);
    
    auto opt_btc = store->get_instrument_by_symbol("BTCUSD");
    auto opt_ndx = store->get_instrument_by_symbol("NDX");
    
    // Add 100 days of history
    // First 99 days: BTC and NDX returns are identical (perfect correlation, zero spread stddev)
    // We make them fluctuate using a sine wave so Pearson correlation is robust against the last-day divergence.
    double price = 100.0;
    {
        std::string dt = "2025-01-01";
        DbBarDaily b_btc;
        b_btc.instrument_id = opt_btc->id;
        b_btc.date = dt;
        b_btc.close = price;
        store->add_bar_daily(b_btc);
        
        DbBarDaily b_ndx;
        b_ndx.instrument_id = opt_ndx->id;
        b_ndx.date = dt;
        b_ndx.close = price;
        store->add_bar_daily(b_ndx);
    }

    for (int i = 1; i < 99; ++i) {
        int year = 2025;
        int month = 1 + (i / 20);
        int day = 1 + (i % 20);
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", year, month, day);
        std::string dt = date_buf;
        
        double return_pct = 0.02 * std::sin(i * 0.5);
        price = price * (1.0 + return_pct);
        
        DbBarDaily b_btc;
        b_btc.instrument_id = opt_btc->id;
        b_btc.date = dt;
        b_btc.close = price;
        store->add_bar_daily(b_btc);
        
        DbBarDaily b_ndx;
        b_ndx.instrument_id = opt_ndx->id;
        b_ndx.date = dt;
        b_ndx.close = price;
        store->add_bar_daily(b_ndx);
    }
    
    // Day 100: Divergence!
    // NDX goes up, BTC goes down slightly, creating a spread but preserving high correlation history.
    std::string dt_100 = "2025-06-01";
    
    DbBarDaily b_btc_last;
    b_btc_last.instrument_id = opt_btc->id;
    b_btc_last.date = dt_100;
    b_btc_last.close = price * 0.9995; // down 0.05%
    store->add_bar_daily(b_btc_last);
    
    DbBarDaily b_ndx_last;
    b_ndx_last.instrument_id = opt_ndx->id;
    b_ndx_last.date = dt_100;
    b_ndx_last.close = price * 1.0005; // up 0.05%
    store->add_bar_daily(b_ndx_last);

    ScreenG screen_g(store, nullptr);
    screen_g.evaluate(dt_100);
    
    auto results = screen_g.get_results();
    ASSERT_TRUE(!results.empty());
    
    // Find NDX vs BTCUSD result
    bool found = false;
    for (const auto& res : results) {
        if (res.symbol_a == "BTCUSD" && res.symbol_b == "NDX") {
            found = true;
            ASSERT_TRUE(res.is_diverged);
            ASSERT_TRUE(res.pearson_corr > 0.9); // high correlation prior to divergence
            ASSERT_TRUE(res.spread_zscore < -2.0);
        }
    }
    ASSERT_TRUE(found);
}

TEST_CASE(screen_c_capitulation_wick_reversal) {
    auto store = std::make_shared<SQLiteStore>(":memory:");
    store->init_schema();

    DbInstrument btc;
    btc.symbol = "BTCUSD";
    btc.asset_class = "crypto";
    store->add_instrument(btc);
    
    auto ts_store = std::make_shared<TimeSeriesStore>();
    auto mock_sr = std::make_shared<MockSRProvider>();
    mock_sr->levels = {{92.0, "swing", 3, 0}};

    // Setup Screen G
    auto screen_g = std::make_shared<ScreenG>(store, ts_store);
    
    ScreenC screen_c(store, ts_store, mock_sr, screen_g);
    
    // Add symbol to watch list manually
    screen_c.add_to_watch("BTCUSD", 72);
    ASSERT_TRUE(screen_c.is_watching("BTCUSD"));

    // Populate TimeSeries for BTCUSD on H4 timeframe (needs >= 21 bars)
    auto ts = ts_store->get_or_create("BTCUSD");
    for (int i = 0; i < 25; ++i) {
        Bar b;
        b.ts = { static_cast<uint64_t>(1716300000000 + i * 4 * 3600 * 1000) };
        b.open = { 100.0 };
        b.high = { i == 24 ? 100.0 : 101.0 };
        b.low = { i == 24 ? 90.0 : 99.0 }; // low 90.0 pierce support 92.0
        b.close = { i == 24 ? 99.0 : 100.0 }; // close 99.0 is in upper third
        b.volume = { i == 24 ? 3000.0 : 1000.0 }; // 3x volume surge
        ts->append_bar(b, Resolution::H4);
    }
    
    // Check symbol
    screen_c.check_symbol("BTCUSD", "2026-05-22");
    
    // Check results and candidate creation
    auto results = screen_c.get_results();
    ASSERT_TRUE(!results.empty());
    ASSERT_EQ(results[0].symbol, "BTCUSD");
    ASSERT_EQ(results[0].pierced_support, 92.0);
    ASSERT_NEAR(results[0].wick_size_pct, 0.90, 0.01);
    
    auto candidates = store->get_candidates();
    ASSERT_TRUE(!candidates.empty());
    ASSERT_EQ(candidates[0].screen, "C");
    ASSERT_EQ(candidates[0].status, "active");
}

TEST_CASE(sr_provider_swing_and_composite) {
    // 1. Test Swing points extraction in AlgorithmicSRProvider
    auto ts_store = std::make_shared<TimeSeriesStore>();
    auto ts = ts_store->get_or_create("AAPL");
    
    // Create daily bars with a swing high on day 7 (index 7)
    for (int i = 0; i < 15; ++i) {
        Bar b;
        b.ts = { static_cast<uint64_t>(1716300000000 + i * 24 * 3600 * 1000) };
        b.open = { 100.0 };
        b.high = { i == 7 ? 120.0 : 105.0 };
        b.low = { i == 7 ? 95.0 : 98.0 };
        b.close = { 100.0 };
        b.volume = { 1000.0 };
        ts->append_bar(b, Resolution::D1);
    }
    // Apple latest price
    Tick t;
    t.ts = { 1716300000000 + 16 * 24 * 3600 * 1000 };
    t.bid = { 100.0 };
    t.ask = { 100.0 };
    ts->append_tick(t);

    AlgorithmicSRProvider algo_provider(ts_store);
    auto levels = algo_provider.get_levels("AAPL");
    
    // Verify that the swing high at 120.0 was extracted
    bool found_swing_high = false;
    for (const auto& lvl : levels) {
        if (std::abs(lvl.price - 120.0) < 0.01) {
            found_swing_high = true;
            break;
        }
    }
    ASSERT_TRUE(found_swing_high);

    // 2. Test Composite merging
    auto prov1 = std::make_shared<MockSRProvider>();
    prov1->levels = {{100.0, "swing", 2, 0}};
    
    auto prov2 = std::make_shared<MockSRProvider>();
    prov2->levels = {{100.5, "ma", 5, 0}};
    
    CompositeSRProvider composite({prov1, prov2}, 0.01); // 1% proximity
    auto comp_levels = composite.get_levels("AAPL");
    
    ASSERT_EQ(comp_levels.size(), 1);
    ASSERT_NEAR(comp_levels[0].price, 100.5, 0.01);
    ASSERT_EQ(comp_levels[0].touch_count, 5);
}

TEST_CASE(position_manager_stop_migrations_and_exits) {
    auto store = std::make_shared<SQLiteStore>(":memory:");
    store->init_schema();
    
    auto ts_store = std::make_shared<TimeSeriesStore>();
    auto mock_sr = std::make_shared<MockSRProvider>();

    PositionManager pm(store, ts_store, mock_sr);

    // AAPL stock instrument
    DbInstrument inst;
    inst.symbol = "AAPL";
    inst.asset_class = "Stock";
    store->add_instrument(inst);
    auto opt_inst = store->get_instrument_by_symbol("AAPL");

    // Add open long position
    DbPosition pos;
    pos.instrument_id = opt_inst->id;
    pos.direction = "long";
    pos.entry_price = 100.0;
    pos.initial_stop = 90.0;
    pos.current_stop = 90.0;
    pos.size = 1.0;
    pos.status = "open";
    int64_t pos_id = store->add_position(pos);

    // 1. Test BE Migration at +0.3R
    auto ts = ts_store->get_or_create("AAPL");
    Tick t;
    t.ts = { 1000 };
    t.bid = { 104.0 };
    t.ask = { 104.0 };
    ts->append_tick(t);

    pm.check_positions();

    auto positions = store->get_positions();
    ASSERT_EQ(positions.size(), 1);
    ASSERT_NEAR(positions[0].current_stop, 100.0, 0.01); // migrated to BE
    ASSERT_EQ(positions[0].status, "open");

    // 2. Test Trailing Stop Update below support level
    mock_sr->levels = {{102.0, "swing", 1, 0}};
    
    pm.check_positions();

    positions = store->get_positions();
    ASSERT_NEAR(positions[0].current_stop, 101.49, 0.01); // Trailed to support * 0.995

    // 3. Test Stop-Breach Exit (Winner exit since R > 0)
    ts = ts_store->get_or_create("AAPL");
    t.ts = { 2000 };
    t.bid = { 101.0 };
    t.ask = { 101.0 };
    ts->append_tick(t);

    pm.check_positions();

    positions = store->get_positions();
    ASSERT_EQ(positions[0].status, "closed_winner");
    ASSERT_NEAR(positions[0].exit_price, 101.0, 0.01);
    ASSERT_NEAR(positions[0].r_realized, 0.1, 0.01);
}
