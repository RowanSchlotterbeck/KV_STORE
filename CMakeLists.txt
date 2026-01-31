cmake_minimum_required(VERSION 4.1)
project(KV_STORE)

set(CMAKE_CXX_STANDARD 20)

add_executable(KV_STORE src/main.cpp
        src/kv_store.cpp
        include/kv_store.h
        src/parser.cpp
        include/parser.h
        src/server.cpp
        network/server.h
        src/client.cpp
        network/client.h)
