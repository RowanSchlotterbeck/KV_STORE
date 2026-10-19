cmake_minimum_required(VERSION 3.16)
project(KV_STORE)

set(CMAKE_CXX_STANDARD 20)

include(cmake/protobuf.cmake)

# --- Protobuf generated code (serialization boundary only) ---
set(KV_PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/proto/kvstore.proto)
set(KV_PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${KV_PROTO_OUT})
add_custom_command(
        OUTPUT ${KV_PROTO_OUT}/kvstore.pb.cc ${KV_PROTO_OUT}/kvstore.pb.h
        COMMAND $<TARGET_FILE:protobuf::protoc>
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                --cpp_out=${KV_PROTO_OUT}
                ${KV_PROTO_SRC}
        DEPENDS ${KV_PROTO_SRC} protobuf::protoc
        COMMENT "Generating C++ from kvstore.proto"
        VERBATIM
)

add_library(kv_proto STATIC ${KV_PROTO_OUT}/kvstore.pb.cc)
target_include_directories(kv_proto PUBLIC ${KV_PROTO_OUT})
target_link_libraries(kv_proto PUBLIC protobuf::libprotobuf)

add_library(kv_core
        src/kv_store.cpp
        src/parser.cpp
        src/engine.cpp
        src/serialization.cpp
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
target_link_libraries(kv_core PRIVATE Threads::Threads kv_proto)

add_executable(kv_server
        src/server.cpp
        src/wal.cpp
        include/wal.h
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

add_executable(kv_bench_engine
        bench/bench_engine.cpp
)

target_link_libraries(kv_bench_engine PRIVATE kv_core)

target_include_directories(kv_bench_engine PRIVATE
        include
        /opt/homebrew/opt/asio/include
)

add_executable(kv_bench_network
        bench/bench_network.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(kv_bench_network PRIVATE Threads::Threads)

target_include_directories(kv_bench_network PRIVATE
        /opt/homebrew/opt/asio/include
)

target_compile_definitions(kv_bench_network PRIVATE
        ASIO_STANDALONE
)

# --- Tests ---
enable_testing()

add_executable(kv_serialization_test
        tests/serialization_test.cpp
)

target_link_libraries(kv_serialization_test PRIVATE kv_core kv_proto)

target_include_directories(kv_serialization_test PRIVATE
        include
)

add_test(NAME serialization COMMAND kv_serialization_test)

add_executable(kv_parser_test
        tests/parser_test.cpp
)

target_link_libraries(kv_parser_test PRIVATE kv_core)

target_include_directories(kv_parser_test PRIVATE
        include
        /opt/homebrew/opt/asio/include
)

add_test(NAME parser COMMAND kv_parser_test)
