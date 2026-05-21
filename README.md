# Tachyon Market Screener Engine

A high-performance C++ market screener and alert engine with an **integrated native desktop GUI window** running the Vite React + Prisma UI dashboard. The system classifies market regimes, evaluates screen rules, interacts with the Saxo Bank OpenAPI, and tracks active trading candidates in a SQLite database.

## System Architecture

```
                       ┌───────────────────────────────┐
                       │  trader_engine (Main Process) │
                       │                               │
                       │     Native Cocoa/WebKit GUI   │
                       │       (Embedded Window)       │
                       └──────────────┬────────────────┘
                                      │ Displays UI
                                      ▼
           ┌─────────────────────────────────────────────────────┐
           │            Multithreaded C++ Services               │
           │                                                     │
           │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐  │
           │  │ Crow HTTP/WS │ │  SQLite DB   │ │ Saxo OpenAPI │  │
           │  │  (Port 8080) │ │  (screener)  │ │ (AES Decrypt)│  │
           │  └──────────────┘ └──────────────┘ └──────────────┘  │
           └─────────────────────────────────────────────────────┘
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
- **macOS Cocoa & WebKit frameworks** (Included by default on macOS).

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
sudo apt-get install -y build-essential cmake sqlite3 libsqlite3-dev libssl-dev pkg-config libgtk-3-dev libwebkit2gtk-4.0-dev
```

### 2. Install Node Dependencies
Run the following command in the root directory to install all monorepo dependencies (this links the React app with the `@prisma/ui` components):
```bash
pnpm install
```

---

## Quick Start: Build & Run in One Command

You can build the entire monorepo (both the React frontend and the C++ desktop app) and run it with a single script:

```bash
./run.sh
```

This script:
1. Installs/compiles the React SPA using `pnpm` workspace filters.
2. Generates the CMake configuration in the `build/` folder.
3. Compiles the C++ engine utilizing all available CPU cores.
4. Executes `./build/trader_engine`.

---

## Manual Building (Step-by-Step)

If you prefer to compile the frontend and C++ engine separately:

### Step 1: Build the React Frontend
```bash
pnpm --filter ui build
```
This outputs the compiled assets to [ui/dist](file:///Users/nunoribeiro/repos/cpp_screener/ui/dist).

### Step 2: Build the C++ Backend & GUI
```bash
# Generate build configuration inside build/
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile the binary
cmake --build build -j4
```

---

## Running the Application

### 1. Verify Databases
Ensure the databases exist inside the `data/` directory:
- **Screener Store**: `./data/screener.db`
- **Token Store**: `./data/tokens.db`

### 2. Telegram Bot Configuration
The engine supports broadcasting alerts to Telegram channels. To activate the Telegram bot (`t.me/falling_knives_bot`):
1. **Chat Channels Setup**: Create three Telegram channels or groups:
   - `Premium` alerts (interrupt-worthy)
   - `Opportunity` alerts (interesting screen setups)
   - `Digest` alerts (EOD digests and logs)
2. **Admin Access**: Add the bot to these channels/groups and grant it administrator permissions (to send messages).
3. **Environment Setup**: Add your API tokens and chat IDs to your environment or create a `.env` file (or `config/secrets.env`):
   ```env
   TELEGRAM_BOT_TOKEN=8859988952:AAFkaAh9tyyTxuw5Rezqeab7Pnko67D3m24
   TG_CHAT_PREMIUM=<your_premium_chat_id>
   TG_CHAT_OPPORTUNITY=<your_opportunity_chat_id>
   TG_CHAT_DIGEST=<your_digest_chat_id>
   ```
4. **Configuration Mapping**: The mapping from environment variables to the engine is defined in `config/config.yaml`:
   ```yaml
   telegram:
     bot_token_env: TELEGRAM_BOT_TOKEN
     chat_premium_env: TG_CHAT_PREMIUM
     chat_opportunity_env: TG_CHAT_OPPORTUNITY
     chat_digest_env: TG_CHAT_DIGEST
     mode: long_polling # or "webhook"
   ```

### 3. Launch the Engine Manually
```bash
./build/trader_engine
```

#### Execution Modes:
- **GUI Desktop Mode (Default)**: On machines with a display server (e.g. macOS or local Linux/Windows desktop), a native application window launches, loading the dashboard UI directly. Closing the window gracefully terminates the background screener.
- **Headless Daemon Mode**: If run in an environment without a display server (e.g. a remote cloud droplet VM), the application automatically detects the absence of a display, bypasses GUI initialization, and runs as a headless background service (accessible via `http://<your-ip>:8080` in an external browser).
