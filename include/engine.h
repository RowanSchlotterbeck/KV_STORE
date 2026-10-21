//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#ifndef KV_STORE_ENGINE_H
#define KV_STORE_ENGINE_H

#pragma once
#include <cstdint>
#include <string>
#include <asio.hpp>

struct engine_init_result
{
    std::uint64_t last_index;           // entries replayed from the log
    std::uint64_t recovered_tail_bytes; // torn tail discarded during replay (0 if clean)
};

// Opens the persistent log at `log_path` and replays it into the state machine.
// Must run before process_line() handles writes. Throws wal_error on failure.
engine_init_result init_engine(const std::string& log_path);

std::string process_line(const std::string& line);

class PersistentLog;

// The engine's persistent log, read-only, for Raft (last index/term, term_at).
const PersistentLog& engine_log();

#endif //KV_STORE_ENGINE_H