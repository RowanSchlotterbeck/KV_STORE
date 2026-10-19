#ifndef KV_STORE_COMMAND_H
#define KV_STORE_COMMAND_H

#include <optional>
#include <string>

// Internal domain type for a state-machine command. Plain C++ only; the
// Protobuf representation lives at the serialization boundary.
struct command
{
    std::string operation;
    std::string key;
    std::optional<std::string> value;
};

#endif //KV_STORE_COMMAND_H
