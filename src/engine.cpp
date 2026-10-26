//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../include/engine.h"

#include <cstdio>
#include "../include/parser.h"
#include "../include/command.h"
#include "../include/state_machine.h"
#include "../include/wal.h"

namespace
{
    StateMachine state_machine;
    PersistentLog wal;

    // Write path: durably log the command, then apply it. A write that can't
    // be persisted is never applied or acknowledged.
    std::string log_then_apply(const command& cmd) {
        try {
            wal.append(cmd);
        } catch (const wal_error& e) {
            std::fprintf(stderr, "write rejected: %s\n", e.what());
            return "ERR_IO";
        }
        return state_machine.apply(cmd);
    }
}

engine_init_result init_engine(const std::string& log_path, std::uint64_t apply_through) {
    wal.open(log_path);
    const std::uint64_t last = wal.replay(state_machine, apply_through);
    return {last, wal.recovered_tail_bytes()};
}

PersistentLog& engine_log() {
    return wal;
}


// Line protocol front end: parse -> validate arity -> build a command.
// Response strings match the previous execute()-based implementation
// exactly; the only addition is ERR_IO when a write can't be persisted.
client_request parse_request(const std::string& line) {
    using kind = client_request::kind;
    auto args = input_parser(line);
    if (args.empty() || args[0] == "Error") {
        return {kind::reply, "INPUT_ERROR", {}};
    }

    const std::string& op = args[0];

    if (op == "EXIT") {
        return {kind::reply, "EXIT", {}};
    }

    if (op == "PUT") {
        if (args.size() != 3) { return {kind::reply, "ERR_USAGE PUT <key> <value>", {}}; }
        return {kind::write, {}, command{op, args[1], args[2]}};
    }

    if (op == "GET") {
        if (args.size() != 2) { return {kind::reply, "ERR_USAGE GET <key>", {}}; }
        return {kind::read, {}, command{op, args[1], std::nullopt}};
    }

    if (op == "DEL") {
        if (args.size() != 2) { return {kind::reply, "ERR_USAGE DEL <key>", {}}; }
        return {kind::write, {}, command{op, args[1], std::nullopt}};
    }

    return {kind::reply, "ERR_UNKNOWN_CMD", {}};
}

std::string execute_read(const command& cmd) {
    return state_machine.get(cmd);
}

std::string apply_command(const command& cmd) {
    return state_machine.apply(cmd);
}

// Plain mode: reads never touch the log; writes are logged, then applied.
std::string process_line(const std::string& line) {
    const client_request req = parse_request(line);
    switch (req.type) {
        case client_request::kind::reply: return req.reply;
        case client_request::kind::read:  return execute_read(req.cmd);
        case client_request::kind::write: return log_then_apply(req.cmd);
    }
    return "ERR_UNKNOWN_CMD";
}
