# Tachyon Market Screener Engine

A high-performance C++ market screener and alert engine combined with an embedded React + Vite SPA dashboard. The system classifies market regimes, evaluates screen rules, interacts with the Saxo Bank OpenAPI, and tracks active trading candidates in a SQLite database.

## System Architecture

```
                  ┌────────────────────────────────────────┐
                  │       trader_engine (C++ Binary)       │
                  │                                        │
                  │  ┌────────────┐   ┌─────────────────┐  │
                  │  │ SQLite DB  │   │  Saxo OpenAPI   │  │
                  │  │ (screener) │   │ (Token Store /  │  │
                  │  └──────┬─────┘   │  Adapter GCM)   │  │
                  │         │         └────────┬────────┘  │
                  │         ▼                  ▼           │
                  │   ┌──────────────────────────────┐     │
                  │   │   Tachyon Trader Core        │     │
                  │   │   (Regime, Screens, Loops)   │     │
                  │   └──────────────┬───────────────┘     │
                  │                  ▼                     │
                  │   ┌──────────────────────────────┐     │
                  │   │   Crow HTTP/WS Server        │     │
                  │   │   (Port 8080 / WS Stream)    │     │
                  │   └──────────────┬───────────────┘     │
                  └──────────────────┼─────────────────────┘
                                     │ Serves assets & API
                                     ▼
                        ┌─────────────────────────┐
                        │   Vite React SPA UI     │
                        │ (Dashboard / Onboarding)│
                        └─────────────────────────┘
```

---

## Prerequisites

Before building, make sure you have the following installed on your system:

### 1. System Compiler & Build Tools
- **C++20 Compiler** (GCC 10+, Clang 11+, or Apple Clang 12+)
- **CMake** (v3.15 or higher)

### 2. Node.js & Package Manager
- **Node.js** (v18 or higher)
- **pnpm** (v8 or higher)

### 3. Native Libraries
- **OpenSSL** (v1.1.1 or v3) – Required for AES-256-GCM token decryption and HTTPS client requests.
- **SQLite3** – Required for local data storage and token persistence.

---

## Installation & Setup

### 1. Install System Dependencies

#### On macOS (using Homebrew)
```bash
brew install cmake sqlite openssl@3
```

#### On Linux (Debian/Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake sqlite3 libsqlite3-dev libssl-dev pkg-config
```

### 2. Install Node Dependencies
Run the following command in the root directory to install all monorepo dependencies (this links the React app with the `@prisma/ui` components):
```bash
pnpm install
```

---

## Building the Project

The project is structured as a monorepo containing a C++ backend and a Vite React frontend in `ui/`.

### Step 1: Build the React Frontend
Build the frontend static assets so they can be served by the C++ engine:
```bash
pnpm --filter ui build
```
This will compile TypeScript, bundle components, and output the production build to [ui/dist](file:///Users/nunoribeiro/repos/cpp_screener/ui/dist).

### Step 2: Build the C++ Backend
Generate the build files and compile the C++ `trader_engine` executable:
```bash
# Generate build configuration
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the executable (using 4 parallel jobs)
cmake --build build -j4
```
This outputs the compiled executable to `build/trader_engine`.

---

## Running the Application

### 1. Verify Configuration Databases
Ensure your databases are in place:
- **Screener Store**: State, alerts, and candidate configurations will be managed at `./data/screener.db`.
- **Token Store**: Your encrypted Saxo credentials database should reside at `./data/tokens.db`.

### 2. Launch the Engine
Start the compiled engine binary:
```bash
./build/trader_engine
```

The system will:
1. Decrypt your Saxo tokens from `./data/tokens.db` (falling back to simulated quotes if credentials are empty or invalid).
2. Initialize or verify the SQLite tables in `./data/screener.db`.
3. Startup the screener loop thread.
4. Launch the Crow web server on **Port 8080**.

### 3. Open the Dashboard
Navigate to `http://localhost:8080` in your web browser. You should see the glassmorphic dark-finance dashboard displaying real-time market regimes, active screens, alerts, and the trading candidate onboarding form.
