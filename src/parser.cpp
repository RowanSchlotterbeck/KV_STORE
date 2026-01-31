//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../include/parser.h"

namespace kv
{
    constexpr std::size_t MAX_KEY_LEN   = 256;
    constexpr std::size_t MAX_VALUE_LEN = 4096;

}

static std::string to_upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),[](unsigned char c){ return std::toupper(c); });
    return s;
}

std::vector<std::string> input_parser(const std::string& line)
{
    std::vector<std::string> args;
    std::string current;
    bool in_quotes = false;
    int quote_counter = 0;

    for (const char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            quote_counter++;
        } else if (std::isspace(c) && !in_quotes) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }

    if (!current.empty())
    {
        args.push_back(current);
    }

    if (args.empty()) return {"Error"};

    if (args[0] == "PUT" && (quote_counter != 4 || args[1].length() > kv::MAX_KEY_LEN || args[2].length() > kv::MAX_VALUE_LEN))
    {
        return {"Error"};
    } else if (args[0] == "GET" && quote_counter != 2)
    {
        return {"Error"};
    } else if (args[0] == "DEL" && quote_counter != 2)
    {
        return {"Error"};
    }

    // Still need to implement RESP

    args[0] = to_upper(args[0]);

    return args;
}
