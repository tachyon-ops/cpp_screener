---
name: codebase-map
description: >
  Project-specific orientation: where every kind of thing lives in this
  codebase. Load at the start of every session to know where to look and 
  where to put things.
---

# Codebase Map

## Project Identity

- **Name:** cpp_screener
- **Purpose (one sentence):** A multi-instrument, multi-asset-class market screener and alert engine that evaluates independent screen modules against a wide universe, gates by market regime, and emits alerts.
- **Primary stack:** C++17/C++20, SQLite, Crow/Drogon (HTTP/WS), React (UI)
- **Repository layout:** Monolithic C++ Application
- **Build orchestrator:** CMake

---

## Top-Level Structure

```text
cpp_screener/
├── CMakeLists.txt              ← Top-level build config
├── config/                     ← Engine config (ports, thresholds, universe, screens) and secrets
├── include/trader/             ← Public headers (interfaces, core types, shared structures)
├── src/                        ← Implementation files and private headers
├── ui/                         ← React SPA (Vite) utilizing Prisma UI components
├── scripts/                    ← Helper scripts (backfill history, run backtests)
├── tests/                      ← GoogleTest unit and replay tests
├── docs/                       ← Specifications and architectural notes
└── .agents/                    ← Agent skills and rules
```

---

## Where Things Live

| Kind of thing | Where it lives | Naming convention |
|---|---|---|
| Domain entities and types | `include/trader/core/` | `snake_case.hpp` |
| Pure business logic | `src/core/` | `snake_case.cpp` |
| Ports (interfaces for I/O) | `include/trader/broker/` | `abstract_interface.hpp` |
| Adapters (Port implementations) | `src/broker/` | `snake_case.cpp` |
| HTTP / API endpoints | `src/web/` | `snake_case.cpp` |
| Screen logic | `src/screens/` | `screen_<id>_<name>.cpp` |
| Regime classification | `src/regime/` | `snake_case.cpp` |
| Database store / schemas | `src/persistence/` | `snake_case.cpp` |
| Configuration | `config/` | `*.yaml` or `*.env` |
| Frontend components | `ui/src/` | React TSX, `*.tsx`, `*.ts` |
| Unit tests | `tests/unit/` | `*_test.cpp` |
| Integration/Replay tests | `tests/replay/` | `*_test.cpp` |

---

## Package / Module Dependency Direction

```text
web/* and alerts/*          ← may depend on everything below
    ↓
screens/* and regime/*      ← may depend on core/*, data/*, broker/* (interfaces only)
    ↓
broker/* (ports)            ← interfaces only (e.g., broker_adapter.hpp); no dependencies on implementations
    ↑ (adapters implement ports in src/broker/)
data/* and persistence/*    ← implement data layer and storage
    ↓
core/*                      ← pure, dependency-free types (Price, Timestamp, Instrument)
```

---

## Build, Run, Test

```bash
# Configure and build backend
mkdir -p build && cd build
cmake ..
make -j4

# Build UI
cd ui
npm install
npm run build

# Run engine (will serve ui/dist)
./trader-engine

# Run tests
cd build && ctest --output-on-failure
```

---

## Environment

- See `config/secrets.env` for API keys.
- Requires Saxo OpenAPI credentials for live streaming, and SQLite3 available.

---

## Key Documents to Read

1. This file (`codebase-map`)
2. `SPECS_v1.0.md` — The foundational architectural blueprint
3. Other library skills relevant to the task (`architecture`, `api-design`)
