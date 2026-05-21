# Market Screener Engine — System Specification

**Version:** 0.3 (a.k.a. v3.0)
**Owner:** Nuno
**Last updated:** 2026-05-21
**Status:** Draft. Sections subject to revision as we iterate on individual subsystems.

---

## 0. Changelog

### v0.3 — Merge of Spec Part 2 (Validation & Ops) into the main spec

- §3.3: Added `positions` and `parameter_changes` tables to SQLite schema (from Part 2 §3.3 and §3.5)
- §6.6 (new): Alert Clustering & Deduplication — the "August 2024 problem" and its solution
- §6.7 (new): Extended button-press response schema (5 button types, skip reasons)
- §9 (new): Trade Journal & Learning Loop — attribution, parameter tuning, screen suspension, "what would have happened" tool, annual review
- §10 (new): Backtesting Framework — walk-forward methodology, fill/slippage modeling, stress event replays, regime classifier self-validation
- §11 (was §9): Implementation Phases — Phase 8 expanded; Phase 7 references clustering
- §12 (was §10): Open Questions — merged from both source documents
- Architecture diagram and §2.3 project layout updated to reflect new modules

### v0.2 — Strategy refinements

- §3.5: External Support/Resistance Provider interface (Nuno has existing SR indicator in another repo)
- §5.7: Rewrote Screen C stop management to match Nuno's actual strategy (immediate BE migration, trail on lower SR levels)
- §6: Pushover added as optional parallel channel for Premium alerts
- §6.4: Clarified Telegram latency picture

### v0.1 — Initial spec

- Architecture, data layer, 7 screens (A-G), regime classifier, alert system, web UI, configuration, implementation phases

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
│   │   ├── cluster.hpp            # Cluster definitions + collapse logic (§6.6)
│   │   └── telegram.hpp           # Telegram Bot API client
│   ├── journal/
│   │   ├── position_tracker.hpp   # Tracks lifecycle from "Acting" → close (§9.2)
│   │   ├── attribution.hpp        # Per-screen, per-tier metrics (§9.3)
│   │   └── learning_loop.hpp      # "What would have happened" tool (§9.6)
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
├── backtest/                      # Separate binary, shares screens/regime/SR code
│   ├── CMakeLists.txt
│   ├── feeder.hpp                 # Deterministic historical bar feeder
│   ├── sim_broker.hpp             # Simulated fills, slippage, commissions
│   ├── metrics.hpp                # Aggregates metrics from trade events
│   └── main.cpp
├── ui/
│   ├── index.html
│   ├── dashboard.js               # Vanilla JS, no framework needed
│   └── style.css
├── scripts/
│   ├── backfill_history.cpp       # One-shot historical data import
│   └── replay_event.cpp           # Replay a specific stress event
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

-- Positions tracks the lifecycle of acted-on alerts (see §9.2)
CREATE TABLE positions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    alert_id INTEGER NOT NULL,
    instrument_id INTEGER NOT NULL,
    direction TEXT,                -- 'long' or 'short'
    entry_ts TEXT NOT NULL,
    entry_price REAL NOT NULL,
    size REAL NOT NULL,
    initial_stop REAL,
    current_stop REAL,
    status TEXT,                   -- 'open','closed_winner','closed_loser','closed_be'
    exit_ts TEXT,
    exit_price REAL,
    exit_reason TEXT,              -- 'trail_stop_hit','target_hit','time_stop','manual_close'
    r_realized REAL,
    max_favorable_excursion_r REAL,
    max_adverse_excursion_r REAL,
    notes TEXT,
    FOREIGN KEY (alert_id) REFERENCES alerts(id),
    FOREIGN KEY (instrument_id) REFERENCES instruments(id)
);

-- Audit trail of parameter tuning decisions (see §9.4)
CREATE TABLE parameter_changes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,
    screen TEXT NOT NULL,
    parameter TEXT NOT NULL,
    old_value TEXT,
    new_value TEXT,
    rationale TEXT,
    backtest_report_path TEXT
);

-- Skip reasons captured from button responses (see §6.7)
CREATE TABLE alert_responses (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    alert_id INTEGER NOT NULL,
    response_ts TEXT NOT NULL,
    response_type TEXT NOT NULL,   -- 'seen','acted','skipped','noted','deferred'
    skip_reason TEXT,              -- only if response_type='skipped'
    note_text TEXT,                -- only if response_type='noted'
    FOREIGN KEY (alert_id) REFERENCES alerts(id)
);

CREATE INDEX idx_alerts_ts ON alerts(ts);
CREATE INDEX idx_alerts_screen ON alerts(screen);
CREATE INDEX idx_bars_daily_date ON bars_daily(date);
CREATE INDEX idx_positions_alert ON positions(alert_id);
CREATE INDEX idx_positions_status ON positions(status);
CREATE INDEX idx_alert_responses_alert ON alert_responses(alert_id);
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

### 3.5 External Support/Resistance Provider

Nuno maintains an existing support/resistance detection indicator in a separate repository. The engine consumes its output through an abstract interface so the implementation can be swapped without engine changes.

#### 3.5.1 Interface

```cpp
struct SRLevel {
    Price price;
    double strength;          // 0..1, significance score
    Resolution detected_on;   // 1H, 4H, 1D, 1W — which TF identified it
    int touches;              // historical touch count
    std::string source;       // "user_indicator", "swing_low", "fib", "round", ...
    std::string label;        // optional human-readable
    Timestamp first_seen;
    Timestamp last_touched;
};

class ISupportResistanceProvider {
public:
    virtual ~ISupportResistanceProvider() = default;

    // Returns all relevant SR levels for an instrument, sorted by price ascending
    virtual std::vector<SRLevel> get_levels(
        InstrumentId id,
        Resolution timeframe,
        Timestamp asof) const = 0;

    // Returns nearest support below a given price (used for trailing stops)
    virtual std::optional<SRLevel> nearest_support_below(
        InstrumentId id, Price current, Resolution timeframe) const = 0;

    // Returns nearest resistance above (used for targets and trailing on shorts)
    virtual std::optional<SRLevel> nearest_resistance_above(
        InstrumentId id, Price current, Resolution timeframe) const = 0;

    // Confluence: returns levels within `proximity_pct` of `price`, across all TFs
    virtual std::vector<SRLevel> levels_near(
        InstrumentId id, Price price, double proximity_pct) const = 0;

    // Tell the provider data is available (it may want to recompute)
    virtual void on_new_bar(InstrumentId id, const Bar& bar, Resolution r) {}
};
```

#### 3.5.2 Implementations

Three concrete implementations supported; chosen per config:

**`ExternalProcessSRProvider`** — for indicators written in Python, JS, or any non-native language.

- Engine spawns the indicator as a child process at startup
- Communication via stdin/stdout protocol (JSON line-delimited) OR via local HTTP endpoint (indicator runs its own tiny server on `127.0.0.1:9100`)
- Caching layer in C++ so we don't roundtrip every call
- Suitable for indicators with significant computation that runs on EOD or hourly schedule

**`LinkedSRProvider`** — for indicators written in C, C++, or with C ABI bindings.

- Linked as static or dynamic library
- Direct function calls, no IPC
- Lowest latency (microseconds)

**`FileBasedSRProvider`** — for any indicator regardless of language.

- Indicator runs as its own cron job, writes a JSON file per instrument to a shared directory
- Engine watches directory with `inotify`, reloads on change
- File format:
  ```json
  {
    "instrument": "BTCUSD",
    "generated_at": "2026-05-21T14:30:00Z",
    "levels": [
      {
        "price": 47200,
        "strength": 0.92,
        "tf": "1D",
        "source": "user_indicator",
        "touches": 4
      },
      {
        "price": 49800,
        "strength": 0.75,
        "tf": "4H",
        "source": "user_indicator",
        "touches": 2
      }
    ]
  }
  ```
- Slowest path but completely decouples the two systems

#### 3.5.3 Configuration

In `config/config.yaml`:

```yaml
sr_provider:
  type: external_process # or "linked" or "file_based"

  # If external_process:
  external_process:
    command: ["python3", "/opt/sr-indicator/run.py"]
    protocol: stdio_json # or "http"
    port: 9100 # only if protocol=http
    startup_timeout_sec: 10
    cache_ttl_sec: 60

  # If linked:
  linked:
    library_path: /opt/sr-indicator/libsr.so

  # If file_based:
  file_based:
    watch_dir: /var/lib/sr-indicator/output
    file_pattern: "{symbol}.json"
```

#### 3.5.4 Fallback

A trivial `AlgorithmicSRProvider` is included as the default and as a fallback for instruments your indicator doesn't cover. It computes:

- Swing highs/lows (lowest in ±5-bar window) over 6 months of daily bars
- 200-day SMA and 200-day EMA
- Fibonacci retracements (0.382, 0.5, 0.618, 0.786) of last major impulse
- Round-number levels per asset class

The engine can run multiple providers and merge their outputs (deduplicate by price proximity, take max `strength`).

#### 3.5.5 Consumed by

- Screen C (Capitulation Wick Reversal) — entry trigger + trail levels
- Screen B (Swing Pullback) — used to validate that the pullback is at a real support, not just at a 20-day MA in empty space
- Future screens: any screen that depends on structural price levels

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
For each active asset, query the `ISupportResistanceProvider` (see §3.5) on each configured trigger timeframe (1H, 4H, 1D). The provider returns SR levels with associated `strength` (0..1) and `touches` count.

Identify **confluence zones**: groups of ≥ 2 SR levels (potentially from different timeframes) within `confluence_proximity_pct` of each other:

- Equities: 1%
- Crypto: 1.5%
- FX: 0.5%
- Commodities: 1%

A confluence zone's aggregate strength is the sum of constituent levels' strengths, capped at 1.0. Higher aggregate = higher conviction.

Notes:

- The user's external SR indicator is the primary source. If it returns no levels for a given instrument, the `AlgorithmicSRProvider` fallback fills in (swing lows, 200MA, fibs).
- Levels marked as "from user_indicator" with high `strength` are weighted higher in conviction scoring.

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

**Stage 4 — Entry and immediate breakeven (the critical risk control):**

This is where Nuno's strategy differs from naive "wait for confirmation" approaches:

1. **Entry:** at the trigger bar (wick + SR confluence). Alert fires immediately with suggested entry zone.
2. **Initial stop:** placed just below the wick low (asset-class buffer below):
   - Crypto: 0.5% below wick low
   - Equities: 0.3% below wick low
   - FX: 0.2% below wick low
   - Commodities: 0.4% below wick low
3. **Immediate breakeven migration:** the moment price moves favorably by `be_trigger_pct` (default: 0.3R from entry, configurable per asset class), the stop migrates to entry price minus a tiny buffer (covers fees + slippage).

Rationale: a dead-cat bounce typically rolls over quickly. By moving to BE almost immediately, dead-cat outcomes cost essentially zero (just fees). The cost of being premature is small; the cost of holding a thesis-broken position through continuation lower is large.

**Stage 5 — Trail logic (uses SR provider):**

Once at breakeven, the trailing logic walks the stop up the structure rather than using a fixed % trail:

```
while position is open:
    lower_sr = sr_provider.nearest_support_below(instrument, current_price, setup_tf)

    if lower_sr.price > current_stop:
        # Price has cleared a new SR level. Ratchet stop just under it.
        new_stop = lower_sr.price * (1.0 - sr_buffer_pct)
        update_stop(new_stop)
        emit_alert("Stop trailed to {new_stop} below SR at {lower_sr.price}")

    sleep_until_next_bar(setup_tf)
```

This means: as price climbs and clears successive SR levels (now acting as new supports), the stop ratchets up to sit just below each one. The user only gets stopped out when price actually reverses through a real structural level, not on noise.

**Partial-exit alerts** continue to fire at +3R, +6R, +10R as opportunities to lock in profit, but the trail is the primary exit mechanism.

**Confirmation bar gate (optional, configurable per screen invocation):**

For cases where the user wants the "DCA with confirmation" pattern instead of single-shot entry:

- First entry: 1/3 size at trigger bar (Premium alert)
- Wait for next bar
- If next bar holds above wick low and prints continuation: Opportunity alert ("add second 1/3")
- Wait one more bar
- If still confirming: Opportunity alert ("add final 1/3")
- If any bar prints close below wick low: invalidation alert

Default behavior is single-shot entry with immediate BE; DCA mode is opt-in per asset class via `screens.yaml`.

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

**Why this is last in implementation order:**

- Highest engineering complexity (SR provider integration, multi-TF, confluence calc, immediate-BE state machine, structural trail)
- Depends on Screen G being live for the divergence multiplier
- Depends on regime classifier being robust (it only runs in Stress/Crisis, plus single-asset capitulation triggers)
- Depends on Nuno's external SR indicator being plumbed in (the algorithmic fallback is enough to start, but the user's indicator is the real edge)
- But also: this is where the highest P&L lives, so worth doing properly rather than rushing

---

## 6. Alert System

### 6.1 Alert Tiers

| Tier        | Telegram chat        | Pushover            | Sound               | Use                                  |
| ----------- | -------------------- | ------------------- | ------------------- | ------------------------------------ |
| Premium     | `Trader-Premium`     | YES (priority high) | Loud, vibrate       | Interrupt-worthy. 0-2/day expected.  |
| Opportunity | `Trader-Opportunity` | NO                  | Silent push         | Look during break. 2-5/day expected. |
| Interesting | `Trader-Digest`      | NO                  | No push, EOD digest | Review evening. 5-15/day expected.   |

Premium alerts are dual-channel: Telegram for rich content + buttons, Pushover for sub-second device-native push. Other tiers are Telegram only.

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

### 6.4 Telegram & Pushover Setup

**Telegram (rich alerts, all tiers):**

- Create bot via `@BotFather`, get token
- Three chats (or three channels): Premium, Opportunity, Digest
- Bot is admin in each
- Engine config has token + chat_ids
- **Inbound (button responses) — two options:**
  - **Long-polling** (default): engine calls `getUpdates` with `timeout=30` in a loop. Telegram returns immediately when an event is available. Latency: typically 100-500ms. No public endpoint needed. Simplest setup.
  - **Webhooks** (upgrade path): set `setWebhook` to point at your VM's public HTTPS URL. Latency: ~50-150ms. Requires domain + Let's Encrypt cert + reverse proxy. ~30 min one-time setup.

C++ implementation: `cpp-httplib` for outgoing POSTs, same library exposes server for inbound webhook on the same process if webhook mode is selected.

**Outbound latency (engine → phone):**

- Telegram outbound is essentially instant. Engine POSTs the message; Telegram's push servers deliver to the phone via APNS/FCM in 1-3 seconds end-to-end. This is fast enough for any human-in-the-loop trade decision.

**Pushover (Premium-tier sub-second push):**

- One-time $5 per platform via [pushover.net](https://pushover.net)
- Engine sends parallel push to Pushover when a Premium alert fires
- Pushover claims < 1s typical delivery; it uses APNS/FCM directly without the chat-app overhead
- Message body is a truncated one-liner ("🟢 BTCUSD wick reversal at 47800, 8R to T1") — full detail is in the Telegram message
- Priority 2 (emergency, with retry) available for "stop loss hit" or critical alerts

**When to use which:**

- Just Telegram: default. Works for 95% of cases.
- Add Pushover: if Premium alerts feel too slow on Telegram (you're missing entries by 5-10 seconds). Easy to add later.
- Add webhooks: if button response logging feels laggy. Rarely necessary.

### 6.5 Digest

EOD job at 22:30 CET:

- Aggregate all Interesting-tier alerts from the day
- Format as a single message with one-line summaries per alert
- Send to Digest chat
- Plus: daily regime status, sector rotation summary, candidate watchlist for next day

### 6.6 Alert Clustering & Deduplication

#### 6.6.1 The August 2024 problem

Without clustering, Screen C in a real capitulation event will fire on 30+ correlated names within the same 30 minutes. On August 5, 2024 (yen carry unwind), this would have meant:

- ~12 US regional banks (KRE, NYCB, ZION, PACW, WAL, CMA, KEY, RF, FITB, HBAN, MTB, USB) all wicking at their respective 200-day MAs around the same time
- BTC, ETH, SOL, AVAX, MATIC, LINK, ADA, DOT all wicking at major supports
- USDJPY wicking
- Nikkei futures wicking
- Several Japanese ADRs (Sony, Toyota) wicking

Result: ~30 Premium-tier alerts in ~3 hours. The user can't process this. They'll either over-trade correlated positions (correlated risk = blown account), pick at random (worse than picking the strongest), or ignore the lot (system useless precisely when it has the most edge).

The dispatcher must collapse correlated signals into a small number of representative alerts while preserving the _information_ that the cluster is firing.

#### 6.6.2 Cluster definitions

Three sources of cluster membership, used in priority order:

**Source A — Pre-defined clusters (highest priority):** stored in `config/clusters.yaml`. Hand-curated, ~20 clusters cover most correlation events. Examples:

```yaml
clusters:
  - id: us_regional_banks
    leader: KRE # ETF as the representative
    members: [NYCB, ZION, PACW, WAL, CMA, KEY, RF, FITB, HBAN, MTB, USB, ...]
    asset_class: equity

  - id: crypto_majors_l1
    leader: BTCUSD
    members: [ETHUSD, SOLUSD, AVAXUSD, ADAUSD, DOTUSD]
    asset_class: crypto

  - id: jp_market
    leader: ^N225
    members: [SONY, TM, MUFG, SMFG, MFG, NTDOY, ...]
    asset_class: equity_jp
```

**Source B — Sector ETF membership:** if two equities are both held by the same sector ETF with weight > 2%, they're in the same auto-cluster, with the ETF as leader. Catches the long tail without manual config.

**Source C — Rolling correlation (lowest priority, expensive):** for instruments not covered by A or B. 30-day rolling correlation matrix. If correlation > 0.7 with any existing cluster leader, join that cluster. Recompute weekly.

#### 6.6.3 Clustering rule

When Screen C (or any future multi-instrument screen) generates alerts:

```
within a 30-minute window:
    group alerts by cluster
    for each cluster with >= 3 alerts:
        promote the cluster's leader alert to Premium (if not already)
        demote all non-leader alerts to a single "cluster context" message
        attach a list of cluster members that also fired to the leader's alert
```

User experience: one Premium push for "Regional banks (cluster) — KRE wick at 200MA, 11 other names also at support: NYCB, ZION, ...". The other 11 don't fire as pushes; they appear in the cluster context expansion of the leader's alert.

If no clear leader exists (cluster has no pre-defined anchor), pick by: (1) highest liquidity, (2) highest confluence strength, (3) first to trigger.

#### 6.6.4 Throttling rules

Cluster collapse alone isn't enough — raw throttles handle pathological cases:

| Tier        | Hard cap  | Window           |
| ----------- | --------- | ---------------- |
| Premium     | 3 alerts  | 30 minutes       |
| Opportunity | 10 alerts | 60 minutes       |
| Interesting | unlimited | (goes to digest) |

If a 4th Premium alert would fire within 30 min of the previous 3: it's queued. Engine evaluates whether to displace one based on conviction score. If displaced, the bumped alert moves to Opportunity. After the 30-min window, the queue is re-evaluated.

For Crisis regime: throttle expands to 6 Premium / 20 Opportunity. System explicitly recognizes the user wants more signal during high-edge events, with practical limits.

#### 6.6.5 Cluster context message format

```
🟢 PREMIUM — KRE (Regional Banks cluster, 12 names)
Screen C @ 200MA, Regime: STRESS

KRE leader signal:
  Entry: 41.80 – 42.20
  Stop: 41.20 (-1.4%)
  T1: 44.50 (4.5R)

Cluster context (also wicking at SR):
  NYCB: at swing-low 11.20
  ZION: at 200MA 38.40
  PACW: at swing-low 8.90
  ... (8 more)

Suggested play:
  - Lead with KRE for size, liquidity
  - OR pick 2-3 individuals with deepest moves
  - DO NOT take all 12 — correlation = same trade

[ Lead with KRE ]  [ Pick 3 best ]  [ Skip cluster ]
```

The "Suggested play" line is intentional — it teaches the user not to over-correlate.

#### 6.6.6 Cluster dissolution

A cluster fires once and then stays in cooldown for 4 hours. New triggers on cluster members within the cooldown are demoted to Interesting (digest only) regardless of conviction. Prevents the same regional banks notifying every 30 minutes during a multi-hour panic.

If the regime flips back to Bull or Chop, all cluster cooldowns are reset.

#### 6.6.7 Single-name edge cases

A name can belong to multiple clusters (USB ∈ `us_regional_banks` and `us_large_caps`). When this happens, the name uses the _most specific_ cluster (smallest member count) for clustering decisions, and the alert is tagged with all memberships for context.

Genuinely unique names (no cluster match) alert standalone. Rare but important for Nuno's "exotic" instruments (CEFs, niche commodities).

### 6.7 Button-Press Response Schema

Telegram inline keyboard responses are richer than binary "saw it / acted":

| Button    | Stored value | Meaning                                      |
| --------- | ------------ | -------------------------------------------- |
| 👀 Saw it | `seen`       | User acknowledged, not acting                |
| ⚡ Acting | `acted`      | Taking the trade — prompts for fill details  |
| ❌ Skip   | `skipped`    | Explicit reject; secondary prompt for reason |
| 📝 Note   | `noted`      | Free-text annotation                         |
| ⏸ Defer   | `deferred`   | Watching for further confirmation            |

Skip reasons (secondary keyboard if Skip is pressed):

- Wrong regime (system disagrees with user)
- Bad news / headline risk
- Position size too large for me right now
- Already in correlated position
- Don't trust this name
- Other (free text)

**Skip reasons are gold.** They reveal where user judgment diverges from system judgment. If "wrong regime" is pressed often around regime transitions → classifier needs tuning. If "already in correlated position" is frequent during cluster events → cluster cooldown needs to be longer. The Trade Journal (§9) consumes this data to drive parameter tuning.

When ⚡ Acting is pressed, the engine prompts for actual fill price (or auto-detects if execution integration is live in Phase 9), creates a `positions` record (§3.3 schema), and tracks the position until exit. See §9.2 for full lifecycle.

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
  mode: long_polling # or "webhook"

pushover:
  enabled: false # toggle on once subscribed
  app_token_env: PUSHOVER_APP_TOKEN
  user_key_env: PUSHOVER_USER_KEY
  premium_only: true

sr_provider:
  type: file_based # placeholder — switch to "external_process" or "linked" once Nuno's indicator is wired
  external_process:
    command: ["python3", "/opt/sr-indicator/run.py"]
    protocol: stdio_json
    cache_ttl_sec: 60
  linked:
    library_path: /opt/sr-indicator/libsr.so
  file_based:
    watch_dir: /var/lib/sr-indicator/output
    file_pattern: "{symbol}.json"
  fallback_to_algorithmic: true

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

### 8.4 `config/clusters.yaml`

Pre-defined cluster groups for alert deduplication (see §6.6.2). Hand-curated, reviewed quarterly.

```yaml
clusters:
  - id: us_regional_banks
    leader: KRE
    members: [NYCB, ZION, PACW, WAL, CMA, KEY, RF, FITB, HBAN, MTB, USB]
    asset_class: equity
    cooldown_hours: 4

  - id: crypto_majors_l1
    leader: BTCUSD
    members: [ETHUSD, SOLUSD, AVAXUSD, ADAUSD, DOTUSD, MATICUSD, LINKUSD]
    asset_class: crypto
    cooldown_hours: 4

  - id: jp_market
    leader: ^N225
    members: [SONY, TM, MUFG, SMFG, MFG, NTDOY]
    asset_class: equity_jp
    cooldown_hours: 6

  - id: semiconductors
    leader: SMH
    members: [NVDA, AMD, INTC, TSM, AVGO, AMAT, KLAC, LRCX, MU, QCOM]
    asset_class: equity
    cooldown_hours: 4

  # ... ~20 total, see clusters.yaml for full list
```

---

## 9. Trade Journal & Learning Loop

### 9.1 The feedback gap

The v0.2 spec collected button-press responses but did nothing with them. A system that doesn't learn from its own performance gets worse over time as markets change but parameters don't. The learning loop closes this gap: button presses + actual trade outcomes + backtest replays = parameter tuning evidence. Every quarter, we look at "what would have happened if we'd tightened this threshold?" with real data, and ship or reject the change.

### 9.2 Trade lifecycle tracking

When ⚡ Acting is pressed:

1. Engine asks: "Filled at what price?" (text input or default = alert's mid-entry)
2. Engine creates a `positions` record (schema in §3.3)
3. Engine tracks the position until manually closed or stop hit
4. Sends periodic status updates ("BTCUSD position +3.2R, trailing stop now at 49100")
5. When closed, computes realized R and updates the record

If execution integration is live (Phase 9), the broker confirms fills automatically and the user skips step 1.

Status updates fire on meaningful events only (stop migrated, partial exit threshold hit, +1R/+3R/+6R/+10R checkpoints), not on every tick. Goal: keep the user informed without spam.

### 9.3 Per-screen attribution

Quarterly review answers, for each screen:

- **Trade count** broken down by tier (Premium / Opportunity / Interesting)
- **Acted-on rate** by tier (what fraction did the user act on?)
- **Hit rate** by tier (of those acted on, what fraction were winners?)
- **Average R** by tier and overall
- **Skip reasons distribution** (why did the user pass on alerts?)
- **Tier accuracy** — did Premium alerts actually perform better than Opportunity? If not, the tier logic is off.

Example output the loop should surface:

> Screen B (Swing Pullback), 2026 Q1:
>
> - 47 alerts total: 8 Premium, 22 Opportunity, 17 Interesting
> - Acted-on rate: Premium 75%, Opportunity 41%, Interesting 6%
> - Hit rate (acted): Premium 67%, Opportunity 45%, Interesting 50%
> - Average R (acted): Premium 2.4R, Opportunity 1.1R, Interesting 0.8R
> - **Skip reasons:** 38% "Wrong regime", 22% "Bad news"
> - **Finding:** Premium tier behaves correctly. The "Wrong regime" skip frequency suggests Screen B is firing during regime transitions where the classifier hasn't caught up yet. Consider adding 1-day regime hysteresis specifically for Screen B's gate.

The finding paragraph is the deliverable. The numbers are inputs.

### 9.4 Parameter tuning workflow

Quarterly cadence — not weekly. Markets need time to produce enough samples for any conclusion to be meaningful.

For each screen with anomalous attribution data:

1. **Hypothesis** — articulate what change might help and why ("tighten wick_ratio_min from 0.60 to 0.65 to filter out marginal wicks that have been hitting BE more than winners")
2. **Backtest** — replay full history with the candidate parameter, compare metrics side-by-side with current
3. **Walk-forward test** — replay only on out-of-sample window (most recent year)
4. **Decision criteria:**
   - **Ship if:** walk-forward Sharpe improves AND walk-forward max DD doesn't worsen by > 20% AND trade count doesn't drop below statistical significance (~30 trades/year)
   - **Defer if:** improvement is marginal or restricted to specific regimes
   - **Reject if:** walk-forward is worse than current
5. **Ship** — update `screens.yaml`, log the change in `parameter_changes` table (schema in §3.3)

The audit trail is essential. Without it, parameter drift becomes untraceable and we lose the ability to ask "why is the system performing differently than 6 months ago?"

### 9.5 When to disable a screen

A screen gets suspended (alerts go to digest only, not pushed) if any of:

- Hit rate < 35% over the last 50 trades (statistical noise threshold)
- Average R < 0.5 over the last 50 trades
- Max DD on screen-only equity curve > 25%
- User skip rate > 80% for 30+ alerts (user is rejecting it consistently — system is wrong about something)

Suspension is not deletion. The screen keeps running and logging, but its alerts don't push. Manual reactivation required via config flag. Each suspension event is logged with the trigger metric for review.

### 9.6 The "what would have happened" tool

A daily-running tool that takes recent skipped or deferred alerts and reports what happened to them:

> Last 7 days, skipped alerts performance:
>
> - 12 alerts skipped, of which:
>   - 7 would have stopped out (correct skip)
>   - 3 would have hit T1 (missed opportunity — 1.2R, 3.4R, 2.1R)
>   - 2 are still pending
>
> **Pattern:** all 3 missed-opportunities were skipped with reason "Wrong regime" during a Bull → Chop transition. The classifier was correct; the user's manual override was wrong. Consider tightening hysteresis to make the gate firmer.

The kindest, most useful form of feedback the system can give: "you were right to skip these, you were wrong to skip these others, here's the pattern." Delivered as part of the EOD digest on a weekly cadence (Sunday evenings).

### 9.7 Annual screen review

Once a year, a deeper review:

- Total system performance vs benchmark (buy-and-hold SPX, buy-and-hold BTC)
- Which screens contributed positively, negatively, neutrally
- Are there screens we should retire?
- Are there new screens we should build? (Driven by user notes accumulating — e.g., "I keep noticing X pattern in my notes, should we encode it?")
- Universe pruning: are there instruments that never produce useful alerts? Remove them to reduce noise.

Output: a roadmap document for the next year, like this spec but at higher altitude.

---

## 10. Backtesting Framework

### 10.1 Why this matters more than it sounds

Every screen defines specific thresholds (wick ratio ≥ 0.60, volume ≥ 2×, RSI 30-50, etc.). Every number is a _hypothesis_. Until those numbers are replayed against historical data and produce trade results we can measure, the entire spec is opinions in YAML.

Two failure modes a backtester must protect against:

**Overfitting** — tuning parameters until backtest looks perfect against the specific 2008-2024 sample, then watching the system fail live because it memorized that sample. Counter: walk-forward analysis (train on 2008-2015, test on 2016-2020, train on 2008-2020, test on 2021-present, never tune on the test set).

**Look-ahead** — accidentally using information that wasn't available at the time. Most common form: using "today's" 200-day MA when scanning bar by bar instead of recomputing as of each historical date. Counter: strict `Timestamp asof` discipline in every data access, enforced by the backtest harness, not by convention.

A third danger specific to this system: **regime conditioning**. If we measure Screen C's win rate over all of 2010-2024, we get a low number because it spends most of its time disabled. The metric we care about is its win rate _when it was supposed to be active_. The backtester must respect the regime gate and report performance only over the periods when each screen was enabled.

### 10.2 Architecture

The backtester is a separate binary, not a runtime mode of the live engine, for one important reason: it must guarantee determinism and reproducibility, and the live engine has clock-driven nondeterminism (network jitter, race conditions on tick arrival). Shared code: the screens themselves, the regime classifier, the SR provider interface. Backtester-specific code: the historical data feeder, the simulated broker, the metrics aggregator.

```
┌─────────────────────────────────────────────────────────────┐
│                   backtest binary                           │
│                                                             │
│  ┌─────────────────┐    ┌──────────────────────────────┐    │
│  │ Historical Bar  │───▶│  Screen evaluators (shared)  │    │
│  │     Feeder      │    │   - Regime classifier        │    │
│  │ (deterministic) │    │   - Screens A-G              │    │
│  └─────────────────┘    │   - SR provider              │    │
│                         └──────────────┬───────────────┘    │
│                                        │ Alerts             │
│                                        ▼                    │
│                         ┌──────────────────────────────┐    │
│                         │   Simulated Broker           │    │
│                         │   - Fill modeling            │    │
│                         │   - Slippage                 │    │
│                         │   - Commissions              │    │
│                         │   - Position tracking        │    │
│                         └──────────────┬───────────────┘    │
│                                        │ Trade events      │
│                                        ▼                    │
│                         ┌──────────────────────────────┐    │
│                         │   Metrics Aggregator         │    │
│                         │   - Per-trade journal        │    │
│                         │   - Per-screen attribution   │    │
│                         │   - Regime-conditional       │    │
│                         │   - Walk-forward windows     │    │
│                         └──────────────┬───────────────┘    │
│                                        ▼                    │
│                         HTML report + JSON results          │
└─────────────────────────────────────────────────────────────┘
```

### 10.3 Data requirements

**Daily bars (required for all screens):**

- US equities: 2008-present, including delisted/dead names (survivorship bias is the #1 killer of equity backtests)
- Crypto majors: 2013-present for BTC, 2015-present for ETH, 2017+ for major alts
- FX majors: 2008-present
- Commodities: 2008-present (continuous front-month for futures)
- Indices: 2008-present
- VIX, HY OAS, breadth indices: 2008-present

Sources: EODHD or Norgate for equities (survivorship-bias-free is the must-have), Binance public REST for crypto historical, FRED for macro, Saxo for what it has.

**Intraday bars (required for Screen A, optional for C):**

- 1-minute bars for top 1500 US equities: 2021-present (Saxo provides ~3 years)
- 1-minute crypto: easy via Binance REST, 2017+
- For pre-2021 testing of Screen A: use daily as approximation (open/close VWAP estimates), flag as low-fidelity

**Storage format:** Parquet files partitioned by year/month/asset_class. Read sequentially during replay. SQLite is fine for state but too slow for full historical replay (Parquet + Arrow is ~50× faster for columnar scans).

### 10.4 Replay mechanics

The replay loop is deceptively simple but the details matter:

```
for each timestamp T in [backtest_start, backtest_end]:
    # 1. Feed new bars to engine state
    for each instrument:
        if has_new_bar(instrument, T):
            store.append_bar(instrument, bar)

    # 2. Update regime (only at session close timestamps)
    if is_session_close(T):
        regime = regime_classifier.evaluate(store, asof=T)

    # 3. Run enabled screens
    for each screen:
        if regime_enabled(screen, regime):
            new_alerts = screen.evaluate(store, regime, universe, asof=T)
            for alert in new_alerts:
                simulated_broker.handle_alert(alert, T)

    # 4. Advance positions
    simulated_broker.mark_to_market(T)
    simulated_broker.check_stops(T)
    simulated_broker.check_targets(T)
```

Critical: every screen evaluation receives an explicit `asof=T` parameter, and the time-series store must refuse to return data with `ts > T`. This is the look-ahead guard.

### 10.5 Fill and slippage modeling

The simulated broker translates alerts into positions with realistic friction. Naive backtests assume you fill at the exact alert price; real fills are worse.

**Fill model (long entry):**

- Alert price: `P_alert` (typically last trade at trigger bar close)
- Assumed fill: `P_fill = P_alert × (1 + slippage_bps / 10000)`
- Slippage assumptions (conservative defaults):

| Asset class              | Slippage (bps) | Commission (bps) |
| ------------------------ | -------------- | ---------------- |
| US large-cap equity      | 5              | 1                |
| US small-cap equity      | 20             | 3                |
| Crypto majors (BTC, ETH) | 8              | 10               |
| Crypto alts              | 25             | 15               |
| FX majors                | 2              | 1                |
| Commodity futures        | 5              | 2                |

For Screen C specifically: in real capitulation, slippage spikes. Model bumps slippage 3× during regime = Stress and 5× during Crisis. Conservative but realistic — Aug 5 2024 BTC had 100-200bps slippage on size during the wick.

**Stop fill model:**

- If price gaps through stop (next bar's high ≤ stop price for a long): fill at `min(open_next_bar, stop_price)`. Matters more for equities (overnight gaps) than 24/7 markets.
- Otherwise: fill at stop price + slippage.

**Realistic execution caveat:**
For Screen A and the immediate-BE migration of Screen C, the backtest cannot perfectly model tick-level fills. Approximation: engine assumes it could move the stop instantly when the trigger price prints in any 1-minute bar. In reality there's latency. Two reporting modes:

- "Ideal" — instantaneous stop movement, no latency
- "With friction" — 500ms latency modeled on stop migrations

Every backtest report shows both numbers. The gap between them is the system's exposure to execution risk.

### 10.6 Metrics

Per-trade record (matches the `positions` schema in §3.3, plus backtest-specific fields):

```json
{
  "trade_id": "...",
  "screen": "C",
  "tier_at_alert": "premium",
  "regime_at_alert": "stress",
  "instrument": "BTCUSD",
  "asset_class": "crypto",
  "entry_ts": "...",
  "entry_price": 48200,
  "stop_price": 47200,
  "exit_ts": "...",
  "exit_price": 56400,
  "exit_reason": "trail_stop_hit",
  "r_realized": 8.2,
  "days_held": 4.2,
  "max_favorable_excursion_r": 9.8,
  "max_adverse_excursion_r": -0.3,
  "confluence_factors": ["user_indicator_strong", "fib_618", "200ma"],
  "g_divergence_active": true
}
```

Per-screen aggregates (computed across windows):

- **Hit rate** (% of trades positive)
- **Average R** (mean realized R across all trades)
- **Expected value per trade** (avg R × allocation)
- **Sharpe** (annualized, computed from equity curve assuming each trade sized for 1% risk)
- **Max drawdown** (peak-to-trough equity decline)
- **Profit factor** (sum of winners / sum of losers, both in R)
- **MAE/MFE distribution** (max adverse vs max favorable excursion — tells us about stop placement)

System-level aggregates:

- **Total trades** by screen × tier × regime
- **Calendar P&L** (per-month, per-year)
- **Regime-conditional performance** (each screen's metrics restricted to periods when its target regime was active)

### 10.7 Walk-forward methodology

Backtest report includes three windows:

1. **Full history** (e.g., 2010-present): not a validation, just describes the system's behavior across the whole sample. Use for sanity, not parameter selection.

2. **Walk-forward** (rolling 5-year train + 1-year test): tune parameters on 2010-2014, evaluate on 2015. Then 2010-2015 train, 2016 test. And so on. Out-of-sample performance is the only honest metric.

3. **Stress event replays** (specific dates): hard-coded list of known regime events. For each, generate a focused report on screen behavior during that window:
   - 2008 Sep-Dec (GFC)
   - 2010 May 6 (flash crash)
   - 2015 Aug (China deval)
   - 2018 Q4 (Volmageddon + Dec selloff)
   - 2020 Feb-Apr (COVID)
   - 2022 (rate hike bear market)
   - 2024 Aug 5 (yen carry unwind)

These don't replace walk-forward; they're qualitative checks ("did Screen C fire on the BTC bottom Aug 5 2024? did Screen B avoid getting chopped up in 2022?").

### 10.8 Regime classifier self-validation

The regime classifier needs its own backtest. For each historical date, compute the regime label using the §4.3 scoring rules. Then compare to manually-labeled ground truth on key events:

| Date       | Expected regime | Acceptable also |
| ---------- | --------------- | --------------- |
| 2017-06-15 | Bull            | —               |
| 2018-12-24 | Stress          | Crisis          |
| 2020-03-23 | Crisis          | —               |
| 2020-04-15 | Stress          | —               |
| 2020-08-15 | Bull            | —               |
| 2022-06-15 | Stress          | —               |
| 2022-10-15 | Stress          | —               |
| 2023-08-01 | Bull            | —               |
| 2024-08-05 | Stress          | Crisis          |
| 2024-08-20 | Bull            | Chop            |

If the classifier mislabels > 2 dates, the scoring rules need adjustment. Don't tune thresholds individually for each date — that's overfitting. Tune in groups (e.g., "the breadth threshold seems off"), retest the full set.

### 10.9 Backtest report format

Output: one HTML file per backtest run, plus a JSON dump for further analysis.

HTML report sections:

1. **Summary** — system-level metrics, equity curve
2. **Regime calendar** — color-coded timeline showing regime labels day-by-day
3. **Per-screen** — drill-down for each screen with its own metrics
4. **Trade journal** — sortable table of every trade with detail rows
5. **Stress event reports** — one section per stress event with chart + screens that fired
6. **Walk-forward windows** — out-of-sample metrics for each window

JSON dump is the source of truth; HTML is generated from it. Lets later tools (parameter optimizers, visualizers) consume backtest output programmatically.

---

## 11. Implementation Phases

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
- **`ISupportResistanceProvider` interface + `AlgorithmicSRProvider` fallback (§3.5)**
- **Wire in Nuno's external SR indicator** (via chosen pattern: external_process / linked / file_based)
- Wick signature detection (multi-TF)
- Confluence calculation using SR provider output
- **Immediate-breakeven state machine** (Nuno's specific stop management)
- **Structural trailing using `nearest_support_below`** queries
- Optional DCA-with-confirmation mode (opt-in per asset class)
- Position sizing engine with asset-class allocation caps
- **Alert clustering & deduplication (§6.6)** — must ship with Screen C because it's the screen that creates the cluster problem
- Integration: G amplifies C's confidence; both gated by regime

**Deliverable:** The full "Falling Knives" engine, screening thousands of instruments across asset classes.

### Phase 8 — Backtesting Framework (3-4 weeks)

Detailed spec: §10.

- Separate `backtest` binary, shares screens/regime/SR code with main engine
- Historical data import into Parquet (US equities incl. delisted via Norgate or EODHD-pro)
- Deterministic bar feeder with strict `asof` discipline (look-ahead guard)
- Simulated broker with per-asset-class slippage and commission models
- "Ideal" vs "with friction" dual reporting for stop migrations
- Walk-forward windows (5-year train + 1-year test, rolling)
- Stress event replay harness (GFC, COVID, 2018 Q4, Aug 2024, etc.)
- Regime classifier self-validation against ground-truth labeled dates (§10.8)
- HTML + JSON report output

**Deliverable:** Can answer "how would Screen X have performed in 2022?" and "did Screen C fire on the Aug 2024 BTC bottom?" with reproducible numbers.

### Phase 8.5 — Trade Journal & Learning Loop (1-2 weeks)

Detailed spec: §9.

- Position tracker hooked into "Acting" button responses
- Skip-reason secondary keyboard + storage
- Per-screen attribution job (quarterly)
- "What would have happened" weekly report (Sunday digest)
- Parameter changes table + audit trail for any tuning
- Auto-suspension rules for underperforming screens

**Deliverable:** Every alert produces journal data; quarterly review can be run on demand.

### Phase 9 — Optional: Execution Integration (variable)

- Place orders via Saxo OpenAPI from within engine
- Manual approval flow (human confirms via Telegram before order goes live)
- Order tracking, fill notifications
- This is intentionally last; trade execution is high-blast-radius

**Deliverable:** Tap "Acting" in Telegram → engine submits order; you get fill confirmation.

---

## 12. Open Questions & Future Considerations

These are decisions flagged explicitly because they need user input or further iteration.

### 12.1 Data & infrastructure

- **Survivorship-bias-free historical data.** Norgate is the gold standard for US equities but costs ~$50/mo. EODHD's cheapest tier may not include delisted names. Without this, equity backtests overstate performance because they don't include companies that went to zero. Recommendation: accept the bias in Phases 0-7, pay for clean data before Phase 8 (Backtesting) ships. Note the bias explicitly in every backtest report run on biased data.

- **Cluster maintenance (§6.6.2).** The `clusters.yaml` file needs hand-curation. Who owns updates as the universe evolves (M&A, sector reclassifications)? Quarterly review by you is probably the right cadence, with the engine flagging instruments that frequently appear in correlated alerts but aren't in any cluster.

### 12.2 Alert dispatch

- **Tier promotion logic for clusters (§6.6.3).** When the entire `crypto_majors_l1` cluster fires, should the leader's tier _automatically_ promote one level (Opportunity → Premium) because cluster-firing is itself a confluence signal? Lean: yes for Crisis regime, no for Stress regime. Discuss.

### 12.3 Backtesting fidelity

- **Backtest fidelity for the immediate-BE state machine (§10.5).** The current backtest model assumes the engine can move the stop instantly on any favorable tick. In reality there's processing latency, broker round-trip, and order rest time. This will overstate Screen C's performance. Decided approach: report dual scenarios — "ideal" and "with 500ms friction" — in every backtest. The gap is the system's execution-risk exposure.

### 12.4 Learning loop tuning

- **Parameter tuning frequency (§9.4).** Specified quarterly. Argument for monthly: faster adaptation in regime shifts. Argument for quarterly: less overfitting, more statistical signal per review. Quarterly is the safer default; tighten later if needed.

- **What counts as "acted on"?** Currently the button press is the trigger. But if the user manually places a trade outside the engine on something the engine alerted, should that count? Tracking requires reconciliation with broker data. Phase 9 problem mostly.

### 12.5 Future screens & features (defer)

- **Multi-account support:** at some point, run separate sizing for different accounts (e.g., crypto vs equity accounts). Defer.
- **Mobile-native UI:** the web UI works on phone but a native app could be nicer. Defer indefinitely.
- **ML-based news classification for Screen A's "non-existential" filter:** initially keyword-based; could add a small LLM call if needed. Cost-conscious — defer.
- **Short side:** all screens are currently long-only. Short variants of A, B, C would roughly mirror the long logic. Defer until long side is profitable.
- **Tax-loss season screen:** specific November-December screen for forced-sale opportunities in small-caps. Possible Phase 10.

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

_End of spec v3.0 (v0.3)._
