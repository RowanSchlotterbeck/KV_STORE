#ifndef KV_STORE_RAFT_MESSAGES_H
#define KV_STORE_RAFT_MESSAGES_H

// Raft peer RPC messages as plain C++ domain types (Raft paper, Figure 2).
// The Protobuf wire form and conversions live in raft_convert.h.

#include <cstdint>
#include <vector>
#include "log_entry.h"

using node_id = std::uint64_t;

struct request_vote_request
{
    std::uint64_t term = 0;            // candidate's term
    node_id candidate_id = 0;          // candidate requesting the vote
    std::uint64_t last_log_index = 0;  // index of candidate's last log entry
    std::uint64_t last_log_term = 0;   // term of candidate's last log entry
};

struct request_vote_response
{
    std::uint64_t term = 0;            // current term, for the candidate to update itself
    bool vote_granted = false;
};

struct append_entries_request
{
    std::uint64_t term = 0;            // leader's term
    node_id leader_id = 0;
    std::uint64_t prev_log_index = 0;  // index of the entry immediately preceding `entries`
    std::uint64_t prev_log_term = 0;   // term of that entry
    std::vector<log_entry> entries;    // empty for a heartbeat
    std::uint64_t leader_commit = 0;   // leader's commit index
};

struct append_entries_response
{
    std::uint64_t term = 0;            // current term, for the leader to update itself
    bool success = false;              // follower had an entry matching prev_log_index/term
};

#endif //KV_STORE_RAFT_MESSAGES_H
