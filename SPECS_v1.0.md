# Market Screener Engine — System Specification

**Version:** 0.1 (draft)
**Owner:** Nuno
**Last updated:** 2026-05-21

---

## 1. System Overview

### 1.1 Purpose

A multi-instrument, multi-asset-class market screener and alert engine that:

- Runs continuously on a small cloud VM
- Pulls live and historical data from Saxo (primary) and free supplementary sources
- Evaluates a set of independent screen modules against a wide instrument universe
- Classifies market regime and gates screens by regime
- Emits tiered alerts via Telegram during US session hours
- Exposes a web UI for afternoon prep and end-of-day review
- Persists trade candidates, alerts, and screen state in SQLite

### 1.2 User Constraints (drive the design)

- User works a day job; live discretionary monitoring is impossible
- US session is 15:30–22:00 CET (CEST: 15:30; CET: 16:30); user reviews around 15:00–16:30 CET
- Alert volume target: 5–8 per day across all tiers combined
- Cost target: minimize ongoing fees beyond broker relationship and necessary historical data
- Language: C++17 or C++20, modelled on user's existing NodeJS SaxoBrokerAdapter pattern
- Hosting: small cloud VM (e.g., DigitalOcean $5–10/mo droplet)

### 1.3 Trading Philosophy Encoded

The system is designed around the principle that:

- **Edge comes from regime-appropriate screens, not universal signals.** Mean reversion is profitable in chop and toxic in trends; momentum continuation is profitable in trends and bleeds in chop.
- **Asymmetric R:R via tight stops at structural levels.** Position sizing scales with stop tightness, allowing large allocations with small account risk.
- **Confluence over single signals.** Premium alerts require multiple independent filters to fire simultaneously.
- **Mechanical execution over discretionary.** The system enforces the discipline a human at work cannot maintain.

---

## 2. Architecture

### 2.1 High-Level Components

```
┌──────────────────────────────────────────────────────────────┐
│                    Cloud VM (Linux, ~2GB RAM)                │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │              trader-engine (C++ binary)                │  │
│  │                                                        │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐      │  │
│  │  │   Data   │  │  Regime  │  │     Screens      │      │  │
│  │  │  Layer   │──▶│Classifier│──▶│  (A, B, C, ...) │      │  │
│  │  └──────────┘  └──────────┘  └────────┬─────────┘      │  │
│  │       ▲                                │                │  │
│  │       │                                ▼                │  │
│  │  ┌──────────┐                  ┌──────────────┐         │  │
│  │  │  Broker  │                  │    Alert     │         │  │
│  │  │ Adapters │                  │  Dispatcher  │         │  │
│  │  └──────────┘                  └──────┬───────┘         │  │
│  │       ▲                               │                 │  │
│  │       │                               ▼                 │  │
│  │  ┌──────────┐                  ┌──────────────┐         │  │
│  │  │ SQLite   │                  │   Telegram   │         │  │
│  │  │  Store   │                  │   Bot API    │         │  │
│  │  └──────────┘                  └──────────────┘         │  │
│  │                                                        │  │
│  │  ┌────────────────────────────────────────────────────┐│  │
│  │  │   Embedded HTTP/WS Server (Crow or Drogon)         ││  │
│  │  │   /api/...   /ws/live                              ││  │
│  │  └────────────────────────────────────────────────────┘│  │
│  └────────────────────────────────────────────────────────┘  │
│                              ▲                               │
└──────────────────────────────│───────────────────────────────┘
                               │ HTTPS/WSS
                  ┌────────────┴────────────┐
                  │                         │
            Browser (Web UI)          Telegram App
            (any device)            (phone, push alerts)
```

### 2.2 Process Model

Single C++ binary, multi-threaded:

| Thread               | Responsibility                                 | Lifecycle        |
| -------------------- | ---------------------------------------------- | ---------------- |
| `main`               | Startup, shutdown, signal handling             | Process lifetime |
| `data_ingest_saxo`   | Saxo WebSocket streaming, REST polling         | Long-lived       |
| `data_ingest_crypto` | Binance/Kraken public WS for crypto            | Long-lived       |
| `data_ingest_macro`  | Daily polling of FRED, Cboe, free macro feeds  | Cron-style       |
| `screen_eod`         | EOD screens (B, D, E, F) — runs after close    | Triggered        |
| `screen_live`        | Live screens (A, C, G) — evaluates on tick/bar | Long-lived       |
| `regime_evaluator`   | Recomputes regime on data updates              | Long-lived       |
| `alert_dispatcher`   | Drains alert queue, posts to Telegram          | Long-lived       |
| `http_server`        | Serves web UI and API                          | Long-lived       |
| `persistence_writer` | Async writes to SQLite                         | Long-lived       |

Inter-thread communication via lock-free queues (`moodycamel::ConcurrentQueue`) for hot paths, `std::mutex`-protected shared state for slow paths.

### 2.3 Project Layout

```
trader-engine/
├── CMakeLists.txt
├── README.md
├── config/
│   ├── config.yaml          # Engine config (ports, thresholds, etc.)
│   ├── universe.yaml        # Instruments to watch per asset class
│   ├── screens.yaml         # Per-screen parameters
│   └── secrets.env          # API keys (gitignored)
├── include/trader/
│   ├── core/
│   │   ├── types.hpp        # Common types (Price, Quantity, Timestamp)
│   │   ├── time.hpp         # Time zones, session calendars
│   │   ├── instrument.hpp   # Instrument + AssetClass enum
│   │   └── result.hpp       # Result<T, E> for error handling
│   ├── data/
│   │   ├── feed.hpp         # Feed interface
│   │   ├── store.hpp        # In-memory time-series store
│   │   ├── timeseries.hpp   # Circular buffer for bars/ticks
│   │   └── bar.hpp          # OHLCV bar struct
│   ├── broker/
│   │   ├── broker_adapter.hpp     # Abstract broker interface
│   │   ├── saxo_adapter.hpp       # Saxo OpenAPI implementation
│   │   ├── binance_adapter.hpp    # Crypto data only (no orders)
│   │   └── free_data_adapter.hpp  # FRED, Yahoo, Cboe scrapers
│   ├── regime/
│   │   ├── regime.hpp             # Regime enum + state
│   │   └── classifier.hpp         # RegimeClassifier
│   ├── screens/
│   │   ├── screen.hpp             # Base Screen interface
│   │   ├── screen_b_pullback.hpp
│   │   ├── screen_d_rotation.hpp
│   │   ├── screen_a_dislocation.hpp
│   │   ├── screen_e_cef.hpp
│   │   ├── screen_f_darvas.hpp
│   │   ├── screen_g_divergence.hpp
│   │   └── screen_c_capitulation.hpp
│   ├── alerts/
│   │   ├── alert.hpp              # Alert struct + tier enum
│   │   ├── dispatcher.hpp         # Alert queue + routing
│   │   └── telegram.hpp           # Telegram Bot API client
│   ├── web/
│   │   ├── http_server.hpp        # Crow/Drogon-based REST + WS
│   │   └── api_handlers.hpp
│   ├── persistence/
│   │   ├── sqlite_store.hpp
│   │   └── schema.hpp
│   └── util/
│       ├── http_client.hpp        # Wraps cpp-httplib/libcurl
│       ├── json.hpp               # Wraps nlohmann::json
│       └── logging.hpp            # Wraps spdlog
├── src/                           # Mirrors include/ structure
├── ui/
│   ├── index.html
│   ├── dashboard.js               # Vanilla JS, no framework needed
│   └── style.css
├── scripts/
│   ├── backfill_history.cpp       # One-shot historical data import
│   └── run_backtest.cpp           # Replay screens against history
├── tests/
│   ├── unit/                      # GoogleTest
│   └── replay/                    # Backtest scenarios
└── docs/
    └── (this spec, design notes)
```

### 2.4 Dependencies

| Library                                                                                        | Purpose                             | License       |
| ---------------------------------------------------------------------------------------------- | ----------------------------------- | ------------- |
| [Crow](https://github.com/CrowCpp/Crow) or [Drogon](https://github.com/drogonframework/drogon) | HTTP/WS server                      | BSD/MIT       |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib)                                          | HTTP client (header-only)           | MIT           |
| [nlohmann/json](https://github.com/nlohmann/json)                                              | JSON parsing                        | MIT           |
| [spdlog](https://github.com/gabime/spdlog)                                                     | Logging                             | MIT           |
| [SQLite3](https://sqlite.org/)                                                                 | Persistent store                    | Public domain |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp)                                                 | Config                              | MIT           |
| [concurrentqueue](https://github.com/cameron314/concurrentqueue)                               | Lock-free queue                     | BSD           |
| [date](https://github.com/HowardHinnant/date)                                                  | Time zones (if not on C++20 chrono) | MIT           |
| [GoogleTest](https://github.com/google/googletest)                                             | Unit tests                          | BSD           |

No Boost dependency intentionally — keeps build simple.

### 2.5 Build & Deployment

- CMake build, `mkdir build && cmake .. && make -j`
- Single statically-linked binary preferred for VM deploy
- systemd service for auto-restart
- Config and secrets mounted from VM filesystem, not baked in
- Logs to `/var/log/trader-engine/` with rotation via spdlog

---

## 3. Data Layer

### 3.1 Asset Classes & Sources

| Asset Class                     | Primary Source              | Backup/Free          | Live?                |
| ------------------------------- | --------------------------- | -------------------- | -------------------- |
| US equities (large/mid)         | Saxo streaming              | Tiingo EOD free tier | Live L1              |
| US equities (full universe)     | Saxo + EODHD                | EODHD cheap tier     | EOD                  |
| Sector/Industry ETFs            | Saxo                        | Yahoo Finance scrape | Live L1              |
| Closed-end funds (NAV)          | EODHD or CEFConnect scrape  | —                    | EOD                  |
| Crypto majors + alts            | Binance/Kraken public WS    | —                    | Live                 |
| FX majors                       | Saxo                        | —                    | Live                 |
| Commodities (silver, gold, oil) | Saxo                        | —                    | Live                 |
| VIX, VIX term structure         | Saxo or Cboe free delayed   | —                    | EOD (15min delay OK) |
| Credit spreads (HY OAS)         | FRED daily                  | —                    | EOD                  |
| Macro / breadth                 | FRED + custom daily scrapes | —                    | EOD                  |

### 3.2 In-Memory Store

`TimeSeriesStore` holds per-instrument circular buffers:

```cpp
struct Bar {
    Timestamp ts;        // UTC nanoseconds
    Price open, high, low, close;
    Quantity volume;
};

class TimeSeries {
    CircularBuffer<Bar> daily_;     // 500 bars (~2 years)
    CircularBuffer<Bar> hourly_;    // 1000 bars (~6 weeks)
    CircularBuffer<Bar> min5_;      // 2000 bars (~2 weeks)
    CircularBuffer<Tick> ticks_;    // Last N ticks only, optional

    mutable std::shared_mutex mu_;  // RW lock, reads dominate
public:
    void append_tick(Tick t);
    void append_bar(Bar b, Resolution r);
    std::vector<Bar> last_n(Resolution r, size_t n) const;
    Bar latest(Resolution r) const;
    // ...
};

class TimeSeriesStore {
    std::unordered_map<InstrumentId, std::unique_ptr<TimeSeries>> series_;
    mutable std::shared_mutex mu_;
public:
    TimeSeries& get_or_create(const Instrument& inst);
    const TimeSeries* get(InstrumentId id) const;
};
```

**Memory budget:** for 3,000 instruments × (500 daily + 1000 hourly + 2000 5min bars) ≈ 3,000 × 3,500 × ~40 bytes = ~420 MB. Comfortable on a 2GB VM.

### 3.3 Persistence (SQLite)

Schema (see `persistence/schema.hpp`):

```sql
CREATE TABLE instruments (
    id INTEGER PRIMARY KEY,
    symbol TEXT NOT NULL,
    asset_class TEXT NOT NULL,
    exchange TEXT,
    saxo_uic INTEGER,
    metadata_json TEXT
);

CREATE TABLE bars_daily (
    instrument_id INTEGER NOT NULL,
    date TEXT NOT NULL,            -- ISO 8601 YYYY-MM-DD
    open REAL, high REAL, low REAL, close REAL,
    volume REAL,
    PRIMARY KEY (instrument_id, date),
    FOREIGN KEY (instrument_id) REFERENCES instruments(id)
);

CREATE TABLE alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,              -- ISO 8601 UTC
    screen TEXT NOT NULL,          -- 'A','B','C','D','E','F','G'
    instrument_id INTEGER,
    tier TEXT NOT NULL,            -- 'premium','opportunity','interesting'
    payload_json TEXT NOT NULL,
    regime_at_alert TEXT,
    acted_on INTEGER DEFAULT 0,    -- user button press
    FOREIGN KEY (instrument_id) REFERENCES instruments(id)
);

CREATE TABLE regime_log (
    ts TEXT PRIMARY KEY,
    regime TEXT NOT NULL,          -- 'bull','chop','stress','crisis'
    vix REAL,
    breadth REAL,
    hy_oas REAL,
    spx_vs_200ma REAL,
    detail_json TEXT
);

CREATE TABLE candidates (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_ts TEXT NOT NULL,
    screen TEXT NOT NULL,
    instrument_id INTEGER,
    entry_zone_low REAL, entry_zone_high REAL,
    suggested_stop REAL,
    rr_target REAL,
    notes TEXT,
    status TEXT DEFAULT 'active',  -- 'active','triggered','expired','cancelled'
    FOREIGN KEY (instrument_id) REFERENCES instruments(id)
);

CREATE INDEX idx_alerts_ts ON alerts(ts);
CREATE INDEX idx_alerts_screen ON alerts(screen);
CREATE INDEX idx_bars_daily_date ON bars_daily(date);
```

WAL mode enabled; writes are batched and committed every ~1s by `persistence_writer` thread.

### 3.4 Broker Adapter Interface

Modelled on Nuno's NodeJS SaxoBrokerAdapter:

```cpp
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

    // Account / orders (Phase 9+)
    virtual Result<AccountInfo> get_account() = 0;
    virtual Result<OrderId> place_order(const OrderRequest& req) = 0;
    virtual Result<void> cancel_order(OrderId id) = 0;
};

class SaxoBrokerAdapter : public BrokerAdapter { /* ... */ };
class BinanceMarketDataAdapter : public BrokerAdapter { /* market data only */ };
```

---

## 4. Regime Classifier

### 4.1 Regime States

```cpp
enum class Regime {
    Bull,    // trending up, low volatility
    Chop,    // range-bound, mean-reversion friendly
    Stress,  // elevated vol, broken trend
    Crisis   // capitulation conditions, divergence opportunities
};
```

### 4.2 Inputs

Computed on each EOD close:

| Input                                   | Source                     | Update freq     |
| --------------------------------------- | -------------------------- | --------------- |
| SPX close vs 200-day MA                 | Saxo or free EOD           | Daily           |
| SPX 200-day MA slope (last 20 sessions) | Computed                   | Daily           |
| % S&P 500 above 200-day MA (breadth)    | Computed from constituents | Daily           |
| VIX close + 5-day average               | Cboe free / Saxo           | Daily (live OK) |
| VIX9D/VIX ratio (term structure)        | Cboe free                  | Daily           |
| HY OAS (BofA HY index spread)           | FRED `BAMLH0A0HYM2`        | Daily           |
| Cumulative advance/decline (NYSE)       | EODHD or scrape            | Daily           |
| New 52-week highs minus lows            | Computed                   | Daily           |

### 4.3 Classification Logic

Scoring approach: each input contributes a score; final regime is determined by thresholding.

```
trend_score:
    +2 if SPX > 200MA AND slope > 0
    +1 if SPX > 200MA AND slope <= 0
    -1 if SPX < 200MA AND slope >= 0
    -2 if SPX < 200MA AND slope < 0

breadth_score:
    +2 if pct_above_200ma > 0.70
    +1 if 0.50 < pct_above_200ma <= 0.70
     0 if 0.30 < pct_above_200ma <= 0.50
    -2 if pct_above_200ma <= 0.30

vol_score:
    +1 if VIX < 15
     0 if 15 <= VIX < 20
    -1 if 20 <= VIX < 25
    -2 if 25 <= VIX < 35
    -3 if VIX >= 35

credit_score:
    +1 if HY_OAS < 350
     0 if 350 <= HY_OAS < 450
    -1 if 450 <= HY_OAS < 600
    -2 if HY_OAS >= 600

total = trend_score + breadth_score + vol_score + credit_score

if total >= 5:           Regime::Bull
elif total >= 1:         Regime::Chop      (also default during transitions)
elif total >= -3:        Regime::Stress
else:                    Regime::Crisis
```

Thresholds calibrated against 2008, 2018-Q4, 2020-Mar, 2022, August 2024. Refine via backtest.

### 4.4 Regime Hysteresis

To avoid flipping on a single noisy day, a regime change requires the score to remain in the new range for 2 consecutive sessions before the regime label updates. Exception: crisis → can be triggered intraday if VIX gaps above 35 or HY OAS jumps >100bps in a day (override flag).

### 4.5 Screen Enablement Matrix

| Screen                | Bull | Chop    | Stress | Crisis |
| --------------------- | ---- | ------- | ------ | ------ |
| A — Intraday MR       | ON   | ON      | OFF    | OFF    |
| B — Swing pullback    | ON   | LIMITED | OFF    | OFF    |
| C — Capitulation wick | OFF  | OFF     | ON     | ON     |
| D — Industry rotation | ON   | ON      | ON     | ON     |
| E — CEF discounts     | ON   | ON      | ON     | ON     |
| F — Darvas box        | ON   | ON      | OFF    | OFF    |
| G — Divergence        | ON   | ON      | ON     | ON     |

LIMITED = runs but only emits Interesting-tier; Premium suppressed.

---

## 5. Screen Specifications

Each screen implements:

```cpp
class Screen {
public:
    virtual ~Screen() = default;
    virtual std::string id() const = 0;
    virtual ScreenType type() const = 0;   // EOD or Live
    virtual Regime min_regime() const = 0; // Lowest regime to be enabled in
    virtual std::vector<Alert> evaluate(
        const TimeSeriesStore& store,
        const RegimeState& regime,
        const Universe& universe) = 0;
};
```

Screens are detailed in implementation order (simple → complex).

---

### 5.1 Screen D — Industry Rotation Board

**Status:** Phase 2 (first to implement after engine skeleton + regime classifier)
**Type:** EOD
**Universe:** ~70 industry/sector ETFs (e.g., XLK, XLF, XLE, SMH, KRE, XBI, IBB, ITB, XHB, etc.)

**Logic:**

For each ETF:

- Compute close vs 200-day MA (% above/below)
- Compute close vs 50-day MA
- Compute 1-month return, 3-month return, 6-month return
- Compute relative strength vs SPY (12-month return rank)
- Detect cross events: 50-day crossed 200-day in last 5 sessions
- Detect MA tests: price within 1% of 200-day or 50-day MA

**Output:**

- Daily heatmap, served at `/api/sector_rotation`, rendered in UI
- No alerts by default (contextual screen)
- Optional alert: when a sector ETF flips above/below 200-day MA → Interesting-tier

**Implementation notes:**

- Pure EOD batch job
- Output is a static page refreshed after US close
- Use as a filter input for Screen B (only buy pullbacks in sectors above their 200-day MA)

---

### 5.2 Screen B — Swing Pullback in Uptrend

**Status:** Phase 3
**Type:** EOD
**Universe:** US equities filtered by Minervini Trend Template (see below)

**Pre-filter (Minervini Trend Template):**
A stock passes if all hold:

1. Price > 150-day MA AND price > 200-day MA
2. 150-day MA > 200-day MA
3. 200-day MA trending up for ≥ 20 sessions
4. 50-day MA > 150-day MA AND 50-day MA > 200-day MA
5. Price > 50-day MA
6. Price ≥ 30% above 52-week low
7. Price within 25% of 52-week high
8. Relative Strength rank (12-month return percentile) ≥ 70

**Setup detection:**
Among passing stocks, identify those that have:

- Pulled back to within 2% of 20-day MA, or to a prior swing high (now support)
- Daily RSI(14) between 30 and 50
- Volume on pullback contracting (5-day avg vol < 20-day avg vol)
- Sector ETF above its 200-day MA (uses Screen D output)
- No earnings within next 5 trading days (avoid binary risk)

**Trigger / output:**

- Candidate published to `candidates` table with entry zone, suggested stop (below pullback low or recent swing low), R:R target ≥ 3.
- Alert tier:
  - Premium: all 8 trend filters pass + RSI < 40 + pullback to actual 20-day MA + sector ETF in top 3 by 3-month return
  - Opportunity: trend filters pass + RSI 30-50 + reasonable pullback
  - Interesting: marginal passes

**Implementation notes:**

- Runs once after US close
- Outputs go into the "swing watchlist" displayed in UI
- User reviews next afternoon, decides which to act on
- Candidates expire after 5 sessions if not triggered (price moves away or breaks support)

---

### 5.3 Screen A — Intraday Mean Reversion (BNF-style)

**Status:** Phase 5
**Type:** Live
**Universe:** Top 1000 US stocks by 20-day average dollar volume

**Setup gate (evaluated pre-market):**

- Stock gaps down (or up, for shorts in later versions) > 5% on identifiable news
- News not existential (no fraud, no bankruptcy, no major guidance withdrawal — heuristic via news classifier or curated keyword block list)
- Asset is in liquid universe

**Live trigger:**
Evaluated every minute during US session for each gap-down candidate:

1. Compute 20-period intraday VWAP and standard deviation of price from VWAP
2. Trigger when price is < VWAP - 2.5σ
3. Volume in last 5 minutes > 2× 20-day average for that 5-min slot
4. Stock's sector ETF not simultaneously breaking down (sector ETF intraday change > -1.5%)
5. Stock's 1-month relative strength still > 0

**Alert tiers:**

- Premium: all 5 conditions + deviation > 3σ + volume > 3× + news classified as non-existential
- Opportunity: 4 of 5 conditions
- Interesting: 3 of 5

**Cool-down:**
After firing on a given symbol, suppress further alerts on that symbol for 60 minutes regardless of conditions.

**Implementation notes:**

- The hard work is the intraday VWAP and rolling-stddev maintenance per symbol
- Use 1-minute bars from Saxo streaming, aggregate to internal 5-min bars
- Pre-market gap detection done at ~14:30 CET on US futures vs prior close

---

### 5.4 Screen E — Closed-End Fund Discount

**Status:** Phase 6
**Type:** EOD
**Universe:** ~400 US-listed CEFs

**Inputs:**

- Daily NAV (from CEFConnect scrape or EODHD)
- Daily market price
- 52-week and 2-year discount distribution per CEF

**Trigger:**

- Current discount > mean discount + 1.5 stddev over 2 years
- AND current discount > 8% in absolute terms (filter out funds that normally trade at par)
- AND CEF leverage ratio < 50% (avoid distress)
- AND CEF average daily volume > $500k (liquid enough)

**Alert tiers:**

- Premium: discount > 2σ AND discount > 15% absolute
- Opportunity: 1.5–2σ
- Interesting: 1–1.5σ

**Implementation notes:**

- Very slow-moving, EOD only
- Outputs to a "long-duration value" watchlist
- Hold periods are weeks to months (mean reversion of discount, not price)

---

### 5.5 Screen F — Darvas Box Breakout

**Status:** Phase 6
**Type:** EOD (weekly)
**Universe:** US equities filtered by liquidity (avg daily $ vol > $10M)

**Box detection:**

- Look for stocks where the highest high of the last N (default 60) sessions has not been broken
- AND the price range has compressed (last 20-session range < 50% of prior 60-session range)
- The "box" = (top: highest high in window, bottom: lowest low in window)

**Trigger:**

- Daily close > box top + 1% with volume > 2× 50-day avg
- Box must have been established for at least 4 weeks

**Alert tiers:**

- Premium: box established > 8 weeks, volume > 3×, in industry above 200-day MA
- Opportunity: box established > 4 weeks
- Interesting: box established 2-4 weeks (marginal)

**Exit logic suggested in alert:**

- Initial stop: 1% below box top (now support)
- Trail: define new box on consolidation, ratchet stop to new box bottom

---

### 5.6 Screen G — Cross-Asset Divergence Detector

**Status:** Phase 7 (companion to Screen C)
**Type:** Live (evaluated on each session close)
**Universe:** Pairs of correlated assets

**Configured pairs (in `screens.yaml`):**

```yaml
divergence_pairs:
  - { a: "^N225", b: "^STOXX", name: "Nikkei vs Europe" }
  - { a: "^N225", b: "^GSPC", name: "Nikkei vs SPX" }
  - { a: "BTCUSD", b: "^IXIC", name: "BTC vs Nasdaq" }
  - { a: "BTCUSD", b: "ETHUSD", name: "BTC vs ETH (intra-crypto)" }
  - { a: "USDJPY", b: "^GSPC", name: "Yen vs SPX (carry proxy)" }
  - { a: "KRE", b: "^GSPC", name: "Regional banks vs SPX" }
  - { a: "XLE", b: "USOIL", name: "Energy stocks vs oil" }
```

**Logic per pair (A, B):**

1. Compute 30-day rolling Pearson correlation between A and B daily returns
2. Compute daily return spread: r_spread = r_A - r_B
3. Compute Z-score of r_spread using rolling 90-day mean and stddev of the spread
4. If |Z| > 2: flag divergence event

**Trigger:**

- Z > 2 or Z < -2 over a single session → asset class divergence alert
- Z > 3 → Premium-tier alert (extreme dislocation)

**Output:**

- Adds context flag to other screens (Screen C uses it as a confidence multiplier)
- Standalone alert when extreme

---

### 5.7 Screen C — Capitulation Wick Reversal ("Falling Knives")

**Status:** Phase 7 (final phase, depends on G)
**Type:** Live (multi-timeframe)
**Universe:** Broad — equities, crypto, FX, commodities. Limited only by liquidity threshold.

**Per asset class universe size:**

- Crypto: ~80 majors and large-cap alts (BTC, ETH, top 100 by market cap)
- FX: 28 major pairs
- US equities: liquid 1500
- European equities: liquid 500
- Commodities: silver, gold, oil, copper, natgas (continuous front-month)
- Indices: SPX, NDX, RUT, DAX, STOXX, N225, HSI, FTSE

**Stage 1 — Big drop detection:**
Per asset, fire stage 1 if ANY of:

- Single-session drop > 3× the asset's own 90-day daily volatility
- Asset's regime classifier flips to Stress or Crisis
- Screen G fires with |Z| > 2 on a pair involving this asset
- Catalyst event from curated calendar (NFP, FOMC, BoJ, ECB, OPEC, major earnings)

Mark asset as "active capitulation watch" for next 72 hours.

**Stage 2 — Support identification:**
For each active asset, maintain a precomputed support map:

- Prior daily swing lows (last 6 months) — use a swing-low detection algorithm (e.g., a low that is the lowest in a ±5-bar window)
- Prior weekly swing lows (last 2 years)
- 200-day SMA and 200-day EMA
- Fibonacci retracements (0.382, 0.5, 0.618, 0.786) from the most recent major impulse (defined as: from most recent significant swing low to swing high within last 12 months)
- Round-number levels (especially for crypto: $1k increments for BTC, $100 for ETH, etc.)
- Volume profile point of control over last 60 sessions
- Prior consolidation zones (multi-week range bottoms)

Identify **confluence zones**: groups of ≥ 2 support types within 1% of each other for equities/FX, ±1.5% for crypto, ±0.5% for high-priced assets.

**Stage 3 — Wick signature detection (the critical trigger):**

For each active asset, on each new bar close on the configured trigger timeframes (1H, 4H, 1D — multi-TF):

Compute:

- `range = high - low`
- `body = |close - open|`
- `lower_wick = min(open, close) - low`
- `upper_wick = high - max(open, close)`

Trigger conditions (ALL required):

- `lower_wick / range >= 0.60` (long lower tail)
- `(close - low) / range >= 0.66` (close in upper third)
- bar's `low` ≤ a confluence zone (touches or pierces support)
- volume of the bar ≥ 2× the 20-bar moving average volume on that timeframe
- bar is among the worst 5% of intra-bar drawdowns over last 60 bars on that timeframe (the "panic" filter)

→ **PREMIUM-tier alert fires immediately** with:

- Suggested entry zone: from current price to wick midpoint
- Suggested stop: 0.5% below wick low (asset-class dependent)
- Computed position size for 1%, 2%, and 5% account risk (so user can choose tier)
- R:R to next overhead resistance, computed automatically
- Multi-TF confluence: list of all timeframes that simultaneously show wick signature (more TFs = higher conviction)
- Screen G flag: if divergence pair is also firing, mark "G+C" combined = maximum conviction

**Stage 4 — Confirmation gate (for the DCA-with-confirmation pattern):**

After the initial wick alert, the engine monitors the NEXT bar:

- If next bar closes above wick low AND prints higher high than wick bar → CONFIRMATION alert (tier: Opportunity, suggesting "add to position")
- If next bar's low breaks the wick low by > 0.3% → INVALIDATION alert (tier: Premium, suggesting "stop hit, abandon thesis")
- If neither (sideways): no alert, continue monitoring up to 3 bars

**Position sizing logic (encoded in alert payload):**

```
risk_amount = account_value * risk_pct
stop_distance_pct = (entry - stop) / entry
position_size_usd = risk_amount / stop_distance_pct
allocation_pct = position_size_usd / account_value

# Caps per asset class
max_allocation = {
    crypto:   0.30,  // 30% max on a single crypto
    equity:   0.20,  // 20% max on a single equity (gap risk)
    fx:       0.50,  // 50% max on FX (low gap risk)
    commodity:0.25,
    index:    0.30
}
```

Three suggested risk_pct tiers presented in the alert:

- 1% risk (standard)
- 2% risk (conviction)
- 5% risk (full conviction — only suggested if Premium + G+C combined + multi-TF)

User chooses allocation by tapping the corresponding inline keyboard button in Telegram.

**Exit management (suggestion engine, optional automation in Phase 9):**

- Move stop to breakeven when price reaches +1R
- Trail stop to last higher low on setup timeframe
- Partial-exit alerts at +3R, +6R, +10R (suggest taking 1/3 each, let final third run)
- "Hard reverse" alert if price prints lower-high lower-low on setup TF after entry

**Why this is last in implementation order:**

- Highest engineering complexity (support detection, multi-TF, confluence calc, confirmation logic)
- Depends on Screen G being live for the divergence multiplier
- Depends on regime classifier being robust (it only runs in Stress/Crisis)
- But also: this is where the highest P&L lives, so worth doing properly rather than rushing

---

## 6. Alert System

### 6.1 Alert Tiers

| Tier        | Telegram chat        | Sound               | Use                                  |
| ----------- | -------------------- | ------------------- | ------------------------------------ |
| Premium     | `Trader-Premium`     | Loud, vibrate       | Interrupt-worthy. 0-2/day expected.  |
| Opportunity | `Trader-Opportunity` | Silent push         | Look during break. 2-5/day expected. |
| Interesting | `Trader-Digest`      | No push, EOD digest | Review evening. 5-15/day expected.   |

### 6.2 Alert Payload Format

Internal struct:

```cpp
struct Alert {
    AlertId id;
    Timestamp ts;
    std::string screen;             // "A", "B", ..., "G"
    Tier tier;                      // Premium/Opportunity/Interesting
    InstrumentId instrument;
    std::string symbol;
    Regime regime_at_alert;

    // Core trade params
    Price suggested_entry_low;
    Price suggested_entry_high;
    Price suggested_stop;
    Price target_1, target_2, target_3;
    double rr_to_target_1;

    // Position sizing (computed for 3 risk tiers)
    PositionSize size_1pct, size_2pct, size_5pct;

    // Context
    std::vector<std::string> confluence_factors;  // e.g., ["200MA","prior_swing","fib_618"]
    std::string news_summary;                      // optional
    nlohmann::json extra;                          // screen-specific
};
```

### 6.3 Telegram Message Format

Premium example:

```
🟢 PREMIUM — BTCUSD
Screen C (Capitulation Wick) + G (Divergence)
Regime: CRISIS

Entry: 47800 – 48400
Stop:  47200 (-1.3%)
T1:    52000 (3.2R)
T2:    58000 (8.0R)

Confluence: 200d MA + prior swing low + Fib 0.618
Multi-TF: 1H, 4H wicks aligned
Divergence: BTC vs Nasdaq Z = +3.4σ

Size for 1% risk: 0.78 BTC ($37k, 8% account)
Size for 2% risk: 1.56 BTC ($75k, 15% account)
Size for 5% risk: 3.90 BTC ($187k, 37% account) — CAPPED at 30%

[ Saw it ]  [ Acting ]  [ Skip ]  [ Note ]
```

Buttons POST back to engine `/api/alert_response`, logged in `alerts.acted_on`.

### 6.4 Telegram Bot Setup

- Create bot via `@BotFather`, get token
- Three chats (or three channels): Premium, Opportunity, Digest
- Bot is admin in each
- Engine config has token + chat_ids
- Webhook receives button responses (set via `setWebhook` API)
- Embedded HTTP server has a route `/telegram/webhook` to handle inbound

C++ implementation: `cpp-httplib` for outgoing POSTs, same library exposes server for inbound webhook on the same process.

### 6.5 Digest

EOD job at 22:30 CET:

- Aggregate all Interesting-tier alerts from the day
- Format as a single message with one-line summaries per alert
- Send to Digest chat
- Plus: daily regime status, sector rotation summary, candidate watchlist for next day

---

## 7. Web UI

### 7.1 Pages

1. **Dashboard** (`/`)
   - Current regime + history mini-chart
   - Sector rotation heatmap (Screen D)
   - Today's alerts feed (filterable by tier/screen)
   - Active candidates from EOD screens

2. **Candidates** (`/candidates`)
   - All active candidates, sortable
   - Per-candidate detail page with chart

3. **Alerts log** (`/alerts`)
   - Full history, filterable
   - Acted-on toggle to update without going through Telegram

4. **Universe** (`/universe`)
   - List of instruments watched per asset class
   - Add/remove, search Saxo for new symbols

5. **Config** (`/config`)
   - Live edit of screens.yaml parameters
   - Hot-reload without restart

### 7.2 Tech Stack

- Backend: Crow or Drogon serving JSON via REST + WebSocket for live updates
- Frontend: plain HTML/JS + lightweight chart library (Chart.js or lightweight-charts from TradingView, both MIT)
- No build step; static files served from `ui/`
- Single-page app, vanilla JS routing

### 7.3 API Endpoints (sketch)

```
GET    /api/regime                   -> current regime + history
GET    /api/sector_rotation          -> heatmap data
GET    /api/alerts?tier=&screen=&date= -> filtered alerts
GET    /api/alerts/:id               -> single alert detail
POST   /api/alert_response           -> button press from Telegram (and UI)
GET    /api/candidates?status=active -> open candidates
GET    /api/instruments              -> list watched instruments
POST   /api/instruments              -> add new
DELETE /api/instruments/:id          -> remove
GET    /api/config/screens           -> current screen params
POST   /api/config/screens           -> update + hot-reload

WS     /ws/live                      -> push: alerts, regime changes, prices
```

---

## 8. Configuration

### 8.1 `config/config.yaml`

```yaml
server:
  http_port: 8080
  ws_port: 8080 # same as http via Crow

logging:
  level: info
  file: /var/log/trader-engine/engine.log
  rotation_mb: 100

database:
  path: /var/lib/trader-engine/state.db

session:
  prep_window: { open: "15:00", close: "16:30", tz: "Europe/Zurich" }
  us_session: { open: "15:30", close: "22:00", tz: "Europe/Zurich" } # CEST
  alerts_window: { open: "15:00", close: "22:30", tz: "Europe/Zurich" }
  # Alerts outside this window are queued, not pushed

telegram:
  bot_token_env: TELEGRAM_BOT_TOKEN
  chat_premium_env: TG_CHAT_PREMIUM
  chat_opportunity_env: TG_CHAT_OPPORTUNITY
  chat_digest_env: TG_CHAT_DIGEST

saxo:
  base_url: https://gateway.saxobank.com/openapi
  token_env: SAXO_TOKEN
  refresh_token_env: SAXO_REFRESH_TOKEN
```

### 8.2 `config/universe.yaml`

```yaml
universes:
  us_equity_liquid_1500:
    source: file:universes/us_liquid_1500.csv
    asset_class: equity
    primary_feed: saxo
  us_sector_etfs:
    source: file:universes/us_sectors.csv
    asset_class: equity
    primary_feed: saxo
  crypto_majors:
    source: file:universes/crypto_majors.csv
    asset_class: crypto
    primary_feed: binance
  fx_majors:
    source: file:universes/fx_majors.csv
    asset_class: fx
    primary_feed: saxo
  cef_universe:
    source: file:universes/cefs.csv
    asset_class: equity
    primary_feed: saxo
```

### 8.3 `config/screens.yaml`

```yaml
screens:
  screen_b_pullback:
    enabled: true
    universe: us_equity_liquid_1500
    trend_template:
      rs_min: 70
      pct_above_52w_low_min: 0.30
      pct_below_52w_high_max: 0.25
    pullback:
      rsi_min: 30
      rsi_max: 50
      proximity_to_20ma: 0.02
      max_earnings_days: 5

  screen_d_rotation:
    enabled: true
    universe: us_sector_etfs
    alert_on_200ma_flip: true

  screen_a_dislocation:
    enabled: true
    universe: us_equity_liquid_1500
    intraday_resolution: 1m
    aggregation: 5m
    sigma_threshold: 2.5
    volume_multiplier: 2.0
    cooldown_minutes: 60

  screen_c_capitulation:
    enabled: true
    universes: [us_equity_liquid_1500, crypto_majors, fx_majors, us_sector_etfs]
    timeframes: [1h, 4h, 1d]
    wick_ratio_min: 0.60
    close_position_min: 0.66
    volume_multiplier: 2.0
    support_confluence_min: 2
    confluence_proximity_pct:
      equity: 0.01
      crypto: 0.015
      fx: 0.005

  screen_g_divergence:
    enabled: true
    pairs:
      - { a: "^N225", b: "^STOXX", name: "Nikkei vs Europe" }
      - { a: "BTCUSD", b: "^IXIC", name: "BTC vs Nasdaq" }
      # ... see section 5.6
    correlation_window: 30
    zscore_window: 90
    alert_threshold: 2.0
    premium_threshold: 3.0
```

---

## 9. Implementation Phases

Each phase is a checkpoint where the system is functional and useful even if incomplete.

### Phase 0 — Engine Skeleton (2 weeks)

- CMake project, dependencies wired
- `BrokerAdapter` interface + Saxo OAuth + token refresh
- Saxo REST quote fetching (no streaming yet)
- `TimeSeriesStore` in-memory
- SQLite schema + persistence writer
- spdlog + config loading
- Trivial Crow HTTP server with `/api/health`
- systemd service definition

**Deliverable:** Engine runs on VM, pulls EOD bars for a small test universe, persists to SQLite, web ping works.

### Phase 1 — Regime Classifier (1 week)

- Pull SPX, VIX, breadth, HY OAS daily
- Implement classifier with scoring + hysteresis
- `/api/regime` endpoint
- Backfill against history (2007-present) and validate labels match expectations
- Log regime changes to `regime_log` table

**Deliverable:** `GET /api/regime` returns current regime; backtest confirms 2008, 2020, 2022, 2024-Aug labelled correctly.

### Phase 2 — Screen D (Industry Rotation) (1 week)

- ~70 sector ETFs in universe
- EOD batch job after US close
- Compute MA distances, RS rankings
- Web UI heatmap page
- (No alerts yet for rotation flips — add in Phase 4)

**Deliverable:** Open browser at 16:30 CET, see today's sector picture.

### Phase 3 — Screen B (Swing Pullback) (2 weeks)

- Minervini trend template implementation
- Pullback detection
- Candidate persistence + expiration logic
- Web UI "Swing Watchlist" page
- (Alerts deferred to Phase 4 when dispatcher is ready)

**Deliverable:** Each evening, watchlist of 0-10 swing candidates with entry zones.

### Phase 4 — Alert System + Telegram (1 week)

- Telegram bot setup + 3 chats
- `AlertDispatcher` with tier routing
- Inline keyboard responses + webhook
- EOD digest job
- Backfill: enable alerts on Screens D and B retroactively (they emit candidates already)

**Deliverable:** Phone receives styled alerts; button presses log to DB.

### Phase 5 — Screen A (Intraday Mean Reversion) (3 weeks)

- Saxo streaming WebSocket subscription
- Intraday bar aggregation in `TimeSeriesStore`
- Rolling VWAP + stddev maintenance
- Pre-market gap detection
- Live trigger evaluation
- Cooldown logic
- Stress-test under live load

**Deliverable:** Live intraday alerts during US session, properly tiered.

### Phase 6 — Screens E & F (CEF Discounts, Darvas) (2 weeks)

- CEF NAV data ingest (CEFConnect scrape or EODHD)
- CEF discount screen
- Darvas box detection on weekly bars
- Both feed into watchlist + alerts

**Deliverable:** Two more screens producing parallel candidate streams.

### Phase 7 — Screens G & C (Divergence + Capitulation Wick) (4-6 weeks)

- Cross-asset pair correlation tracking
- Z-score divergence detection (Screen G)
- Support map computation (per asset, daily refresh)
- Wick signature detection (multi-TF)
- Confluence calculation
- Confirmation bar gate
- Position sizing engine
- Integration: G amplifies C's confidence; both gated by regime

**Deliverable:** The full "Falling Knives" engine, screening thousands of instruments across asset classes.

### Phase 8 — Backtesting Framework (2 weeks)

- Historical replay against `bars_daily` and intraday data
- Per-screen P&L attribution
- Slippage and commission modeling
- Sharpe, max DD, win rate metrics
- Output reports as HTML

**Deliverable:** Can answer "how would Screen X have performed in 2022?"

### Phase 9 — Optional: Execution Integration (variable)

- Place orders via Saxo OpenAPI from within engine
- Manual approval flow (human confirms via Telegram before order goes live)
- Order tracking, fill notifications
- This is intentionally last; trade execution is high-blast-radius

**Deliverable:** Tap "Acting" in Telegram → engine submits order; you get fill confirmation.

---

## 10. Open Questions & Future Considerations

- **Multi-account support:** at some point, run separate sizing for different accounts (e.g., crypto vs equity accounts). Defer.
- **Mobile-native UI:** the web UI works on phone but a native app could be nicer. Defer indefinitely.
- **ML-based news classification for Screen A's "non-existential" filter:** initially keyword-based; could add a small LLM call if needed. Cost-conscious — defer.
- **Short side:** all screens are currently long-only. Short variants of A, B, C would roughly mirror the long logic. Defer until long side is profitable.
- **Tax-loss season screen:** specific November-December screen for forced-sale opportunities in small-caps. Possible Phase 10.
- **Correlation cluster detection:** if multiple alerts fire on correlated names, alert volume could spike misleadingly. Add a "cluster suppression" layer once we have enough data to tune it.

---

## Appendix A — Saxo OpenAPI Notes

- Auth: OAuth 2.0, refresh tokens valid 90 days. Engine must persist and auto-refresh.
- Streaming: WebSocket at `wss://streaming.saxobank.com/openapi/streamingws/connect`. Binary message framing with protobuf-like headers. Subscriptions are created via REST, then messages stream on the WS connection.
- Rate limits: REST endpoints have per-endpoint limits documented in their portal; respect via token-bucket in `SaxoBrokerAdapter`.
- Historical bars: `/chart/v1/charts` endpoint, paginated.
- Reference: instrument lookup by UIC (unique instrument code), not by symbol.

---

## Appendix B — Telegram Bot API Notes

- Bot token from `@BotFather`
- Send message: `POST https://api.telegram.org/bot<TOKEN>/sendMessage` (JSON body)
- Inline keyboard: `reply_markup` field with `inline_keyboard` array
- Receive button presses: `setWebhook` to your engine's public HTTPS endpoint, or use long-polling via `getUpdates`
- For a VM behind reverse proxy: easier to use long-polling, no inbound HTTPS needed
- Markdown vs HTML parse mode: HTML is safer for arbitrary text (no escape hell)

---

## Appendix C — Glossary

- **R / R:R:** "R" = the amount risked on a trade (entry - stop). R:R is reward-to-risk ratio. A 3R win means profit = 3 × the original risk amount.
- **Wick signature:** A candle with a long lower (or upper) shadow indicating absorption at an extreme; the wick low/high is treated as the structural extreme.
- **Confluence zone:** Region where multiple independent support/resistance types overlap.
- **Regime:** A discrete market state (Bull/Chop/Stress/Crisis) that gates which screens are enabled.
- **DCA with confirmation:** Scaling into a position only after each successive bar confirms the prior; protects against "knife-catching".

---

_End of spec v0.1._
