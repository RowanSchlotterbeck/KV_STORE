#include "../include/raft_node.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include "../include/wal.h"

const char* to_string(raft_role role) {
    switch (role) {
        case raft_role::follower:  return "follower";
        case raft_role::candidate: return "candidate";
        case raft_role::leader:    return "leader";
    }
    return "?";
}

log_position PersistentLogView::last() const {
    return {log_.last_index(), log_.last_term()};
}

std::optional<std::uint64_t> PersistentLogView::term_at(std::uint64_t index) const {
    return log_.term_at(index);
}

RaftNode::RaftNode(asio::io_context& io, node_id self, std::vector<node_id> peers, PeerClient& client,
                   const RaftLogView& log, raft_options options)
    : self_(self), peers_(std::move(peers)), client_(client),
      log_(log), options_(std::move(options)),
      rng_(options_.seed != 0 ? options_.seed : std::random_device{}()),
      election_timer_(io), heartbeat_timer_(io) {
    if (options_.state) {
        const raft_hard_state saved = options_.state->load();
        current_term_ = saved.term;
        voted_for_ = saved.voted_for;
    }
}

RaftNode::~RaftNode() {
    alive_.reset();
    election_timer_.cancel();
    heartbeat_timer_.cancel();
}

template <typename... Args>
void RaftNode::log(const Args&... parts) const {
    if (!options_.log) { return; }
    std::ostringstream line;
    line << "[raft node " << self_ << " term " << current_term_ << " " << to_string(role_) << "] ";
    (line << ... << parts);
    line << "\n";
    std::cerr << line.str();
}

raft_status RaftNode::status() const {
    return {self_, role_, current_term_, voted_for_, leader_id_};
}

std::map<node_id, peer_progress> RaftNode::progress() const {
    return role_ == raft_role::leader ? progress_ : std::map<node_id, peer_progress>{};
}

void RaftNode::notify() {
    if (options_.on_change) { options_.on_change(status()); }
}

void RaftNode::start() {
    std::ostringstream peers;
    for (const node_id p : peers_) { peers << " " << p; }
    log("starting; cluster of ", peers_.size() + 1, " (peers:", peers.str(), "), majority ",
        majority(), ", election timeout ", options_.election_timeout_min.count(), "-",
        options_.election_timeout_max.count(), "ms, heartbeat ", options_.heartbeat_interval.count(), "ms");
    if (options_.state) {
        log("persistent state ", options_.state->path(), ": term ", current_term_, ", voted for ",
            voted_for_ != 0 ? "node " + std::to_string(voted_for_) : std::string("none"));
    }
    notify();
    reset_election_timer();
}

PeerHandlers RaftNode::handlers() {
    std::weak_ptr<char> alive = alive_;
    PeerHandlers h;
    h.on_request_vote = [this, alive](const request_vote_request& req) {
        if (alive.expired()) { return request_vote_response{0, false}; }
        return handle_request_vote(req);
    };
    h.on_append_entries = [this, alive](const append_entries_request& req) {
        if (alive.expired()) { return append_entries_response{0, false}; }
        return handle_append_entries(req);
    };
    return h;
}

// ---- timers ----

void RaftNode::reset_election_timer() {
    std::uniform_int_distribution<std::int64_t> pick(options_.election_timeout_min.count(),
                                                     options_.election_timeout_max.count());
    const std::uint64_t gen = ++election_gen_;
    election_timer_.expires_after(std::chrono::milliseconds(pick(rng_)));
    election_timer_.async_wait([this, alive = std::weak_ptr<char>(alive_), gen](std::error_code ec) {
        if (ec || alive.expired() || gen != election_gen_) { return; }
        if (role_ != raft_role::leader) { start_election(); }
    });
}

// ---- elections ----

void RaftNode::start_election() {
    const std::uint64_t old_term = current_term_;
    persist(current_term_ + 1, self_);  // on disk before any RequestVote goes out
    role_ = raft_role::candidate;
    leader_id_ = 0;
    votes_ = {self_};
    log("election timeout: term ", old_term, " -> ", current_term_,
        ", became candidate, voted for self (1/", majority(), " votes)");
    notify();
    reset_election_timer();  // a split vote just times out again into a fresh election

    if (votes_.size() >= majority()) {  // single-node cluster
        become_leader();
        return;
    }

    const log_position mine = log_.last();
    const request_vote_request req{current_term_, self_, mine.last_index, mine.last_term};
    for (const node_id peer : peers_) {
        client_.send_request_vote(peer, req,
            [this, alive = std::weak_ptr<char>(alive_), peer, term = current_term_](
                rpc_result<request_vote_response> reply) {
                if (alive.expired()) { return; }
                on_vote_reply(peer, term, reply);
            });
    }
}

void RaftNode::on_vote_reply(node_id peer, std::uint64_t election_term,
                             const rpc_result<request_vote_response>& reply) {
    if (!reply.ok()) { return; }  // unreachable peer: no vote; the election timer covers retries
    if (reply.response.term > current_term_) {
        step_down(reply.response.term, "RequestVote reply", peer);
        return;
    }
    // Stale: an election we already won, lost, or abandoned for a newer term.
    if (role_ != raft_role::candidate || current_term_ != election_term) { return; }
    if (!reply.response.vote_granted) {
        log("node ", peer, " refused its vote");
        return;
    }
    if (!votes_.insert(peer).second) { return; }  // duplicate reply: already counted
    log("vote received from node ", peer, " (", votes_.size(), "/", majority(), ")");
    if (votes_.size() >= majority()) { become_leader(); }
}

void RaftNode::become_leader() {
    role_ = raft_role::leader;
    leader_id_ = self_;
    // Optimistic start: assume every follower matches our whole log, and let
    // failed consistency checks walk nextIndex back where that's wrong.
    const std::uint64_t next = log_.last().last_index + 1;
    progress_.clear();
    for (const node_id peer : peers_) { progress_[peer] = {next, 0}; }
    ++election_gen_;  // a leader runs no election timeout
    election_timer_.cancel();
    log("WON ELECTION with ", votes_.size(), "/", peers_.size() + 1,
        " votes; now leader, sending heartbeats");
    notify();
    send_heartbeats();
}

// ---- heartbeats ----

void RaftNode::send_heartbeats() {
    if (role_ != raft_role::leader) { return; }
    for (const node_id peer : peers_) {
        // Per-follower probe at nextIndex - 1. No entries yet, and leader_commit stays 0.
        peer_progress& p = progress_[peer];
        std::optional<std::uint64_t> prev_term = log_.term_at(p.next_index - 1);
        if (!prev_term) {
            // Our log has no entry there (it can't shrink under a leader, so
            // this is defensive): probe from our own end instead.
            p.next_index = log_.last().last_index + 1;
            prev_term = log_.term_at(p.next_index - 1);
        }
        const append_entries_request req{current_term_, self_, p.next_index - 1, prev_term.value_or(0), {}, 0};
        client_.send_append_entries(peer, req,
            [this, alive = std::weak_ptr<char>(alive_), peer, term = current_term_, prev = req.prev_log_index](
                rpc_result<append_entries_response> reply) {
                if (alive.expired()) { return; }
                on_heartbeat_reply(peer, term, prev, reply);
            });
    }

    const std::uint64_t gen = ++heartbeat_gen_;
    heartbeat_timer_.expires_after(options_.heartbeat_interval);
    heartbeat_timer_.async_wait([this, alive = std::weak_ptr<char>(alive_), gen](std::error_code ec) {
        if (ec || alive.expired() || gen != heartbeat_gen_) { return; }
        send_heartbeats();
    });
}

void RaftNode::on_heartbeat_reply(node_id peer, std::uint64_t sent_term, std::uint64_t prev_log_index,
                                  const rpc_result<append_entries_response>& reply) {
    if (!reply.ok()) { return; }  // unreachable peer: the next heartbeat retries
    if (reply.response.term > current_term_) {
        step_down(reply.response.term, "AppendEntries reply", peer);
        return;
    }
    // Stale: sent in an earlier term, or we're no longer leader.
    if (role_ != raft_role::leader || current_term_ != sent_term) { return; }
    peer_progress& p = progress_[peer];
    if (reply.response.success) {
        // Entries are empty, so prev_log_index is exactly the point known to match.
        p.match_index = std::max(p.match_index, prev_log_index);
        p.next_index = p.match_index + 1;
        return;
    }
    // Several heartbeats can be in flight with the same probe; only the reply
    // to the current one may move nextIndex, so one mismatch steps back once.
    if (prev_log_index + 1 != p.next_index) { return; }
    if (p.next_index <= 1) {
        // prev_log_index 0 always matches, so a follower can't reject it.
        log("node ", peer, " rejected prev_log_index 0; nextIndex stays 1");
        return;
    }
    p.next_index -= 1;
    log("node ", peer, " rejected prev_log_index ", prev_log_index, "; nextIndex[", peer, "] ",
        p.next_index + 1, " -> ", p.next_index);
}

// ---- term authority ----

void RaftNode::step_down(std::uint64_t new_term, const char* why, node_id from) {
    const bool was_leader = role_ == raft_role::leader;
    const raft_role old_role = role_;
    const std::uint64_t old_term = current_term_;
    persist(new_term, 0);  // on disk before the caller replies or acts on the new term
    leader_id_ = 0;
    role_ = raft_role::follower;
    votes_.clear();
    progress_.clear();
    ++heartbeat_gen_;
    heartbeat_timer_.cancel();
    const std::string source = from != 0 ? " from node " + std::to_string(from) : std::string();
    const std::string role_change = old_role != raft_role::follower
        ? std::string(", ") + to_string(old_role) + " -> follower" : std::string();
    log("saw term ", new_term, " in ", why, source, ": term ", old_term, " -> ", new_term, role_change);
    notify();
    // Followers and candidates already have an election timer running; a
    // leader had none, so it needs one now or it would never time out.
    if (was_leader) { reset_election_timer(); }
}

void RaftNode::persist(std::uint64_t term, node_id voted_for) {
    if (term == current_term_ && voted_for == voted_for_) { return; }  // e.g. a repeat vote for the same candidate
    if (options_.state) {
        try {
            options_.state->save({term, voted_for});
        } catch (const raft_state_error& e) {
            // Continuing would mean sending or replying on a term/vote a
            // restart could forget, which is exactly how double votes happen.
            std::cerr << "[raft node " << self_ << "] FATAL: cannot persist term " << term
                      << ", voted for " << voted_for << ": " << e.what() << "\n";
            std::abort();
        }
    }
    current_term_ = term;
    voted_for_ = voted_for;
}

// ---- RPC handlers ----

request_vote_response RaftNode::handle_request_vote(const request_vote_request& req) {
    if (req.term > current_term_) { step_down(req.term, "RequestVote", req.candidate_id); }
    if (req.term < current_term_) {
        log("refused vote to node ", req.candidate_id, ": its term ", req.term, " is stale");
        return {current_term_, false};
    }

    const log_position mine = log_.last();
    const bool up_to_date = req.last_log_term > mine.last_term
        || (req.last_log_term == mine.last_term && req.last_log_index >= mine.last_index);
    const bool free_to_vote = voted_for_ == 0 || voted_for_ == req.candidate_id;

    if (free_to_vote && up_to_date) {
        persist(current_term_, req.candidate_id);  // on disk before the grant is returned
        log("granted vote to node ", req.candidate_id);
        notify();
        reset_election_timer();
        return {current_term_, true};
    }
    if (!free_to_vote) {
        log("refused vote to node ", req.candidate_id, ": already voted for node ", voted_for_);
    } else {
        log("refused vote to node ", req.candidate_id, ": its log (", req.last_log_index, "@t",
            req.last_log_term, ") is behind mine (", mine.last_index, "@t", mine.last_term, ")");
    }
    return {current_term_, false};
}

append_entries_response RaftNode::handle_append_entries(const append_entries_request& req) {
    if (req.term > current_term_) { step_down(req.term, "AppendEntries", req.leader_id); }
    if (req.term < current_term_) { return {current_term_, false}; }

    if (role_ == raft_role::leader) {
        // Two leaders in one term would violate Election Safety; never expected.
        log("ERROR: AppendEntries from node ", req.leader_id, " claiming leadership of my own term");
        return {current_term_, false};
    }
    if (role_ == raft_role::candidate) {
        role_ = raft_role::follower;
        votes_.clear();
        log("node ", req.leader_id, " won term ", current_term_, "; candidate -> follower");
    }
    if (leader_id_ != req.leader_id) {
        leader_id_ = req.leader_id;
        log("following leader node ", leader_id_);
        notify();
    }
    // A current-term leader is legitimate even when our logs disagree, so the
    // timer resets either way; success reports only the consistency check.
    reset_election_timer();

    // prev_log_index 0 is the empty prefix every log shares: always a match.
    if (req.prev_log_index == 0) { return {current_term_, true}; }
    const std::optional<std::uint64_t> mine = log_.term_at(req.prev_log_index);
    if (!mine) {
        log("rejected AppendEntries from node ", req.leader_id, ": no entry at prev_log_index ",
            req.prev_log_index, " (log ends at ", log_.last().last_index, ")");
        return {current_term_, false};
    }
    if (*mine != req.prev_log_term) {
        log("rejected AppendEntries from node ", req.leader_id, ": entry at prev_log_index ",
            req.prev_log_index, " has term ", *mine, ", leader expects ", req.prev_log_term);
        return {current_term_, false};
    }
    return {current_term_, true};
}
