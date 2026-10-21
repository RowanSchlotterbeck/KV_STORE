#ifndef KV_STORE_RAFT_NODE_H
#define KV_STORE_RAFT_NODE_H

// Raft leader election and heartbeats (Raft paper, Figure 2), and nothing
// more: no log replication, no commit index, no log-matching check on
// AppendEntries, no client routing.
//
// Everything runs on the caller's io_context: the RPC handlers, the RPC
// reply callbacks, and both timers. No threads, no locks.
//
// current_term and voted_for are kept in memory as the working copy and, when
// raft_options::state is set, written durably through it before the node acts
// on a new value: before granting a vote, before sending RequestVote for a new
// election, and before acting on a higher term seen in any RPC. A node that
// restarts mid-term therefore still remembers its vote. If a save fails the
// node crash-stops (aborts) rather than run on a term/vote that isn't on disk.

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <vector>
#include "raft_messages.h"
#include "raft_state.h"
#include "../network/peer_transport.h"

constexpr std::chrono::milliseconds kElectionTimeoutMin{150};
constexpr std::chrono::milliseconds kElectionTimeoutMax{300};
constexpr std::chrono::milliseconds kHeartbeatInterval{50};  // well under kElectionTimeoutMin

enum class raft_role { follower, candidate, leader };
const char* to_string(raft_role role);

struct log_position
{
    std::uint64_t last_index = 0;
    std::uint64_t last_term = 0;
};

struct raft_status
{
    node_id id = 0;
    raft_role role = raft_role::follower;
    std::uint64_t term = 0;
    node_id voted_for = 0;
    node_id leader_id = 0;  // leader known for `term`, 0 if none yet
};

struct raft_options
{
    // Each election timeout is drawn uniformly from [min, max].
    std::chrono::milliseconds election_timeout_min = kElectionTimeoutMin;
    std::chrono::milliseconds election_timeout_max = kElectionTimeoutMax;
    std::chrono::milliseconds heartbeat_interval = kHeartbeatInterval;
    std::uint64_t seed = 0;     // RNG seed for timeouts; 0 = random_device
    bool log = true;            // human-readable transitions on stderr
    // Called (on the io_context thread) after every role, term or leader change.
    std::function<void(const raft_status&)> on_change;
    // Durable backing for current_term/voted_for, loaded by the constructor
    // and must outlive the node. Null keeps them in memory only (unit tests
    // that don't exercise restarts); the server always sets it.
    RaftStateFile* state = nullptr;
};

class RaftNode
{
public:
    using log_position_fn = std::function<log_position()>;

    // `peers` is every other node in the cluster; the cluster size used for
    // majorities is peers.size() + 1. `client` must outlive the node.
    // Throws raft_state_error if options.state can't be loaded.
    RaftNode(asio::io_context& io, node_id self, std::vector<node_id> peers, PeerClient& client,
             log_position_fn local_log, raft_options options = {});
    ~RaftNode();
    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    // Starts as a follower with a randomized election timer.
    void start();

    // Handlers for PeerServer. Safe to outlive the node: once it's destroyed
    // they reply {term 0, refused} without touching it.
    PeerHandlers handlers();

    request_vote_response handle_request_vote(const request_vote_request& req);
    append_entries_response handle_append_entries(const append_entries_request& req);

    raft_status status() const;
    raft_role role() const { return role_; }
    std::uint64_t current_term() const { return current_term_; }
    std::size_t majority() const { return (peers_.size() + 1) / 2 + 1; }

private:
    void reset_election_timer();
    void start_election();
    void on_vote_reply(node_id peer, std::uint64_t election_term,
                       const rpc_result<request_vote_response>& reply);
    void become_leader();
    void send_heartbeats();
    void on_heartbeat_reply(const rpc_result<append_entries_response>& reply);
    void step_down(std::uint64_t new_term, const char* why, node_id from);
    // Durably records term/vote, then updates the in-memory copy.
    void persist(std::uint64_t term, node_id voted_for);
    void notify();
    template <typename... Args> void log(const Args&... parts) const;

    node_id self_;
    std::vector<node_id> peers_;
    PeerClient& client_;
    log_position_fn local_log_;
    raft_options options_;
    std::mt19937_64 rng_;

    std::uint64_t current_term_ = 0;
    node_id voted_for_ = 0;
    raft_role role_ = raft_role::follower;
    node_id leader_id_ = 0;
    std::set<node_id> votes_;  // granted to us in current_term_ (candidate only)

    asio::steady_timer election_timer_;
    asio::steady_timer heartbeat_timer_;
    // Bumped on every re-arm/cancel; a handler from an older arming is a no-op
    // even if it was already queued when the timer was reset.
    std::uint64_t election_gen_ = 0;
    std::uint64_t heartbeat_gen_ = 0;

    // Weakly captured by every callback; expires when the node is destroyed.
    std::shared_ptr<char> alive_ = std::make_shared<char>();
};

#endif //KV_STORE_RAFT_NODE_H
