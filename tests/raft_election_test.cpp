// Raft leader election + heartbeat tests.
//
// Part 1 drives one RaftNode against a fake PeerClient on this thread, so
// each Figure 2 rule is checked deterministically (the test delivers every
// RPC reply by hand). Part 2 runs real clusters over loopback sockets: each
// node has its own io_context, thread, client and PeerServer (the node
// itself stays single-threaded), and an on_change observer records every
// transition so Election Safety is checked over the complete history, not
// by sampling.
//
// Isolation: listeners bind 127.0.0.1 port 0 (OS-assigned); no files are
// written; every node thread is stopped and joined by RAII even on failure;
// each test re-arms alarm() so a hang fails rather than stalling CTest.
// Set RAFT_TEST_VERBOSE=1 to see the nodes' own stderr logs.
// Standalone (no framework): exits non-zero if any check fails.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include "peer_transport.h"
#include "../include/raft_node.h"

using asio::ip::tcp;
using namespace std::chrono_literals;
using steady = std::chrono::steady_clock;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static const bool kVerbose = std::getenv("RAFT_TEST_VERBOSE") != nullptr;

static long long ms_since(steady::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count();
}

// =====================================================================
// Part 1: one node, fake transport, every reply delivered by the test
// =====================================================================

class FakeClient final : public PeerClient
{
public:
    struct vote_call { node_id peer; request_vote_request req; request_vote_callback cb; };
    struct append_call { node_id peer; append_entries_request req; append_entries_callback cb; };

    void send_request_vote(node_id peer, const request_vote_request& req,
                           request_vote_callback cb) override {
        votes.push_back({peer, req, std::move(cb)});
    }
    void send_append_entries(node_id peer, const append_entries_request& req,
                             append_entries_callback cb) override {
        appends.push_back({peer, req, std::move(cb)});
    }

    std::vector<vote_call> votes;
    std::vector<append_call> appends;
};

static rpc_result<request_vote_response> vote_reply(std::uint64_t term, bool granted) {
    return {rpc_error::none, {term, granted}};
}

static rpc_result<append_entries_response> append_reply(std::uint64_t term, bool success) {
    return {rpc_error::none, {term, success}};
}

// Runs handlers until `pred` holds (checked after every handler) or the deadline passes.
static bool run_until(asio::io_context& io, const std::function<bool()>& pred,
                      std::chrono::milliseconds deadline) {
    const auto start = steady::now();
    io.restart();
    while (!pred()) {
        if (ms_since(start) >= deadline.count()) { return false; }
        io.run_one_for(5ms);
    }
    return true;
}

static void run_for(asio::io_context& io, std::chrono::milliseconds d) {
    io.restart();
    io.run_for(d);
}

static raft_options quiet(std::chrono::milliseconds min, std::chrono::milliseconds max,
                          std::chrono::milliseconds heartbeat = 20ms) {
    raft_options o;
    o.election_timeout_min = min;
    o.election_timeout_max = max;
    o.heartbeat_interval = heartbeat;
    o.seed = 42;
    o.log = kVerbose;
    return o;
}

struct Harness
{
    asio::io_context io;
    FakeClient client;
    log_position mine;
    RaftNode node;

    Harness(node_id self, std::vector<node_id> peers, raft_options opts)
        : node(io, self, std::move(peers), client, [this] { return mine; }, std::move(opts)) {}

    // Waits for the election timer to fire into a candidacy for `term`.
    bool await_candidate(std::uint64_t term, std::chrono::milliseconds deadline = 2000ms) {
        return run_until(io, [&] {
            return node.role() == raft_role::candidate && node.current_term() == term;
        }, deadline);
    }
};

static void test_majority_math() {
    std::puts("test_majority_math");
    asio::io_context io;
    FakeClient c;
    auto pos = [] { return log_position{}; };
    const std::vector<std::vector<node_id>> clusters = {{}, {2}, {2, 3}, {2, 3, 4}, {2, 3, 4, 5}, {2, 3, 4, 5, 6, 7}};
    const std::vector<std::size_t> expect = {1, 2, 2, 3, 3, 4};  // N = 1,2,3,4,5,7
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        RaftNode n(io, 1, clusters[i], c, pos, quiet(10s, 10s));
        CHECK(n.majority() == expect[i]);
    }
}

static void test_vote_rules() {
    std::puts("test_vote_rules");
    Harness h(1, {2, 3}, quiet(10s, 10s));
    h.node.start();

    // First vote in term 1 is granted and remembered.
    auto r = h.node.handle_request_vote({1, 2, 0, 0});
    CHECK(r.vote_granted && r.term == 1);
    CHECK(h.node.status().voted_for == 2 && h.node.current_term() == 1);
    // One vote per term: a different candidate is refused...
    r = h.node.handle_request_vote({1, 3, 0, 0});
    CHECK(!r.vote_granted && r.term == 1);
    // ...but a retry from the same candidate is granted again.
    r = h.node.handle_request_vote({1, 2, 0, 0});
    CHECK(r.vote_granted);
    // Stale term: refused, reply carries our term, nothing changes.
    r = h.node.handle_request_vote({0, 3, 99, 99});
    CHECK(!r.vote_granted && r.term == 1 && h.node.status().voted_for == 2);

    // Up-to-date check against the local log (5 @ term 2).
    h.mine = {5, 2};
    // Higher term adopts the term and clears voted_for even when the vote is refused.
    r = h.node.handle_request_vote({2, 3, 9, 1});  // lower last term, longer log: behind
    CHECK(!r.vote_granted && r.term == 2);
    CHECK(h.node.current_term() == 2 && h.node.status().voted_for == 0);
    CHECK(h.node.role() == raft_role::follower);
    r = h.node.handle_request_vote({2, 3, 4, 2});  // same last term, shorter: behind
    CHECK(!r.vote_granted);
    r = h.node.handle_request_vote({2, 3, 5, 2});  // same last term, same length: ok
    CHECK(r.vote_granted && h.node.status().voted_for == 3);
    r = h.node.handle_request_vote({3, 2, 1, 3});  // higher last term wins despite shorter log
    CHECK(r.vote_granted && r.term == 3 && h.node.status().voted_for == 2);
    r = h.node.handle_request_vote({4, 3, 6, 2});  // same last term, longer: ok
    CHECK(r.vote_granted && r.term == 4);
}

// Only a granted vote or current-term AppendEntries resets the election timer.
static void test_timer_resets() {
    std::puts("test_timer_resets");
    {
        // Baseline: with no contact, the election starts ~timeout after start.
        Harness h(1, {2, 3}, quiet(120ms, 120ms));
        const auto start = steady::now();
        h.node.start();
        CHECK(h.await_candidate(1));
        const auto elapsed = ms_since(start);
        CHECK(elapsed >= 110 && elapsed < 600);
    }
    {
        // A refused (stale) RequestVote and a stale AppendEntries do NOT reset it.
        Harness h(1, {2, 3}, quiet(120ms, 120ms));
        h.node.handle_request_vote({5, 2, 0, 0});  // term 5, granted -> resets
        const auto start = steady::now();
        h.node.start();
        run_for(h.io, 60ms);
        CHECK(!h.node.handle_request_vote({4, 3, 0, 0}).vote_granted);   // stale term
        CHECK(!h.node.handle_request_vote({5, 3, 0, 0}).vote_granted);   // already voted
        CHECK(!h.node.handle_append_entries({4, 3, 0, 0, {}, 0}).success); // stale leader
        CHECK(h.await_candidate(6));
        const auto elapsed = ms_since(start);
        CHECK(elapsed >= 110 && elapsed < 160 + 400);  // fired on the original schedule
    }
    {
        // A granted vote pushes the election back by a full timeout.
        Harness h(1, {2, 3}, quiet(120ms, 120ms));
        h.node.start();
        run_for(h.io, 80ms);
        CHECK(h.node.role() == raft_role::follower);
        const auto grant = steady::now();
        CHECK(h.node.handle_request_vote({1, 2, 0, 0}).vote_granted);
        CHECK(h.await_candidate(2));
        CHECK(ms_since(grant) >= 110);
    }
    {
        // Heartbeats from a current leader keep a follower from ever timing out.
        Harness h(1, {2, 3}, quiet(120ms, 120ms));
        h.node.start();
        const auto start = steady::now();
        while (ms_since(start) < 700) {
            CHECK(h.node.handle_append_entries({1, 2, 0, 0, {}, 0}).success);
            run_for(h.io, 40ms);
        }
        CHECK(h.node.role() == raft_role::follower && h.node.current_term() == 1);
        CHECK(h.node.status().leader_id == 2);
        CHECK(h.client.votes.empty());  // never started an election
        // Once heartbeats stop, it does time out.
        CHECK(h.await_candidate(2));
    }
}

static void test_election_win_n5() {
    std::puts("test_election_win_n5");
    Harness h(1, {2, 3, 4, 5}, quiet(50ms, 50ms, 20ms));
    h.mine = {7, 3};
    h.node.start();
    CHECK(h.await_candidate(1));
    CHECK(h.node.status().voted_for == 1);

    // RequestVote went to every peer, carrying the local log position.
    CHECK(h.client.votes.size() == 4);
    std::set<node_id> asked;
    for (const auto& v : h.client.votes) {
        asked.insert(v.peer);
        CHECK(v.req.term == 1 && v.req.candidate_id == 1);
        CHECK(v.req.last_log_index == 7 && v.req.last_log_term == 3);
    }
    CHECK((asked == std::set<node_id>{2, 3, 4, 5}));

    auto& calls = h.client.votes;
    auto reply_from = [&](node_id peer, rpc_result<request_vote_response> r) {
        for (auto& v : calls) { if (v.peer == peer) { v.cb(r); return; } }
    };
    reply_from(2, vote_reply(1, true));    // 2/3
    CHECK(h.node.role() == raft_role::candidate);
    reply_from(2, vote_reply(1, true));    // duplicate: still 2/3
    CHECK(h.node.role() == raft_role::candidate);
    reply_from(3, vote_reply(1, false));   // refusal
    reply_from(4, {rpc_error::timeout, {}});  // unreachable
    CHECK(h.node.role() == raft_role::candidate);
    reply_from(5, vote_reply(1, true));    // 3/3: majority of 5
    CHECK(h.node.role() == raft_role::leader);
    CHECK(h.node.status().leader_id == 1 && h.node.current_term() == 1);

    // Heartbeats go out immediately: empty AppendEntries to every peer.
    CHECK(h.client.appends.size() == 4);
    for (const auto& a : h.client.appends) {
        CHECK(a.req.term == 1 && a.req.leader_id == 1 && a.req.entries.empty());
    }
    // A late grant changes nothing.
    reply_from(3, vote_reply(1, true));
    CHECK(h.node.role() == raft_role::leader && h.node.current_term() == 1);

    // Heartbeats repeat; a leader runs no election timeout (term stays put).
    const std::size_t before = h.client.appends.size();
    run_for(h.io, 300ms);  // 6x the election timeout, ~15 heartbeat intervals
    CHECK(h.node.role() == raft_role::leader && h.node.current_term() == 1);
    const std::size_t rounds = (h.client.appends.size() - before) / 4;
    CHECK(rounds >= 8 && rounds <= 20);
    CHECK(h.client.votes.size() == 4);  // no further elections

    // success=false at our own term is not a step-down signal (no log matching yet).
    h.client.appends.back().cb(append_reply(1, false));
    CHECK(h.node.role() == raft_role::leader);
}

// Votes are counted per term: grants from an abandoned election don't count.
static void test_split_vote_and_stale_replies() {
    std::puts("test_split_vote_and_stale_replies");
    Harness h(1, {2, 3}, quiet(60ms, 60ms));
    h.node.start();
    CHECK(h.await_candidate(1));
    // Nobody answers: split vote -> the timer fires again into term 2.
    CHECK(h.await_candidate(2));
    CHECK(h.client.votes.size() == 4);
    // Late term-1 grants arrive: stale, must not make us leader of term 2.
    h.client.votes[0].cb(vote_reply(1, true));
    h.client.votes[1].cb(vote_reply(1, true));
    CHECK(h.node.role() == raft_role::candidate && h.node.current_term() == 2);
    // One term-2 grant: 2/2 of N=3 -> leader.
    h.client.votes[2].cb(vote_reply(2, true));
    CHECK(h.node.role() == raft_role::leader && h.node.current_term() == 2);
}

static void test_single_node_cluster() {
    std::puts("test_single_node_cluster");
    Harness h(1, {}, quiet(30ms, 30ms));
    h.node.start();
    CHECK(run_until(h.io, [&] { return h.node.role() == raft_role::leader; }, 1000ms));
    CHECK(h.node.current_term() == 1);
}

static void test_term_authority() {
    std::puts("test_term_authority");
    auto make_leader = [](Harness& h) {
        h.node.start();
        CHECK(h.await_candidate(1));
        h.client.votes[0].cb(vote_reply(1, true));
        CHECK(h.node.role() == raft_role::leader);
    };
    {
        // Leader gets AppendEntries with a higher term: follower of the new leader.
        Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
        make_leader(h);
        const auto r = h.node.handle_append_entries({5, 3, 0, 0, {}, 0});
        CHECK(r.success && r.term == 5);
        const auto s = h.node.status();
        CHECK(s.role == raft_role::follower && s.term == 5 && s.voted_for == 0 && s.leader_id == 3);
        // Heartbeats stop.
        const std::size_t sent = h.client.appends.size();
        run_for(h.io, 30ms);
        CHECK(h.client.appends.size() == sent);
        // The ex-leader got an election timer back, so it times out again.
        CHECK(h.await_candidate(6));
    }
    {
        // Leader gets RequestVote with a higher term: steps down (and may vote).
        Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
        make_leader(h);
        const auto r = h.node.handle_request_vote({3, 2, 0, 0});
        CHECK(r.vote_granted && r.term == 3);
        CHECK(h.node.role() == raft_role::follower && h.node.status().voted_for == 2);
    }
    {
        // Leader sees a higher term in a heartbeat reply.
        Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
        make_leader(h);
        h.client.appends.front().cb(append_reply(9, false));
        CHECK(h.node.role() == raft_role::follower && h.node.current_term() == 9);
        CHECK(h.node.status().voted_for == 0);
        // Nothing else re-armed its election timer: the step-down itself must.
        CHECK(h.await_candidate(10));
    }
    {
        // Leader refuses a higher-term candidate whose log is behind: still
        // steps down, and (no vote granted, so no reset) must time out again.
        Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
        make_leader(h);
        h.mine = {4, 1};
        const auto r = h.node.handle_request_vote({3, 2, 0, 0});
        CHECK(!r.vote_granted && r.term == 3);
        CHECK(h.node.role() == raft_role::follower && h.node.status().voted_for == 0);
        CHECK(h.await_candidate(4));
    }
    {
        // Candidate sees a higher term in a vote reply.
        Harness h(1, {2, 3}, quiet(50ms, 50ms));
        h.node.start();
        CHECK(h.await_candidate(1));
        h.client.votes[0].cb(vote_reply(4, false));
        CHECK(h.node.role() == raft_role::follower && h.node.current_term() == 4);
        // A grant for the abandoned term-1 election can't revive it.
        h.client.votes[1].cb(vote_reply(1, true));
        CHECK(h.node.role() == raft_role::follower);
    }
    {
        // Candidate gets AppendEntries for its own term: someone else won.
        Harness h(1, {2, 3}, quiet(50ms, 50ms));
        h.node.start();
        CHECK(h.await_candidate(1));
        const auto r = h.node.handle_append_entries({1, 2, 0, 0, {}, 0});
        CHECK(r.success && r.term == 1);
        CHECK(h.node.role() == raft_role::follower && h.node.status().leader_id == 2);
        // Our self-vote stands for term 1, so another candidate is refused.
        CHECK(!h.node.handle_request_vote({1, 3, 0, 0}).vote_granted);
    }
    {
        // Stale AppendEntries: rejected with our term, no state change.
        Harness h(1, {2, 3}, quiet(10s, 10s));
        h.node.start();
        h.node.handle_request_vote({7, 2, 0, 0});
        const auto r = h.node.handle_append_entries({6, 3, 0, 0, {}, 0});
        CHECK(!r.success && r.term == 7);
        CHECK(h.node.status().leader_id == 0 && h.node.current_term() == 7);
    }
}

// Callbacks that arrive after the node is gone must not touch it.
static void test_late_callbacks_after_destruction() {
    std::puts("test_late_callbacks_after_destruction");
    asio::io_context io;
    FakeClient client;
    PeerHandlers handlers;
    {
        RaftNode node(io, 1, {2, 3}, client, [] { return log_position{}; }, quiet(20ms, 20ms));
        handlers = node.handlers();
        node.start();
        run_until(io, [&] { return !client.votes.empty(); }, 1000ms);
    }
    CHECK(!client.votes.empty());
    for (auto& v : client.votes) { v.cb(vote_reply(1, true)); }  // ASan would flag a use-after-free
    CHECK(!handlers.on_request_vote({9, 2, 0, 0}).vote_granted);
    run_for(io, 50ms);  // any timer handlers left over are no-ops
}

// =====================================================================
// Part 2: real clusters over loopback sockets
// =====================================================================

struct event { long long at_ms; raft_status s; };

// Every status change from every node, in arrival order.
class History
{
public:
    History() : start_(steady::now()) {}

    void record(const raft_status& s) {
        std::lock_guard lock(mu_);
        events_.push_back({ms_since(start_), s});
        latest_[s.id] = s;
    }
    std::map<node_id, raft_status> latest() {
        std::lock_guard lock(mu_);
        return latest_;
    }
    std::vector<event> events() {
        std::lock_guard lock(mu_);
        return events_;
    }
    long long now_ms() const { return ms_since(start_); }

    // Election Safety over the complete history: at most one leader per term.
    bool at_most_one_leader_per_term() {
        std::map<std::uint64_t, std::set<node_id>> leaders;
        for (const auto& e : events()) {
            if (e.s.role == raft_role::leader) { leaders[e.s.term].insert(e.s.id); }
        }
        bool ok = true;
        for (const auto& [term, ids] : leaders) {
            if (ids.size() > 1) {
                std::fprintf(stderr, "  ELECTION SAFETY VIOLATED: %zu leaders in term %llu\n",
                             ids.size(), static_cast<unsigned long long>(term));
                ok = false;
            }
        }
        return ok;
    }

    std::size_t elections_started_after(long long t_ms) {
        std::size_t n = 0;
        for (const auto& e : events()) {
            if (e.at_ms > t_ms && e.s.role == raft_role::candidate) { ++n; }
        }
        return n;
    }

private:
    std::mutex mu_;
    steady::time_point start_;
    std::vector<event> events_;
    std::map<node_id, raft_status> latest_;
};

// One cluster member: io_context + thread + PeerServer + client + RaftNode.
class LiveNode
{
public:
    LiveNode(node_id id, History& history) : id_(id), history_(history) {
        // Listen first (port 0) so every node's address is known before any
        // client is built; handlers forward to raft_, set before io runs.
        PeerHandlers fwd;
        fwd.on_request_vote = [this](const request_vote_request& r) { return raft_->handle_request_vote(r); };
        fwd.on_append_entries = [this](const append_entries_request& r) { return raft_->handle_append_entries(r); };
        server_ = std::make_unique<PeerServer>(io_, tcp::endpoint(asio::ip::address_v4::loopback(), 0), fwd);
    }

    ~LiveNode() { kill(); }

    std::uint16_t port() const { return port_; }
    node_id id() const { return id_; }
    bool alive() const { return thread_.joinable(); }

    void start(const peer_endpoints& cluster, raft_options opts) {
        port_ = server_->local_endpoint().port();
        peer_endpoints others = cluster;
        std::vector<node_id> peers;
        for (const auto& [id, ep] : cluster) { if (id != id_) { peers.push_back(id); } }
        client_ = std::make_unique<ConnectPerRpcClient>(io_, others, opts.election_timeout_min);
        opts.on_change = [this](const raft_status& s) { history_.record(s); };
        opts.seed = 1000 + id_ * 7919 + static_cast<std::uint64_t>(steady::now().time_since_epoch().count());
        opts.log = kVerbose;
        raft_ = std::make_unique<RaftNode>(io_, id_, peers, *client_, [] { return log_position{}; }, opts);
        raft_->start();
        thread_ = std::thread([this] { io_.run(); });
    }

    // Crash-stop: halt the event loop, then close the listener so peers get
    // connection refused, as they would from a dead process.
    void kill() {
        if (!thread_.joinable()) { return; }
        io_.stop();
        thread_.join();
        server_.reset();
        raft_.reset();
        client_.reset();
    }

    std::uint16_t listen_port() const { return server_->local_endpoint().port(); }

private:
    node_id id_;
    History& history_;
    asio::io_context io_;
    std::unique_ptr<PeerServer> server_;
    std::unique_ptr<ConnectPerRpcClient> client_;
    std::unique_ptr<RaftNode> raft_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

class Cluster
{
public:
    explicit Cluster(std::size_t n, raft_options opts = {}) {
        for (node_id id = 1; id <= n; ++id) { nodes_.push_back(std::make_unique<LiveNode>(id, history)); }
        peer_endpoints eps;
        for (const auto& node : nodes_) {
            eps[node->id()] = tcp::endpoint(asio::ip::address_v4::loopback(), node->listen_port());
        }
        for (const auto& node : nodes_) { node->start(eps, opts); }
    }

    ~Cluster() { for (auto& n : nodes_) { n->kill(); } }

    LiveNode& node(node_id id) { return *nodes_.at(id - 1); }

    struct agreement { node_id leader = 0; std::uint64_t term = 0; };

    // All live nodes agree: one leader, everyone else a follower of it, same term.
    std::optional<agreement> agreed() {
        const auto latest = history.latest();
        std::optional<agreement> out;
        std::uint64_t term = 0;
        node_id leader = 0;
        for (const auto& n : nodes_) {
            if (!n->alive()) { continue; }
            const auto it = latest.find(n->id());
            if (it == latest.end()) { return std::nullopt; }
            const raft_status& s = it->second;
            if (term == 0) { term = s.term; }
            if (s.term != term || term == 0) { return std::nullopt; }
            if (s.role == raft_role::leader) {
                if (leader != 0) { return std::nullopt; }
                leader = s.id;
            } else if (s.role != raft_role::follower) {
                return std::nullopt;
            }
        }
        if (leader == 0) { return std::nullopt; }
        for (const auto& n : nodes_) {
            if (n->alive() && n->id() != leader && latest.at(n->id()).leader_id != leader) { return std::nullopt; }
        }
        return agreement{leader, term};
    }

    std::optional<agreement> await_agreement(std::chrono::milliseconds deadline) {
        const auto start = steady::now();
        while (ms_since(start) < deadline.count()) {
            if (auto a = agreed()) { return a; }
            std::this_thread::sleep_for(5ms);
        }
        return std::nullopt;
    }

    std::size_t live_count() const {
        std::size_t n = 0;
        for (const auto& node : nodes_) { n += node->alive() ? 1 : 0; }
        return n;
    }

    History history;

private:
    std::vector<std::unique_ptr<LiveNode>> nodes_;
};

// Elects, then holds steady for `hold` with no churn. Returns the agreement.
static std::optional<Cluster::agreement> elect_and_hold(Cluster& c, const char* label,
                                                        std::chrono::milliseconds hold) {
    const auto start = steady::now();
    const auto a = c.await_agreement(3000ms);
    CHECK(a.has_value());
    if (!a) { return a; }
    std::printf("  %s: node %llu elected leader of term %llu after %lldms (%zu nodes)\n", label,
                static_cast<unsigned long long>(a->leader), static_cast<unsigned long long>(a->term),
                ms_since(start), c.live_count());

    // Stability: across the hold window nobody starts an election, and the
    // same leader/term still holds at the end.
    const long long t0 = c.history.now_ms();
    std::this_thread::sleep_for(hold);
    CHECK(c.history.elections_started_after(t0) == 0);
    const auto later = c.agreed();
    CHECK(later.has_value() && later->leader == a->leader && later->term == a->term);
    return a;
}

static void test_cluster3_happy_path() {
    std::puts("test_cluster3_happy_path");
    ::alarm(30);
    Cluster c(3);
    // ~20 heartbeat intervals, ~3x the max election timeout.
    elect_and_hold(c, "3-node", 1000ms);
    CHECK(c.history.at_most_one_leader_per_term());
}

// Followers stay put for a long stretch: heartbeats are resetting their timers.
static void test_heartbeats_suppress_elections() {
    std::puts("test_heartbeats_suppress_elections");
    ::alarm(30);
    Cluster c(3);
    const auto a = elect_and_hold(c, "3-node (long hold)", 3000ms);  // ~10-20 election timeouts
    if (a) {
        const auto latest = c.history.latest();
        for (const auto& [id, s] : latest) {
            CHECK(s.term == a->term);  // no follower ever bumped its term
        }
    }
    CHECK(c.history.at_most_one_leader_per_term());
}

static void test_cluster3_failover() {
    std::puts("test_cluster3_failover");
    ::alarm(30);
    Cluster c(3);
    const auto first = elect_and_hold(c, "3-node before failover", 300ms);
    if (!first) { return; }
    const auto killed_at = steady::now();
    c.node(first->leader).kill();
    std::printf("  killed leader node %llu\n", static_cast<unsigned long long>(first->leader));

    const auto second = c.await_agreement(3000ms);
    CHECK(second.has_value());
    if (second) {
        std::printf("  3-node after failover: node %llu elected leader of term %llu after %lldms\n",
                    static_cast<unsigned long long>(second->leader),
                    static_cast<unsigned long long>(second->term), ms_since(killed_at));
        CHECK(second->leader != first->leader);
        CHECK(second->term > first->term);
        // And it holds with only 2 of 3 alive.
        const long long t0 = c.history.now_ms();
        std::this_thread::sleep_for(500ms);
        CHECK(c.history.elections_started_after(t0) == 0);
        const auto later = c.agreed();
        CHECK(later && later->leader == second->leader && later->term == second->term);
    }
    CHECK(c.history.at_most_one_leader_per_term());
}

static void test_cluster5_with_two_failovers() {
    std::puts("test_cluster5_with_two_failovers");
    ::alarm(40);
    Cluster c(5);
    auto a = elect_and_hold(c, "5-node", 500ms);
    // 5 -> 4 -> 3 live nodes: a majority (3) remains each time.
    for (int round = 0; round < 2 && a; ++round) {
        c.node(a->leader).kill();
        const auto killed_at = steady::now();
        const auto b = c.await_agreement(3000ms);
        CHECK(b.has_value());
        if (!b) { break; }
        std::printf("  5-node after killing node %llu: node %llu leads term %llu after %lldms (%zu alive)\n",
                    static_cast<unsigned long long>(a->leader), static_cast<unsigned long long>(b->leader),
                    static_cast<unsigned long long>(b->term), ms_since(killed_at), c.live_count());
        CHECK(b->leader != a->leader && b->term > a->term);
        a = b;
    }
    CHECK(c.history.at_most_one_leader_per_term());
}

// Without a majority, no leader may emerge; candidates keep retrying (no deadlock).
static void test_minority_cannot_elect() {
    std::puts("test_minority_cannot_elect");
    ::alarm(30);
    Cluster c(3);
    const auto a = c.await_agreement(3000ms);
    CHECK(a.has_value());
    if (!a) { return; }
    // Kill everyone but one follower.
    node_id survivor = 0;
    for (node_id id = 1; id <= 3; ++id) {
        if (id != a->leader && survivor == 0) { survivor = id; } else { c.node(id).kill(); }
    }
    const long long t0 = c.history.now_ms();
    std::this_thread::sleep_for(1500ms);
    std::size_t leader_events = 0;
    for (const auto& e : c.history.events()) {
        if (e.at_ms > t0 && e.s.role == raft_role::leader) { ++leader_events; }
    }
    CHECK(leader_events == 0);
    CHECK(c.history.elections_started_after(t0) >= 3);  // still trying, term climbing
    const auto s = c.history.latest().at(survivor);
    CHECK(s.role == raft_role::candidate && s.term > a->term);
    std::printf("  lone node %llu: still candidate at term %llu (was %llu), no leader\n",
                static_cast<unsigned long long>(survivor), static_cast<unsigned long long>(s.term),
                static_cast<unsigned long long>(a->term));
    CHECK(c.history.at_most_one_leader_per_term());
}

// Several fresh 3-node clusters, to catch election races that only show up sometimes.
static void test_repeated_elections() {
    std::puts("test_repeated_elections");
    ::alarm(40);
    std::map<std::uint64_t, int> terms;
    std::map<node_id, int> winners;
    for (int i = 0; i < 8; ++i) {
        Cluster c(3);
        const auto a = c.await_agreement(3000ms);
        CHECK(a.has_value());
        if (a) { ++terms[a->term]; ++winners[a->leader]; }
        CHECK(c.history.at_most_one_leader_per_term());
    }
    std::printf("  8 clusters, winning term histogram:");
    for (const auto& [t, n] : terms) { std::printf(" t%llu x%d", static_cast<unsigned long long>(t), n); }
    std::printf("; winners:");
    for (const auto& [id, n] : winners) { std::printf(" node%llu x%d", static_cast<unsigned long long>(id), n); }
    std::printf("\n");
}

int main() {
    ::alarm(20);
    test_majority_math();
    test_vote_rules();
    test_timer_resets();
    test_election_win_n5();
    test_split_vote_and_stale_replies();
    test_single_node_cluster();
    test_term_authority();
    test_late_callbacks_after_destruction();

    test_cluster3_happy_path();
    test_heartbeats_suppress_elections();
    test_cluster3_failover();
    test_cluster5_with_two_failovers();
    test_minority_cannot_elect();
    test_repeated_elections();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all raft election tests passed");
    return 0;
}
