#include "../include/raft_node.h"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>

const char* to_string(raft_role role) {
    switch (role) {
        case raft_role::follower:  return "follower";
        case raft_role::candidate: return "candidate";
        case raft_role::leader:    return "leader";
    }
    return "?";
}

RaftNode::RaftNode(asio::io_context& io, node_id self, std::vector<node_id> peers, PeerClient& client,
                   log_position_fn local_log, raft_options options)
    : self_(self), peers_(std::move(peers)), client_(client),
      local_log_(std::move(local_log)), options_(std::move(options)),
      rng_(options_.seed != 0 ? options_.seed : std::random_device{}()),
      election_timer_(io), heartbeat_timer_(io) {}

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

void RaftNode::notify() {
    if (options_.on_change) { options_.on_change(status()); }
}

void RaftNode::start() {
    std::ostringstream peers;
    for (const node_id p : peers_) { peers << " " << p; }
    log("starting; cluster of ", peers_.size() + 1, " (peers:", peers.str(), "), majority ",
        majority(), ", election timeout ", options_.election_timeout_min.count(), "-",
        options_.election_timeout_max.count(), "ms, heartbeat ", options_.heartbeat_interval.count(), "ms");
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
    current_term_ += 1;
    role_ = raft_role::candidate;
    voted_for_ = self_;
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

    const log_position mine = local_log_();
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
    const log_position mine = local_log_();
    // prev_log_* and leader_commit are best-effort values; nobody checks them yet.
    const append_entries_request req{current_term_, self_, mine.last_index, mine.last_term, {}, 0};
    for (const node_id peer : peers_) {
        client_.send_append_entries(peer, req,
            [this, alive = std::weak_ptr<char>(alive_)](rpc_result<append_entries_response> reply) {
                if (alive.expired()) { return; }
                on_heartbeat_reply(reply);
            });
    }

    const std::uint64_t gen = ++heartbeat_gen_;
    heartbeat_timer_.expires_after(options_.heartbeat_interval);
    heartbeat_timer_.async_wait([this, alive = std::weak_ptr<char>(alive_), gen](std::error_code ec) {
        if (ec || alive.expired() || gen != heartbeat_gen_) { return; }
        send_heartbeats();
    });
}

void RaftNode::on_heartbeat_reply(const rpc_result<append_entries_response>& reply) {
    // success=false at an equal term means nothing yet (no log matching this chunk).
    if (reply.ok() && reply.response.term > current_term_) {
        step_down(reply.response.term, "AppendEntries reply", 0);
    }
}

// ---- term authority ----

void RaftNode::step_down(std::uint64_t new_term, const char* why, node_id from) {
    const bool was_leader = role_ == raft_role::leader;
    const raft_role old_role = role_;
    const std::uint64_t old_term = current_term_;
    current_term_ = new_term;
    voted_for_ = 0;
    leader_id_ = 0;
    role_ = raft_role::follower;
    votes_.clear();
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

// ---- RPC handlers ----

request_vote_response RaftNode::handle_request_vote(const request_vote_request& req) {
    if (req.term > current_term_) { step_down(req.term, "RequestVote", req.candidate_id); }
    if (req.term < current_term_) {
        log("refused vote to node ", req.candidate_id, ": its term ", req.term, " is stale");
        return {current_term_, false};
    }

    const log_position mine = local_log_();
    const bool up_to_date = req.last_log_term > mine.last_term
        || (req.last_log_term == mine.last_term && req.last_log_index >= mine.last_index);
    const bool free_to_vote = voted_for_ == 0 || voted_for_ == req.candidate_id;

    if (free_to_vote && up_to_date) {
        voted_for_ = req.candidate_id;
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
    reset_election_timer();
    return {current_term_, true};
}
