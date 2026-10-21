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

log_position PersistentRaftLog::last() const {
    return {log_.last_index(), log_.last_term()};
}

std::optional<std::uint64_t> PersistentRaftLog::term_at(std::uint64_t index) const {
    return log_.term_at(index);
}

std::vector<log_entry> PersistentRaftLog::entries(std::uint64_t from, std::size_t max_count,
                                                  std::size_t max_bytes) const {
    return log_.read_entries(from, max_count, max_bytes);
}

void PersistentRaftLog::truncate_from(std::uint64_t index) {
    log_.truncate_from(index);
}

void PersistentRaftLog::append(std::span<const log_entry> entries) {
    log_.append_entries(entries);
}

RaftNode::RaftNode(asio::io_context& io, node_id self, std::vector<node_id> peers, PeerClient& client,
                   RaftLog& log, raft_options options)
    : self_(self), peers_(std::move(peers)), client_(client),
      log_(log), options_(std::move(options)),
      rng_(options_.seed != 0 ? options_.seed : std::random_device{}()),
      election_timer_(io), heartbeat_timer_(io) {
    if (options_.state) {
        const raft_hard_state saved = options_.state->load();
        current_term_ = saved.term;
        voted_for_ = saved.voted_for;
        // Committed entries are never truncated, so the log can't be shorter
        // than a commit index saved from it; clamp anyway rather than trust it.
        commit_index_ = std::min(saved.commit_index, log_.last().last_index);
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
            voted_for_ != 0 ? "node " + std::to_string(voted_for_) : std::string("none"),
            ", commit index ", commit_index_, " (log has ", log_.last().last_index, ")");
    }
    // Rebuild the state machine from committed entries only; anything after
    // commit_index stays in the log, unapplied, until it is committed (or
    // truncated by a leader).
    apply_committed();
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
    // A no-op in our term: once it commits, so has everything before it, and
    // we have applied all of it (reads wait for that; see read_barrier).
    term_start_ = append_own(command{kNoopOperation, {}, std::nullopt});
    log("appended no-op at index ", term_start_);
    notify();
    advance_leader_commit();  // a single-node cluster commits it at once
    send_heartbeats();
}

std::uint64_t RaftNode::append_own(command cmd) {
    const log_entry entry{log_.last().last_index + 1, current_term_, std::move(cmd)};
    try {
        log_.append(std::span<const log_entry>(&entry, 1));
    } catch (const wal_error& e) {
        // Our own copy counts toward the majority, so it must be on disk.
        std::cerr << "[raft node " << self_ << "] FATAL: cannot append to my log: " << e.what() << "\n";
        std::abort();
    }
    return entry.index;
}

bool RaftNode::propose(command cmd, outcome_fn done) {
    if (role_ != raft_role::leader) { return false; }
    const std::string op = cmd.operation;
    const std::uint64_t index = append_own(std::move(cmd));
    log("client ", op, ": appended at index ", index, " in term ", current_term_, "; replicating");
    pending_.emplace(index, waiter{current_term_, false, std::move(done)});
    advance_leader_commit();  // single-node cluster: commits (and may complete `done`) here
    send_heartbeats();        // ship it now rather than at the next heartbeat
    return true;
}

bool RaftNode::read_barrier(outcome_fn done) {
    if (role_ != raft_role::leader) { return false; }
    if (last_applied_ >= term_start_) {
        done({raft_outcome::kind::applied, {}});
    } else {
        pending_.emplace(term_start_, waiter{current_term_, true, std::move(done)});
    }
    return true;
}

// ---- commit and apply ----

void RaftNode::advance_leader_commit() {
    if (role_ != raft_role::leader) { return; }
    // Our own last index counts as our match. The majority()-th largest match
    // is the highest index a majority holds.
    std::vector<std::uint64_t> matches{log_.last().last_index};
    for (const auto& [peer, p] : progress_) { matches.push_back(p.match_index); }
    std::sort(matches.begin(), matches.end(), std::greater<>());
    const std::uint64_t n = matches[majority() - 1];
    if (n <= commit_index_) { return; }
    // Figure 8: only an entry from our own term may be committed by counting
    // replicas. Terms never decrease along a log, so if n's term is older,
    // every index below it is older too and there is nothing to commit yet;
    // those entries commit indirectly once one of ours above them does.
    const std::optional<std::uint64_t> term = log_.term_at(n);
    if (!term || *term != current_term_) {
        log("index ", n, " is on a majority but from term ", term.value_or(0),
            "; not committing it until an entry from term ", current_term_, " is");
        return;
    }
    set_commit(n);
}

void RaftNode::set_commit(std::uint64_t index) {
    if (index <= commit_index_) { return; }
    if (options_.state) {
        try {
            options_.state->save({current_term_, voted_for_, index});
        } catch (const raft_state_error& e) {
            std::cerr << "[raft node " << self_ << "] FATAL: cannot persist commit index " << index
                      << ": " << e.what() << "\n";
            std::abort();
        }
    }
    log("commitIndex ", commit_index_, " -> ", index);
    commit_index_ = index;
    apply_committed();
}

void RaftNode::apply_committed() {
    while (last_applied_ < commit_index_) {
        std::vector<log_entry> batch;
        try {
            batch = log_.entries(last_applied_ + 1, std::min<std::uint64_t>(commit_index_ - last_applied_, 64),
                                 SIZE_MAX);
        } catch (const wal_error& e) {
            std::cerr << "[raft node " << self_ << "] FATAL: cannot read committed entry "
                      << last_applied_ + 1 << ": " << e.what() << "\n";
            std::abort();
        }
        if (batch.empty() || batch.front().index != last_applied_ + 1) {
            std::cerr << "[raft node " << self_ << "] FATAL: committed entry " << last_applied_ + 1
                      << " missing from my log\n";
            std::abort();
        }
        for (const log_entry& e : batch) {
            std::string result;
            if (e.cmd.operation == kNoopOperation) {
                log("applied ", e.index, " (no-op, term ", e.term, ")");
            } else {
                if (options_.apply) { result = options_.apply(e); }
                log("applied ", e.index, " (", e.cmd.operation, ", term ", e.term, ") -> ", result);
            }
            last_applied_ = e.index;

            // Complete whoever waits on this index. A write whose slot now
            // holds another term's entry was overwritten: it never committed.
            auto [first, last] = pending_.equal_range(e.index);
            std::vector<waiter> done;
            for (auto it = first; it != last; ++it) { done.push_back(std::move(it->second)); }
            pending_.erase(first, last);
            for (auto& w : done) {
                if (w.read || w.term == e.term) {
                    w.done({raft_outcome::kind::applied, w.read ? std::string() : result});
                } else {
                    w.done({raft_outcome::kind::lost_leadership, {}});
                }
            }
        }
    }
}

void RaftNode::fail_pending(const char* why) {
    if (pending_.empty()) { return; }
    log("failing ", pending_.size(), " pending client request(s): ", why);
    auto failed = std::move(pending_);
    pending_.clear();
    for (auto& [index, w] : failed) { w.done({raft_outcome::kind::lost_leadership, {}}); }
}

// ---- heartbeats ----

void RaftNode::send_heartbeats() {
    if (role_ != raft_role::leader) { return; }
    for (const node_id peer : peers_) {
        // Per-follower probe at nextIndex - 1, plus our entries from nextIndex
        // on (none if the follower is caught up), plus our commit index.
        peer_progress& p = progress_[peer];
        std::optional<std::uint64_t> prev_term = log_.term_at(p.next_index - 1);
        if (!prev_term) {
            // Our log has no entry there (it can't shrink under a leader, so
            // this is defensive): probe from our own end instead.
            p.next_index = log_.last().last_index + 1;
            prev_term = log_.term_at(p.next_index - 1);
        }
        append_entries_request req{current_term_, self_, p.next_index - 1, prev_term.value_or(0), {},
                                   commit_index_};
        try {
            req.entries = log_.entries(p.next_index, kMaxEntriesPerAppend, kMaxAppendBytes);
        } catch (const wal_error& e) {
            // Can't read our own log: still assert leadership, just without entries.
            log("cannot read entries from ", p.next_index, " for node ", peer, ": ", e.what());
        }
        client_.send_append_entries(peer, req,
            [this, alive = std::weak_ptr<char>(alive_), peer, term = current_term_, prev = req.prev_log_index,
             count = req.entries.size()](rpc_result<append_entries_response> reply) {
                if (alive.expired()) { return; }
                on_heartbeat_reply(peer, term, prev, count, reply);
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
                                  std::size_t sent_entries, const rpc_result<append_entries_response>& reply) {
    if (!reply.ok()) { return; }  // unreachable peer: the next heartbeat retries
    if (reply.response.term > current_term_) {
        step_down(reply.response.term, "AppendEntries reply", peer);
        return;
    }
    // Stale: sent in an earlier term, or we're no longer leader.
    if (role_ != raft_role::leader || current_term_ != sent_term) { return; }
    peer_progress& p = progress_[peer];
    if (reply.response.success) {
        // The follower now matches us through the last entry that request
        // carried. matchIndex never goes back, so a late or duplicate success
        // for an earlier batch can't undo a later one.
        const std::uint64_t matched = prev_log_index + sent_entries;
        if (matched > p.match_index) {
            log("matchIndex[", peer, "] ", p.match_index, " -> ", matched);
            p.match_index = matched;
            advance_leader_commit();
        }
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
    term_start_ = 0;
    fail_pending("lost leadership");
    notify();
    // Followers and candidates already have an election timer running; a
    // leader had none, so it needs one now or it would never time out.
    if (was_leader) { reset_election_timer(); }
}

void RaftNode::persist(std::uint64_t term, node_id voted_for) {
    if (term == current_term_ && voted_for == voted_for_) { return; }  // e.g. a repeat vote for the same candidate
    if (options_.state) {
        try {
            options_.state->save({term, voted_for, commit_index_});
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

    // prev_log_index 0 is the empty prefix every log shares: always a match
    // (whatever prev_log_term says), and the entries are still stored below.
    const std::optional<std::uint64_t> mine =
        req.prev_log_index == 0 ? std::optional<std::uint64_t>(req.prev_log_term) : log_.term_at(req.prev_log_index);
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
    for (std::size_t k = 0; k < req.entries.size(); ++k) {
        if (req.entries[k].index != req.prev_log_index + 1 + k) {
            log("rejected AppendEntries from node ", req.leader_id, ": entry ", k, " has index ",
                req.entries[k].index, ", expected ", req.prev_log_index + 1 + k);
            return {current_term_, false};
        }
    }
    store_entries(req);  // on disk before success is returned
    // The leader's commit index, capped at the last entry this request
    // covered: entries past that may be a stale tail the leader hasn't
    // checked yet, so they can't be assumed to match its committed ones.
    if (req.leader_commit > commit_index_) {
        set_commit(std::min<std::uint64_t>(req.leader_commit, req.prev_log_index + req.entries.size()));
    }
    return {current_term_, true};
}

void RaftNode::store_entries(const append_entries_request& req) {
    const std::span<const log_entry> incoming(req.entries);
    try {
        // Skip what we already have with the same term (a retransmission must
        // never delete good entries); at the first term conflict, delete ours
        // from there on; append everything after that point.
        std::size_t k = 0;
        for (; k < incoming.size(); ++k) {
            const std::optional<std::uint64_t> mine = log_.term_at(incoming[k].index);
            if (!mine) { break; }
            if (*mine != incoming[k].term) {
                log("conflict at index ", incoming[k].index, " (my term ", *mine, ", leader's ",
                    incoming[k].term, "): truncating my log from ", incoming[k].index,
                    " (was ", log_.last().last_index, " entries)");
                log_.truncate_from(incoming[k].index);
                break;
            }
        }
        if (k == incoming.size()) { return; }
        log_.append(incoming.subspan(k));
        log("appended entries ", incoming[k].index, "..", incoming.back().index, " from node ",
            req.leader_id, " (last index now ", log_.last().last_index, ")");
    } catch (const wal_error& e) {
        // Replying success here would acknowledge entries that may not be on
        // disk; replying failure would walk the leader back for no reason.
        std::cerr << "[raft node " << self_ << "] FATAL: cannot store entries from node "
                  << req.leader_id << ": " << e.what() << "\n";
        std::abort();
    }
}
