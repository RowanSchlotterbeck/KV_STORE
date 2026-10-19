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

engine_init_result init_engine(const std::string& log_path) {
    wal.open(log_path);
    const std::uint64_t last = wal.replay(state_machine);
    return {last, wal.recovered_tail_bytes()};
}


// Line protocol front end: parse -> validate arity -> build a command ->
// (writes: log, then) StateMachine. Reads never touch the log. Response
// strings match the previous execute()-based implementation exactly; the only
// addition is ERR_IO when a write can't be persisted.
std::string process_line(const std::string& line) {
    auto args = input_parser(line);
    if (args.empty() || args[0] == "Error") {
        return "INPUT_ERROR";
    }

    const std::string& op = args[0];

    if (op == "EXIT") {
        return "EXIT";
    }

    if (op == "PUT") {
        if (args.size() != 3) { return "ERR_USAGE PUT <key> <value>"; }
        return log_then_apply(command{op, args[1], args[2]});
    }

    if (op == "GET") {
        if (args.size() != 2) { return "ERR_USAGE GET <key>"; }
        return state_machine.get(command{op, args[1], std::nullopt});
    }

    if (op == "DEL") {
        if (args.size() != 2) { return "ERR_USAGE DEL <key>"; }
        return log_then_apply(command{op, args[1], std::nullopt});
    }

    if (op == "LIST") {
        return state_machine.list();
    }

    return "ERR_UNKNOWN_CMD";
}
