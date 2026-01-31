#include <iostream>
#include <unordered_map>
#include <string>
#include <algorithm>


namespace kv
{
    constexpr std::size_t MAX_KEY_LEN   = 256;
    constexpr std::size_t MAX_VALUE_LEN = 4096;

}

// Turns any string into uppercase
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

    return args;
}

// Must declare any key or value with quotes

std::string execute(

    std::unordered_map<std::string, std::string>& map,
    const std::vector<std::string>& args)
{

    // Check if line is empty
    if (args.empty())
    {
        return "ERR_EMPTY";
    }

    std::string command = to_upper(args[0]);


    if (command == "EXIT")
    {
        return "EXIT";
    }


    if (command == "PUT")
    {
        if (args.size() != 3)
        {
            return "ERR_USAGE PUT <key> <value>";
        }

        map[args[1]] = args[2];
        return "OK";

    } else if (command == "GET")
    {
        if (args.size() != 2)
        {
            return "ERR_USAGE GET <key>";
        }

        const auto it = map.find(args[1]);
        return (it != map.end()) ? it->second : "NOT_FOUND";


    } else if  (command == "DEL")
    {

        if (args.size() != 2)
        {
            return "ERR_USAGE DEL <key>";
        }

        return map.erase(args[1]) ? "OK" : "NOT_FOUND";

    } else if (command == "LIST")
    {
        std::string out;
        for (auto& [k, v] : map)
            out += k + " => " + v + "\n";

        return out.empty() ? "(empty)" : out;
    } else if  (command == "EXIT")
    {
        return "EXIT";
    }

    return "ERR_UNKNOWN_CMD";

}

int main ()
{
    std::unordered_map<std::string, std::string> store;
    std::string line;

    while (std::getline(std::cin, line)) {
        std::string result;
        auto args = input_parser(line);
        if (args[0] == "Error")
        {
            result = "INPUT_ERROR";
        } else
        {
            result = execute(store, args);
        }


        if (result == "EXIT")
            break;

        std::cout << result << std::endl;
    }
}

