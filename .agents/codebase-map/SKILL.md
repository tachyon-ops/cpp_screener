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
- **Primary stack:** C++20, SQLite, Crow (HTTP/WS), React (UI), Cocoa/WebKit Webview GUI
- **Repository layout:** Monolithic C++ Application with React SPA
- **Build orchestrator:** CMake and pnpm

---

## Top-Level Structure

```text
cpp_screener/
├── CMakeLists.txt              ← Top-level build config
├── run.sh                      ← Unified build & run script
├── pnpm-workspace.yaml         ← pnpm monorepo/workspace config
├── pnpm-lock.yaml              ← pnpm lockfile
├── include/trader/             ← Public headers (interfaces, core types, shared structures)
├── src/                        ← Implementation files and private headers
├── ui/                         ← React SPA (Vite) utilizing Prisma UI components
├── vendor/prisma/              ← [Read-Only] Git submodule containing shared PRISMA UI library
├── data/                       ← Data directory for SQLite databases (e.g. screener.db, tokens.db)
├── build/                      ← CMake build directory (created at build time)
├── config/                     ← [Planned/Virtual] Engine config (ports, thresholds, universe, screens) and secrets
├── scripts/                    ← [Planned/Virtual] Helper scripts (backfill history, run backtests)
├── tests/                      ← [Planned/Virtual] GoogleTest unit and replay tests
├── docs/                       ← [Planned/Virtual] Specifications and architectural notes
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
| HTTP / API endpoints / Web Server | `src/web/` / `include/trader/web/` | `snake_case.cpp` / `snake_case.hpp` |
| Token / Credentials Storage | `src/storage/` / `include/trader/storage/` | `snake_case.cpp` / `snake_case.hpp` |
| Screen logic | `src/screens/` | `screen_id_name.cpp` |
| Regime classification | `src/core/` | `snake_case.cpp` |
| Database store / schemas | `src/persistence/` / `include/trader/persistence/` | `snake_case.cpp` / `snake_case.hpp` |
| Database Files | `data/` | `*.db` |
| Configuration | Configured via DB seeds / environment variables | N/A |
| Frontend components | `ui/src/` | React TSX, `*.tsx`, `*.ts` |
| Unit tests | `tests/unit/` [Planned] | `*_test.cpp` |
| Integration/Replay tests | `tests/replay/` [Planned] | `*_test.cpp` |

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

The application has a unified compilation and execution script:

```bash
# Clean build and run the entire stack (React SPA UI + C++ Backend + Webview GUI)
./run.sh
```

### Manual Individual Steps

If you need to run compilation steps manually:

```bash
# 1. Build the React SPA UI (using pnpm workspaces)
pnpm --filter ui build

# 2. Configure CMake in the build/ directory
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Compile C++ Backend and Webview wrapper
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# 4. Run the engine (serves ui/dist on port 8080, launches native macOS desktop window)
./build/trader_engine
```

### Testing

Automated tests (GoogleTest) are planned under a virtual `tests/` directory but are not yet implemented in the initial phases of the project.

---

## Environment

- Saxo Bank API credentials and OAuth tokens are stored in the SQLite database `./data/tokens.db` (decrypted using a default key `c55f7b0566a4b5f6a2406d8d7b3a9242dda2e55ec3d136892a4d9950a908b4cc`).
- The application database stores instruments, regime logs, candidates, and alerts at `./data/screener.db`.
- The Webview component runs on the main thread and displays the React SPA served locally from `http://localhost:8080`.

---

## Key Documents to Read

1. This file (`codebase-map`)
2. [SPECS_v3.9.md](file:///Users/nunoribeiro/repos/cpp_screener/SPECS_v3.9.md) — The active architectural blueprint and specification (v3.9)
3. [SPECS_v2.0.md](file:///Users/nunoribeiro/repos/cpp_screener/SPECS_v2.0.md) — The previous v2.0 architectural spec
4. [SPECS_v1.0.md](file:///Users/nunoribeiro/repos/cpp_screener/SPECS_v1.0.md) — The foundational blueprint
5. Other library skills relevant to the task (`architecture`, `api-design`, `prisma-ui`)
