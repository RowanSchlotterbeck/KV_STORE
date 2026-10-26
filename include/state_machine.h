#ifndef KV_STORE_STATE_MACHINE_H
#define KV_STORE_STATE_MACHINE_H

#include <string>
#include <unordered_map>
#include "command.h"

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

    // Number of keys stored (for tests comparing whole stores).
    std::size_t size() const { return store.size(); }

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

#endif //KV_STORE_STATE_MACHINE_H
