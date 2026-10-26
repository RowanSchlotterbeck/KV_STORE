# Provides:
#   protobuf::libprotobuf  - runtime library
#   protobuf::protoc       - compiler (an executable target, never whatever is on PATH)
#
# Default: build a pinned protobuf + abseil from source via FetchContent so a
# fresh clone needs nothing preinstalled and protoc always matches the runtime.
# Set -DKV_STORE_USE_SYSTEM_PROTOBUF=ON to use an installed package instead
# (e.g. `brew install protobuf`), which skips the long first build.

option(KV_STORE_USE_SYSTEM_PROTOBUF "Use an installed Protobuf instead of FetchContent" OFF)

if(KV_STORE_USE_SYSTEM_PROTOBUF)
    find_package(Protobuf CONFIG REQUIRED)
    message(STATUS "KV_STORE: using system Protobuf ${Protobuf_VERSION}")
    return()
endif()

include(FetchContent)

# Abseil first: protobuf's build skips its own abseil when absl::strings exists.
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(absl
        URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz
        URL_HASH SHA256=f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_ABSL_PROVIDER "module" CACHE STRING "" FORCE)
FetchContent_Declare(protobuf
        URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v29.3.tar.gz
        URL_HASH SHA256=008a11cc56f9b96679b4c285fd05f46d317d685be3ab524b2a310be0fbad987e
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(absl protobuf)
message(STATUS "KV_STORE: using FetchContent Protobuf v29.3")
