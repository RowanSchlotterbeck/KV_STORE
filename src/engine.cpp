//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../include/engine.h"
#include <unordered_map>
#include "../include/parser.h"
#include "../include/kv_store.h"


static std::unordered_map<std::string, std::string> store;

std::string process_line(const std::string& line) {
    auto args = input_parser(line);
    if (args.empty() || args[0] == "Error") {
        return "INPUT_ERROR";
    }
    return execute(store, args);
}



