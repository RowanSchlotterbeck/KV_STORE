//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../include/kv_store.h"

std::string execute(

    std::unordered_map<std::string, std::string>& map,
    const std::vector<std::string>& args)
{

    // Check if line is empty
    if (args.empty())
    {
        return "ERR_EMPTY";
    }

    const std::string& command = args[0];


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
