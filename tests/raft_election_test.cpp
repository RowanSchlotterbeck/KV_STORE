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
// Isolation: listeners bind 127.0.0.1 port 0 (OS-assigned); the persistent
// cluster tests write term/vote files only under their own mkdtemp directory,
// removed afterwards; every node thread is stopped and joined by RAII even on failure;
// each test re-arms alarm() so a hang fails rather than stalling CTest.
// Set RAFT_TEST_VERBOSE=1 to see the nodes' own stderr logs.
// Standalone (no framework): exits non-zero if any check fails.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
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
#include "../include/state_machine.h"
#include "../include/wal.h"

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

// A fresh directory for one test's files, removed when it goes out of scope.
struct TempDir
{
    std::filesystem::path path;
    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "kv_raft_election_test.XXXXXX").string();
        if (::mkdtemp(tmpl.data())) { path = tmpl; }
    }
    ~TempDir() {
        std::error_code ec;
        if (!path.empty()) { std::filesystem::remove_all(path, ec); }
    }
};

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

// A log as a list of terms: terms[i - 1] is the term of entry i.
struct FakeLog final : RaftLogView
{
    std::vector<std::uint64_t> terms;

    log_position last() const override {
        return terms.empty() ? log_position{} : log_position{terms.size(), terms.back()};
    }
    std::optional<std::uint64_t> term_at(std::uint64_t index) const override {
        if (index == 0) { return 0; }
        if (index > terms.size()) { return std::nullopt; }
        return terms[index - 1];
    }
    // `index` entries, all in `term`.
    void set_last(std::uint64_t index, std::uint64_t term) { terms.assign(index, term); }
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
    FakeLog log;
    RaftNode node;

    Harness(node_id self, std::vector<node_id> peers, raft_options opts)
        : node(io, self, std::move(peers), client, log, std::move(opts)) {}

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
    const FakeLog pos;
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
    h.log.set_last(5, 2);
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
    h.log.set_last(7, 3);
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
        h.log.set_last(4, 1);
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

// ---- AppendEntries consistency check (follower side) ----

static append_entries_request heartbeat(std::uint64_t term, node_id leader, std::uint64_t prev_index,
                                        std::uint64_t prev_term) {
    return {term, leader, prev_index, prev_term, {}, 0};
}

static void test_consistency_check() {
    std::puts("test_consistency_check");
    {
        // prev_log_index 0 always matches: empty log, and whatever prev_log_term says.
        Harness h(1, {2, 3}, quiet(10s, 10s));
        h.node.start();
        CHECK(h.node.handle_append_entries(heartbeat(1, 2, 0, 0)).success);
        CHECK(h.node.handle_append_entries(heartbeat(1, 2, 0, 7)).success);
        h.log.terms = {1, 1, 2};
        CHECK(h.node.handle_append_entries(heartbeat(1, 2, 0, 0)).success);
    }
    {
        // Follower log: index 1..4 in terms 1, 1, 2, 3.
        Harness h(1, {2, 3}, quiet(10s, 10s));
        h.log.terms = {1, 1, 2, 3};
        h.node.start();
        // Matching entries succeed, at the end or in the middle.
        CHECK(h.node.handle_append_entries(heartbeat(4, 2, 4, 3)).success);
        CHECK(h.node.handle_append_entries(heartbeat(4, 2, 3, 2)).success);
        CHECK(h.node.handle_append_entries(heartbeat(4, 2, 1, 1)).success);
        // Shorter than prev_log_index: no such slot.
        auto r = h.node.handle_append_entries(heartbeat(4, 2, 5, 3));
        CHECK(!r.success && r.term == 4);
        CHECK(!h.node.handle_append_entries(heartbeat(4, 2, 99, 3)).success);
        CHECK(!h.node.handle_append_entries(heartbeat(4, 2, UINT64_MAX, UINT64_MAX)).success);
        // Entry present, different term.
        r = h.node.handle_append_entries(heartbeat(4, 2, 3, 3));
        CHECK(!r.success && r.term == 4);
        CHECK(!h.node.handle_append_entries(heartbeat(4, 2, 4, 2)).success);
        CHECK(!h.node.handle_append_entries(heartbeat(4, 2, 1, 0)).success);
        // A mismatch is still a legitimate leader: follower of node 2, term unchanged.
        const auto s = h.node.status();
        CHECK(s.role == raft_role::follower && s.term == 4 && s.leader_id == 2 && s.voted_for == 0);
        // Term rules come first: a stale leader is refused even with a matching log.
        CHECK(!h.node.handle_append_entries(heartbeat(3, 3, 4, 3)).success);
    }
    {
        // A mismatching current-term AppendEntries still resets the election
        // timer (only success reports the log check).
        Harness h(1, {2, 3}, quiet(120ms, 120ms));
        h.node.start();
        const auto start = steady::now();
        while (ms_since(start) < 500) {
            CHECK(!h.node.handle_append_entries(heartbeat(1, 2, 3, 1)).success);
            run_for(h.io, 40ms);
        }
        CHECK(h.node.role() == raft_role::follower && h.client.votes.empty());
    }
    {
        // A candidate that gets a mismatching AppendEntries for its term still yields.
        Harness h(1, {2, 3}, quiet(50ms, 50ms));
        h.node.start();
        CHECK(h.await_candidate(1));
        CHECK(!h.node.handle_append_entries(heartbeat(1, 2, 9, 1)).success);
        CHECK(h.node.role() == raft_role::follower && h.node.status().leader_id == 2);
    }
}

// ---- nextIndex / matchIndex (leader side) ----

static void make_leader(Harness& h, std::uint64_t term = 1) {
    h.node.start();
    CHECK(h.await_candidate(term));
    for (auto& v : h.client.votes) {
        if (v.req.term == term) { v.cb(vote_reply(term, true)); break; }
    }
    CHECK(h.node.role() == raft_role::leader);
}

// Heartbeats sent so far to `peer`, and the number already seen.
static std::vector<FakeClient::append_call*> appends_to(FakeClient& c, node_id peer, std::size_t skip = 0) {
    std::vector<FakeClient::append_call*> out;
    for (std::size_t i = skip; i < c.appends.size(); ++i) {
        if (c.appends[i].peer == peer) { out.push_back(&c.appends[i]); }
    }
    return out;
}

static void test_leader_progress() {
    std::puts("test_leader_progress");
    Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
    h.log.terms = {1, 1, 1};
    CHECK(h.node.progress().empty());  // not leader
    make_leader(h);

    // On winning: nextIndex = last + 1, matchIndex = 0, for every peer.
    auto p = h.node.progress();
    CHECK(p.size() == 2);
    CHECK(p[2].next_index == 4 && p[2].match_index == 0);
    CHECK(p[3].next_index == 4 && p[3].match_index == 0);

    // The first heartbeats probe nextIndex - 1 = 3 with our term there; still empty.
    CHECK(h.client.appends.size() == 2);
    for (const auto& a : h.client.appends) {
        CHECK(a.req.prev_log_index == 3 && a.req.prev_log_term == 1);
        CHECK(a.req.entries.empty() && a.req.leader_commit == 0 && a.req.term == 1 && a.req.leader_id == 1);
    }

    // Success from peer 2 only: matchIndex[2] = 3, nextIndex[2] = 4; peer 3 untouched.
    appends_to(h.client, 2).at(0)->cb(append_reply(1, true));
    p = h.node.progress();
    CHECK(p[2].next_index == 4 && p[2].match_index == 3);
    CHECK(p[3].next_index == 4 && p[3].match_index == 0);

    // A failure that isn't for the current probe can't move anything:
    // here, a duplicate rejection of prev 3 after nextIndex already moved.
    appends_to(h.client, 3).at(0)->cb(append_reply(1, false));
    CHECK(h.node.progress()[3].next_index == 3);
    // Two more heartbeats to 3 go out, both probing prev 2; both get rejected.
    const std::size_t seen = h.client.appends.size();
    run_until(h.io, [&] { return appends_to(h.client, 3, seen).size() >= 2; }, 1000ms);
    auto probes = appends_to(h.client, 3, seen);
    CHECK(probes.size() >= 2);
    if (probes.size() >= 2) {
        CHECK(probes[0]->req.prev_log_index == 2 && probes[1]->req.prev_log_index == 2);
        probes[0]->cb(append_reply(1, false));
        probes[1]->cb(append_reply(1, false));  // same probe, in flight together
        CHECK(h.node.progress()[3].next_index == 2);  // stepped back once, not twice
    }
    // The original stale rejection of prev 3 again: ignored.
    appends_to(h.client, 3).at(0)->cb(append_reply(1, false));
    CHECK(h.node.progress()[3].next_index == 2);
    // An unreachable peer changes nothing.
    appends_to(h.client, 2).back()->cb({rpc_error::connect_failed, {}});
    CHECK(h.node.progress()[2].next_index == 4 && h.node.progress()[2].match_index == 3);
}

// nextIndex never goes below 1, and index 0 always matches, so the walk ends.
static void test_next_index_floor() {
    std::puts("test_next_index_floor");
    Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
    h.log.terms = {1};
    make_leader(h);
    appends_to(h.client, 2).at(0)->cb(append_reply(1, false));  // prev 1 rejected
    CHECK(h.node.progress()[2].next_index == 1);
    const std::size_t seen = h.client.appends.size();
    run_until(h.io, [&] { return !appends_to(h.client, 2, seen).empty(); }, 1000ms);
    auto probe = appends_to(h.client, 2, seen);
    CHECK(!probe.empty());
    if (!probe.empty()) {
        CHECK(probe[0]->req.prev_log_index == 0 && probe[0]->req.prev_log_term == 0);
        // A (misbehaving) rejection of index 0 can't push it below 1.
        probe[0]->cb(append_reply(1, false));
        CHECK(h.node.progress()[2].next_index == 1);
        CHECK(h.node.role() == raft_role::leader);
    }
}

// A higher term in an AppendEntries reply ends leadership: no further
// decrements, and late replies from that term can't touch a later term's state.
static void test_reply_with_higher_term() {
    std::puts("test_reply_with_higher_term");
    Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
    h.log.terms = {1, 1, 1};
    make_leader(h);
    // A copy: `appends` reallocates as more heartbeats go out.
    const PeerClient::append_entries_callback to3 = appends_to(h.client, 3).at(0)->cb;
    appends_to(h.client, 2).at(0)->cb(append_reply(4, false));
    CHECK(h.node.role() == raft_role::follower && h.node.current_term() == 4);
    CHECK(h.node.progress().empty());
    // Peer 3's term-1 rejection arrives after the step-down: ignored.
    to3(append_reply(1, false));
    CHECK(h.node.role() == raft_role::follower && h.node.current_term() == 4);

    // Win term 5; a straggler from term 1 still can't move the new nextIndex.
    CHECK(h.await_candidate(5));
    for (auto& v : h.client.votes) {
        if (v.req.term == 5) { v.cb(vote_reply(5, true)); break; }
    }
    CHECK(h.node.role() == raft_role::leader);
    CHECK(h.node.progress()[3].next_index == 4);
    to3(append_reply(1, false));
    CHECK(h.node.progress()[3].next_index == 4 && h.node.role() == raft_role::leader);
    // A rejection carrying the same term is just a mismatch, not a step-down.
    appends_to(h.client, 3).back()->cb(append_reply(5, false));
    CHECK(h.node.role() == raft_role::leader && h.node.progress()[3].next_index == 3);
}

// Writes a PersistentLog whose entry i has term terms[i - 1], and opens it.
static void build_log(PersistentLog& log, const std::filesystem::path& path,
                      const std::vector<std::uint64_t>& terms) {
    std::filesystem::create_directories(path.parent_path());
    std::string frames;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        frames += PersistentLog::encode_frame(
            log_entry{i + 1, terms[i], command{"PUT", "k" + std::to_string(i + 1), "v"}});
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(frames.data(), 1, frames.size(), f);
        std::fclose(f);
    }
    log.open(path.string());
    StateMachine sm;
    CHECK(log.replay(sm) == terms.size());
}

// The backward walk, on real PersistentLogs that diverge after index 4:
//   index:     1 2 3 4 | 5 6 7
//   leader:    1 1 1 2 | 2 3 3
//   follower:  1 1 1 2 | 4 4
// The leader starts at nextIndex 8 and must step back to 5 (prev 4, the last
// agreeing entry), succeed there, and then stay put.
static void test_backward_walk() {
    std::puts("test_backward_walk");
    ::alarm(20);
    TempDir dir;
    CHECK(!dir.path.empty());
    PersistentLog leader_log;
    PersistentLog follower_log;
    build_log(leader_log, dir.path / "leader" / "kv.log", {1, 1, 1, 2, 2, 3, 3});
    build_log(follower_log, dir.path / "follower" / "kv.log", {1, 1, 1, 2, 4, 4});
    const PersistentLogView leader_view(leader_log);
    const PersistentLogView follower_view(follower_log);

    // The leader resumes at term 4 (from its state file) so it wins term 5,
    // newer than any entry in either log.
    RaftStateFile leader_state((dir.path / "leader" / "kv.log.raftstate").string(), 1);
    leader_state.save({4, 0});
    raft_options lo = quiet(50ms, 50ms, 20ms);
    lo.state = &leader_state;
    asio::io_context leader_io;
    FakeClient leader_client;
    RaftNode leader(leader_io, 1, {2, 3}, leader_client, leader_view, lo);

    asio::io_context follower_io;
    FakeClient follower_client;
    RaftNode follower(follower_io, 2, {1, 3}, follower_client, follower_view, quiet(10s, 10s));
    follower.start();

    leader.start();
    run_until(leader_io, [&] { return !leader_client.votes.empty(); }, 2000ms);
    CHECK(leader.current_term() == 5);
    leader_client.votes.at(0).cb(vote_reply(5, true));
    CHECK(leader.role() == raft_role::leader);

    // Deliver each heartbeat to the real follower and its reply back, one round at a time.
    std::vector<std::string> trajectory;
    std::vector<peer_progress> after;
    std::size_t seen = 0;
    const auto fmt = [](const peer_progress& p) {
        return "next " + std::to_string(p.next_index) + "/match " + std::to_string(p.match_index);
    };
    trajectory.push_back("start: " + fmt(leader.progress()[2]));
    for (int round = 0; round < 8; ++round) {
        if (!run_until(leader_io, [&] { return !appends_to(leader_client, 2, seen).empty(); }, 1000ms)) { break; }
        auto* call = appends_to(leader_client, 2, seen).front();
        seen = static_cast<std::size_t>(call - leader_client.appends.data()) + 1;
        const auto req = call->req;
        CHECK(req.entries.empty() && req.leader_commit == 0);
        const auto resp = follower.handle_append_entries(req);
        call->cb({rpc_error::none, resp});
        after.push_back(leader.progress()[2]);
        trajectory.push_back("prev " + std::to_string(req.prev_log_index) + "@t" + std::to_string(req.prev_log_term)
                             + (resp.success ? " ok   -> " : " FAIL -> ") + fmt(after.back()));
    }
    std::printf("  nextIndex[2] trajectory:\n");
    for (const auto& line : trajectory) { std::printf("    %s\n", line.c_str()); }

    CHECK(after.size() == 8);
    if (after.size() == 8) {
        const std::vector<std::uint64_t> want_next = {7, 6, 5, 5, 5, 5, 5, 5};
        const std::vector<std::uint64_t> want_match = {0, 0, 0, 4, 4, 4, 4, 4};
        for (std::size_t i = 0; i < after.size(); ++i) {
            CHECK(after[i].next_index == want_next[i] && after[i].match_index == want_match[i]);
        }
    }
    // The follower took every probe as from a legitimate leader and kept its log.
    CHECK(follower.role() == raft_role::follower && follower.current_term() == 5);
    CHECK(follower.status().leader_id == 1);
    CHECK(follower_log.last_index() == 6);
    CHECK(leader.role() == raft_role::leader && leader.current_term() == 5);
}

// Callbacks that arrive after the node is gone must not touch it.
static void test_late_callbacks_after_destruction() {
    std::puts("test_late_callbacks_after_destruction");
    asio::io_context io;
    FakeClient client;
    PeerHandlers handlers;
    {
        const FakeLog log;
        RaftNode node(io, 1, {2, 3}, client, log, quiet(20ms, 20ms));
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

// One cluster member: io_context + thread + PeerServer + client + RaftNode,
// plus its term/vote file when `state_path` is given. `port` 0 picks a free
// one; a restarted node passes its old port so peers can still reach it.
// `log_terms` is its (fixed) log: log_terms[i - 1] is the term of entry i.
class LiveNode
{
public:
    LiveNode(node_id id, History& history, std::uint16_t port = 0, std::string state_path = {},
             std::vector<std::uint64_t> log_terms = {})
        : id_(id), history_(history), state_path_(std::move(state_path)) {
        log_.terms = std::move(log_terms);
        // Listen first (port 0) so every node's address is known before any
        // client is built; handlers forward to raft_, set before io runs.
        PeerHandlers fwd;
        fwd.on_request_vote = [this](const request_vote_request& r) { return raft_->handle_request_vote(r); };
        fwd.on_append_entries = [this](const append_entries_request& r) { return raft_->handle_append_entries(r); };
        server_ = std::make_unique<PeerServer>(io_, tcp::endpoint(asio::ip::address_v4::loopback(), port), fwd);
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
        if (!state_path_.empty()) {
            state_ = std::make_unique<RaftStateFile>(state_path_, id_);
            opts.state = state_.get();
        }
        raft_ = std::make_unique<RaftNode>(io_, id_, peers, *client_, log_, opts);
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
        state_.reset();
    }

    std::uint16_t listen_port() const { return server_->local_endpoint().port(); }

    // The node's leader-side progress, read on its own io thread.
    std::map<node_id, peer_progress> progress() {
        if (!alive()) { return {}; }
        std::promise<std::map<node_id, peer_progress>> result;
        asio::post(io_, [&] { result.set_value(raft_->progress()); });
        return result.get_future().get();
    }

private:
    node_id id_;
    History& history_;
    std::string state_path_;
    FakeLog log_;
    asio::io_context io_;
    std::unique_ptr<RaftStateFile> state_;
    std::unique_ptr<PeerServer> server_;
    std::unique_ptr<ConnectPerRpcClient> client_;
    std::unique_ptr<RaftNode> raft_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

class Cluster
{
public:
    // With a `state_dir`, node N keeps its term/vote in <state_dir>/nN.log.raftstate.
    // `logs` gives node N a fixed log (terms per entry); nodes not listed have an empty one.
    explicit Cluster(std::size_t n, raft_options opts = {}, std::filesystem::path state_dir = {},
                     std::map<node_id, std::vector<std::uint64_t>> logs = {})
        : opts_(std::move(opts)), state_dir_(std::move(state_dir)), logs_(std::move(logs)) {
        for (node_id id = 1; id <= n; ++id) {
            nodes_.push_back(std::make_unique<LiveNode>(id, history, 0, state_path(id), log_terms(id)));
        }
        for (const auto& node : nodes_) {
            eps_[node->id()] = tcp::endpoint(asio::ip::address_v4::loopback(), node->listen_port());
        }
        for (const auto& node : nodes_) { node->start(eps_, opts_); }
    }

    ~Cluster() { for (auto& n : nodes_) { n->kill(); } }

    LiveNode& node(node_id id) { return *nodes_.at(id - 1); }

    std::string state_path(node_id id) const {
        if (state_dir_.empty()) { return {}; }
        return RaftStateFile::path_for_log((state_dir_ / ("n" + std::to_string(id) + ".log")).string());
    }

    // Crash a node, then bring up a fresh one with the same id, port and state file.
    void restart(node_id id) {
        auto& slot = nodes_.at(id - 1);
        const std::uint16_t port = slot->port();
        slot.reset();
        slot = std::make_unique<LiveNode>(id, history, port, state_path(id), log_terms(id));
        slot->start(eps_, opts_);
    }

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
    std::vector<std::uint64_t> log_terms(node_id id) const {
        const auto it = logs_.find(id);
        return it == logs_.end() ? std::vector<std::uint64_t>{} : it->second;
    }

    raft_options opts_;
    std::filesystem::path state_dir_;
    std::map<node_id, std::vector<std::uint64_t>> logs_;
    peer_endpoints eps_;
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

// The happy path again with every node persisting term/vote: the fsyncs on the
// election path must not cost stability, and the files must match memory.
static void test_cluster3_persistent_state() {
    std::puts("test_cluster3_persistent_state");
    ::alarm(30);
    TempDir dir;
    CHECK(!dir.path.empty());
    Cluster c(3, {}, dir.path);
    const auto a = elect_and_hold(c, "3-node persistent", 1000ms);
    CHECK(c.history.at_most_one_leader_per_term());
    if (!a) { return; }
    const auto latest = c.history.latest();
    for (node_id id = 1; id <= 3; ++id) {
        const raft_hard_state disk = RaftStateFile(c.state_path(id), id).load();
        CHECK(disk.term == a->term);
        CHECK(disk.voted_for == latest.at(id).voted_for);  // file and memory agree
    }
    CHECK(RaftStateFile(c.state_path(a->leader), a->leader).load().voted_for == a->leader);
}

// A follower crashes and restarts on its saved state: it comes back at the
// current term and simply follows; no new election, no second leader.
static void test_cluster3_follower_restart() {
    std::puts("test_cluster3_follower_restart");
    ::alarm(30);
    TempDir dir;
    CHECK(!dir.path.empty());
    Cluster c(3, {}, dir.path);
    const auto a = elect_and_hold(c, "3-node persistent", 300ms);
    if (!a) { return; }
    const node_id follower = a->leader == 1 ? 2 : 1;
    const raft_hard_state before = RaftStateFile(c.state_path(follower), follower).load();
    CHECK(before.term == a->term);

    const long long t0 = c.history.now_ms();
    c.node(follower).kill();
    std::this_thread::sleep_for(100ms);  // down for a couple of heartbeats
    c.restart(follower);
    std::printf("  restarted follower node %llu (saved term %llu, voted for %llu)\n",
                static_cast<unsigned long long>(follower), static_cast<unsigned long long>(before.term),
                static_cast<unsigned long long>(before.voted_for));

    // Its first status after restart is the saved term, not term 0.
    bool saw_restart = false;
    for (const auto& e : c.history.events()) {
        if (e.at_ms > t0 && e.s.id == follower) {
            CHECK(e.s.term == before.term && e.s.voted_for == before.voted_for);
            saw_restart = true;
            break;
        }
    }
    CHECK(saw_restart);

    const auto b = c.await_agreement(2000ms);
    CHECK(b.has_value());
    if (b) { CHECK(b->leader == a->leader && b->term == a->term); }
    std::this_thread::sleep_for(1000ms);
    CHECK(c.history.elections_started_after(t0) == 0);
    const auto later = c.agreed();
    CHECK(later && later->leader == a->leader && later->term == a->term);
    CHECK(c.history.at_most_one_leader_per_term());
}

// Identical non-empty logs (three term-0 entries, as client writes make
// today): every probe succeeds the first time, so matchIndex lands on 3 and
// nextIndex never moves from 4. A rejection anywhere would leave it lower.
static void test_cluster3_identical_logs() {
    std::puts("test_cluster3_identical_logs");
    ::alarm(30);
    const std::vector<std::uint64_t> log = {0, 0, 0};
    Cluster c(3, {}, {}, {{1, log}, {2, log}, {3, log}});
    const auto a = elect_and_hold(c, "3-node identical logs", 1000ms);
    CHECK(c.history.at_most_one_leader_per_term());
    if (!a) { return; }
    const auto p = c.node(a->leader).progress();
    CHECK(p.size() == 2);
    for (const auto& [peer, pr] : p) {
        std::printf("  leader %llu: nextIndex[%llu]=%llu matchIndex[%llu]=%llu\n",
                    static_cast<unsigned long long>(a->leader), static_cast<unsigned long long>(peer),
                    static_cast<unsigned long long>(pr.next_index), static_cast<unsigned long long>(peer),
                    static_cast<unsigned long long>(pr.match_index));
        CHECK(pr.next_index == 4 && pr.match_index == 3);
    }
}

// One follower is behind (2 entries vs 3+2): it can't win an election, and
// the leader walks its nextIndex back over real sockets to 3 (prev 2 matches).
// Rejected heartbeats still keep it a follower: no churn.
static void test_cluster3_lagging_follower() {
    std::puts("test_cluster3_lagging_follower");
    ::alarm(30);
    const std::vector<std::uint64_t> full = {0, 0, 0, 0, 0};
    Cluster c(3, {}, {}, {{1, full}, {2, full}, {3, {0, 0}}});
    const auto a = elect_and_hold(c, "3-node, node 3 behind", 1000ms);
    CHECK(c.history.at_most_one_leader_per_term());
    if (!a) { return; }
    CHECK(a->leader != 3);
    const node_id other = a->leader == 1 ? 2 : 1;
    const auto p = c.node(a->leader).progress();
    std::printf("  leader %llu: nextIndex[%llu]=%llu/match %llu, nextIndex[3]=%llu/match %llu\n",
                static_cast<unsigned long long>(a->leader), static_cast<unsigned long long>(other),
                static_cast<unsigned long long>(p.at(other).next_index),
                static_cast<unsigned long long>(p.at(other).match_index),
                static_cast<unsigned long long>(p.at(3).next_index),
                static_cast<unsigned long long>(p.at(3).match_index));
    CHECK(p.at(other).next_index == 6 && p.at(other).match_index == 5);
    CHECK(p.at(3).next_index == 3 && p.at(3).match_index == 2);
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
    test_consistency_check();
    test_leader_progress();
    test_next_index_floor();
    test_reply_with_higher_term();
    test_backward_walk();

    test_cluster3_happy_path();
    test_heartbeats_suppress_elections();
    test_cluster3_failover();
    test_cluster5_with_two_failovers();
    test_minority_cannot_elect();
    test_repeated_elections();
    test_cluster3_persistent_state();
    test_cluster3_follower_restart();
    test_cluster3_identical_logs();
    test_cluster3_lagging_follower();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all raft election tests passed");
    return 0;
}
