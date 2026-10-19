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
    bool token_quoted = false; // current token contained a quote, so "" yields an empty token
    int quote_counter = 0;

    for (const char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            token_quoted = true;
            quote_counter++;
        } else if (std::isspace(c) && !in_quotes) {
            if (!current.empty() || token_quoted) {
                args.push_back(current);
            }
            current.clear();
            token_quoted = false;
        } else {
            current.push_back(c);
        }
    }

    if (!current.empty() || token_quoted)
    {
        args.push_back(current);
    }

    // An empty quoted string is only meaningful as a PUT value: PUT "<key>" "".
    // Everywhere else empty tokens are dropped, exactly as before.
    const bool put_with_empty_value = args.size() == 3 && to_upper(args[0]) == "PUT"
                                      && !args[1].empty() && args[2].empty();
    if (!put_with_empty_value)
    {
        std::erase_if(args, [](const std::string& a) { return a.empty(); });
    }

    if (args.empty()) return {"Error"};

    // Too few args with balanced quotes falls through to process_line's ERR_USAGE.
    if (args[0] == "PUT" && (quote_counter != 4 ||
        (args.size() >= 3 && (args[1].length() > kv::MAX_KEY_LEN || args[2].length() > kv::MAX_VALUE_LEN))))
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
