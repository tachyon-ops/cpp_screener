#!/bin/bash
# Exit immediately if any command fails
set -e

# Formatting colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}[1/4] Building React SPA Frontend...${NC}"
if ! command -v pnpm &> /dev/null; then
    echo -e "${RED}Error: pnpm is not installed. Please install it or use npm install -g pnpm.${NC}"
    exit 1
fi
pnpm --filter ui build

echo -e "${BLUE}[2/4] Configuring C++ CMake system in build/...${NC}"
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo -e "${BLUE}[3/4] Compiling C++ Backend & GUI...${NC}"
# Use all available CPU cores for compilation
CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake --build build -j$CPU_CORES

echo -e "${GREEN}[4/4] Launching Tachyon Trading Screener...${NC}"
./build/trader_engine
