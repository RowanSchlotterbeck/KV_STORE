//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#ifndef KV_STORE_ENGINE_H
#define KV_STORE_ENGINE_H

#pragma once
#include <cstdint>
#include <string>
#include <asio.hpp>
#include "command.h"

struct engine_init_result
{
    std::uint64_t last_index;           // entries replayed from the log
    std::uint64_t recovered_tail_bytes; // torn tail discarded during replay (0 if clean)
};

// Opens the persistent log at `log_path` and replays it, applying entries up
// to `apply_through` to the state machine (all of them by default: plain
// mode). Cluster mode passes 0: Raft applies committed entries itself. Must
// run before any request is handled. Throws wal_error on failure.
engine_init_result init_engine(const std::string& log_path, std::uint64_t apply_through = UINT64_MAX);

// Plain (single-node) mode: one line in, one response out. Writes are logged
// (term 0) and applied immediately.
std::string process_line(const std::string& line);

// One client line, classified. Cluster mode routes reads and writes through
// Raft; `reply` requests (errors, EXIT) are answered as is.
struct client_request
{
    enum class kind { reply, read, write };
    kind type = kind::reply;
    std::string reply;  // kind::reply: the complete response
    command cmd;        // kind::read / kind::write
};
client_request parse_request(const std::string& line);

// GET against the state machine, as process_line() answers it.
std::string execute_read(const command& cmd);

// Applies a committed command to the state machine and returns its result
// ("OK", "NOT_FOUND", ...). Cluster mode's only path that changes the store.
std::string apply_command(const command& cmd);

class PersistentLog;

// The engine's persistent log, for Raft. Raft may append and truncate it
// (replication) but never applies what it stores: that's the commit step.
PersistentLog& engine_log();

#endif //KV_STORE_ENGINE_H