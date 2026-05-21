cmake_minimum_required(VERSION 3.15)
project(trader_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Search hints for Homebrew installations on macOS
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
    set(SQLite3_ROOT "/opt/homebrew/opt/sqlite")
endif()

# Find system packages
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# Include FetchContent for package management
include(FetchContent)

# 1. Fetch nlohmann_json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# 2. Fetch cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

# 3. Fetch Asio (standalone library for Crow)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)
include_directories(SYSTEM "${asio_SOURCE_DIR}/asio/include")

# 4. Fetch Crow (HTTP & WebSocket server)
FetchContent_Declare(
    crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
)
# Crow has some dependencies or options we want to configure
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "Build Crow examples" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "Build Crow tests" FORCE)
FetchContent_MakeAvailable(crow)
set_target_properties(Crow PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${crow_SOURCE_DIR}/include>")

# 5. Fetch Webview
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG master
)
FetchContent_MakeAvailable(webview)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SQLite3_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

# Source files
set(SOURCE_FILES
    src/main.cpp
    src/broker/saxo_adapter.cpp
    src/storage/token_store.cpp
    src/persistence/sqlite_store.cpp
    src/web/http_server.cpp
    src/core/regime_classifier.cpp
    src/screens/screen_d.cpp
    src/screens/screen_b.cpp
    src/core/telegram.cpp
    src/core/alert_dispatcher.cpp
    src/storage/time_series_store.cpp
    src/screens/screen_a.cpp
    src/screens/screen_e.cpp
    src/screens/screen_f.cpp
    src/core/sr_provider.cpp
    src/screens/screen_g.cpp
    src/screens/screen_c.cpp
    src/core/position_manager.cpp
)

# Executable target
add_executable(trader_engine ${SOURCE_FILES})

# Link libraries
target_link_libraries(trader_engine
    PRIVATE
        SQLite3::SQLite3
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        httplib::httplib
        Crow
        webview::core
)

# Enable cpp-httplib to use OpenSSL for HTTPS and define ASIO_STANDALONE for Crow
target_compile_definitions(trader_engine PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT ASIO_STANDALONE)

