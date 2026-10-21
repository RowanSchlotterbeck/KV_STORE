#ifndef KV_STORE_RAFT_NODE_H
#define KV_STORE_RAFT_NODE_H

// Raft leader election, heartbeats and the AppendEntries log-consistency
// check (Raft paper, Figure 2), and nothing more: heartbeats carry no
// entries, so no log replication, no commit index, no client routing yet.
//
// Each heartbeat is a probe. The leader keeps nextIndex/matchIndex per
// follower and sends prev_log_index = nextIndex - 1 with the term its own log
// has there; a follower succeeds only if its log has that entry with that
// term. On failure the leader steps nextIndex back by one, so successive
// heartbeats walk back to the last index the two logs agree on.
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
#include <map>
#include <memory>
#include <optional>
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

class PersistentLog;

struct log_position
{
    std::uint64_t last_index = 0;
    std::uint64_t last_term = 0;
};

// Read-only view of the local log, as Raft needs it.
class RaftLogView
{
public:
    virtual ~RaftLogView() = default;
    virtual log_position last() const = 0;
    // Term of the entry at `index`: 0 for index 0, nullopt past the end.
    virtual std::optional<std::uint64_t> term_at(std::uint64_t index) const = 0;
};

// RaftLogView over the node's PersistentLog. `log` must outlive the view.
class PersistentLogView final : public RaftLogView
{
public:
    explicit PersistentLogView(const PersistentLog& log) : log_(log) {}
    log_position last() const override;
    std::optional<std::uint64_t> term_at(std::uint64_t index) const override;

private:
    const PersistentLog& log_;
};

// A leader's view of one follower (Figure 2, "volatile state on leaders").
struct peer_progress
{
    std::uint64_t next_index = 1;   // next log index to send; prev_log_index = next_index - 1
    std::uint64_t match_index = 0;  // highest index known to match the leader's log
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
    // `peers` is every other node in the cluster; the cluster size used for
    // majorities is peers.size() + 1. `client` and `log` must outlive the node.
    // Throws raft_state_error if options.state can't be loaded.
    RaftNode(asio::io_context& io, node_id self, std::vector<node_id> peers, PeerClient& client,
             const RaftLogView& log, raft_options options = {});
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
    // Per-follower progress while leader; empty otherwise.
    std::map<node_id, peer_progress> progress() const;

private:
    void reset_election_timer();
    void start_election();
    void on_vote_reply(node_id peer, std::uint64_t election_term,
                       const rpc_result<request_vote_response>& reply);
    void become_leader();
    void send_heartbeats();
    void on_heartbeat_reply(node_id peer, std::uint64_t sent_term, std::uint64_t prev_log_index,
                            const rpc_result<append_entries_response>& reply);
    void step_down(std::uint64_t new_term, const char* why, node_id from);
    // Durably records term/vote, then updates the in-memory copy.
    void persist(std::uint64_t term, node_id voted_for);
    void notify();
    template <typename... Args> void log(const Args&... parts) const;

    node_id self_;
    std::vector<node_id> peers_;
    PeerClient& client_;
    const RaftLogView& log_;
    raft_options options_;
    std::mt19937_64 rng_;

    std::uint64_t current_term_ = 0;
    node_id voted_for_ = 0;
    raft_role role_ = raft_role::follower;
    node_id leader_id_ = 0;
    std::set<node_id> votes_;  // granted to us in current_term_ (candidate only)
    std::map<node_id, peer_progress> progress_;  // leader only; reset on every win

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
