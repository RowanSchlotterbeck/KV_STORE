#ifndef KV_STORE_LOG_ENTRY_H
#define KV_STORE_LOG_ENTRY_H

#include <cstdint>
#include "command.h"

struct log_entry
{
    std::uint64_t index = 0;
    std::uint64_t term = 0;
    command cmd;
};

#endif //KV_STORE_LOG_ENTRY_H
