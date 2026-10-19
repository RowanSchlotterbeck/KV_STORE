//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../include/engine.h"

#include <unordered_map>
#include "../include/parser.h"
#include "../include/command.h"


// A state machine is an internal component of every raft node and it's job is to be the internal map for the node based on the log

class StateMachine {
public:
    std::string apply(const command& cmd) {
        if (cmd.operation == "PUT") { return set(cmd); }
        if (cmd.operation == "DEL") { return del(cmd); }
        return "ERR_UNKNOWN_CMD";
    }

    std::string get(const command& cmd) const {
        if (cmd.key.empty()) { return "ERR_INVALID"; }
        auto it = store.find(cmd.key);
        return (it != store.end()) ? it->second : "NOT_FOUND";
    }

    std::string list() const {
        std::string out;
        for (const auto& [k, v] : store)
            out += k + " => " + v + "\n";
        return out.empty() ? "(empty)" : out;
    }

private:
    std::unordered_map<std::string, std::string> store;

    std::string set(const command& cmd) {
        if (cmd.key.empty())           { return "ERR_INVALID"; }
        if (!cmd.value.has_value())    { return "ERR_INVALID"; }
        store[cmd.key] = *cmd.value;
        return "OK";
    }

    std::string del(const command& cmd) {
        if (cmd.key.empty()) { return "ERR_INVALID"; }
        return store.erase(cmd.key) ? "OK" : "NOT_FOUND";
    }
};


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
