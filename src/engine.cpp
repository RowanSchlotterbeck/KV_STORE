//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../include/engine.h"

#include "../include/parser.h"
#include "../include/command.h"
#include "../include/state_machine.h"


// Line protocol front end: parse -> validate arity -> build a command -> StateMachine.
// Response strings match the previous execute()-based implementation exactly.
std::string process_line(const std::string& line) {
    static StateMachine state_machine;

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
        return state_machine.apply(command{op, args[1], args[2]});
    }

    if (op == "GET") {
        if (args.size() != 2) { return "ERR_USAGE GET <key>"; }
        return state_machine.get(command{op, args[1], std::nullopt});
    }

    if (op == "DEL") {
        if (args.size() != 2) { return "ERR_USAGE DEL <key>"; }
        return state_machine.apply(command{op, args[1], std::nullopt});
    }

    if (op == "LIST") {
        return state_machine.list();
    }

    return "ERR_UNKNOWN_CMD";
}
