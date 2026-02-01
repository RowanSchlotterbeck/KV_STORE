cmake_minimum_required(VERSION 3.16)
project(KV_STORE)

set(CMAKE_CXX_STANDARD 20)

add_library(kv_core
        src/kv_store.cpp
        src/parser.cpp
        src/engine.cpp
)

target_include_directories(kv_core PRIVATE
        include
        network
        /opt/homebrew/opt/asio/include
)


target_compile_definitions(kv_core PRIVATE
        ASIO_STANDALONE
)

find_package(Threads REQUIRED)
target_link_libraries(kv_core PRIVATE Threads::Threads)

add_executable(kv_server
        src/server.cpp
)

target_link_libraries(kv_server PRIVATE kv_core)

target_include_directories(kv_server PRIVATE
        include
        network
        /opt/homebrew/opt/asio/include
)

add_executable(kv_client
        src/client.cpp
)

target_link_libraries(kv_client PRIVATE kv_core)

target_include_directories(kv_client PRIVATE
        include
        network
        /opt/homebrew/opt/asio/include
)





