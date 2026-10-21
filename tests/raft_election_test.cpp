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

// The entry every fake or seeded test log holds at `index` in `term`. The
// command depends on both, so entries from different terms really differ.
static log_entry fake_entry(std::uint64_t index, std::uint64_t term) {
    return {index, term, command{"PUT", "k" + std::to_string(index),
                                 "v" + std::to_string(index) + "t" + std::to_string(term)}};
}

// An in-memory log as a list of terms: terms[i - 1] is the term of entry i,
// whose contents are fake_entry(i, terms[i - 1]) unless the entry came in
// through append() (e.g. a leader's no-op or a client write), which keeps
// its real command. Ignores the byte cap (fake entries are tiny); the
// PersistentLog tests cover it.
struct FakeLog final : RaftLog
{
    std::vector<std::uint64_t> terms;
    std::map<std::uint64_t, command> commands;     // appended entries' real commands
    std::size_t max_batch = SIZE_MAX;              // lower to force one-entry batches
    std::vector<std::vector<log_entry>> appended;  // every append() batch, in order
    std::vector<std::uint64_t> truncations;        // every truncate_from() index, in order

    log_position last() const override {
        return terms.empty() ? log_position{} : log_position{terms.size(), terms.back()};
    }
    std::optional<std::uint64_t> term_at(std::uint64_t index) const override {
        if (index == 0) { return 0; }
        if (index > terms.size()) { return std::nullopt; }
        return terms[index - 1];
    }
    std::vector<log_entry> entries(std::uint64_t from, std::size_t max_count, std::size_t) const override {
        std::vector<log_entry> out;
        for (std::uint64_t i = from; i >= 1 && i <= terms.size() && out.size() < std::min(max_count, max_batch); ++i) {
            out.push_back(fake_entry(i, terms[i - 1]));
            if (const auto it = commands.find(i); it != commands.end()) { out.back().cmd = it->second; }
        }
        return out;
    }
    void truncate_from(std::uint64_t index) override {
        truncations.push_back(index);
        if (index >= 1 && index <= terms.size()) {
            terms.resize(index - 1);
            commands.erase(commands.lower_bound(index), commands.end());
        }
    }
    void append(std::span<const log_entry> batch) override {
        appended.emplace_back(batch.begin(), batch.end());
        for (const auto& e : batch) {
            CHECK(e.index == terms.size() + 1);
            terms.push_back(e.term);
            commands[e.index] = e.cmd;
        }
    }
    // `index` entries, all in `term`.
    void set_last(std::uint64_t index, std::uint64_t term) { terms.assign(index, term); }
};

// Writes a log file whose entry i is fake_entry(i, terms[i - 1]).
static void write_log_file(const std::filesystem::path& path, const std::vector<std::uint64_t>& terms) {
    std::filesystem::create_directories(path.parent_path());
    std::string frames;
    for (std::size_t i = 0; i < terms.size(); ++i) { frames += PersistentLog::encode_frame(fake_entry(i + 1, terms[i])); }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        std::fwrite(frames.data(), 1, frames.size(), f);
        std::fclose(f);
    }
}

static std::string read_file(const std::filesystem::path& path) {
    std::string out;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { out.append(buf, n); }
        std::fclose(f);
    }
    return out;
}

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
    FakeLog pos;
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

    // Heartbeats go out immediately to every peer, carrying the new
    // leader's no-op (index 8, after the 7 entries it had).
    CHECK(h.client.appends.size() == 4);
    for (const auto& a : h.client.appends) {
        CHECK(a.req.term == 1 && a.req.leader_id == 1 && a.req.prev_log_index == 7);
        CHECK(a.req.entries.size() == 1 && a.req.entries[0].index == 8 && a.req.entries[0].term == 1
              && a.req.entries[0].cmd.operation == kNoopOperation);
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
        // Leader gets RequestVote with a higher term: steps down (and may vote,
        // here for a candidate whose log has caught up with our no-op 1@1).
        Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
        make_leader(h);
        const auto r = h.node.handle_request_vote({3, 2, 1, 1});
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

// Starts the node and grants it votes for `term` until it wins.
static void make_leader(Harness& h, std::uint64_t term = 1) {
    h.node.start();
    CHECK(h.await_candidate(term));
    const auto votes = h.client.votes;  // a copy: winning sends more RPCs
    for (const auto& v : votes) {
        if (v.req.term == term && h.node.role() == raft_role::candidate) { v.cb(vote_reply(term, true)); }
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

    // The first heartbeats probe nextIndex - 1 = 3 with our term there and
    // carry the no-op the new leader appended at 4; nothing committed yet.
    CHECK(h.client.appends.size() == 2);
    for (const auto& a : h.client.appends) {
        CHECK(a.req.prev_log_index == 3 && a.req.prev_log_term == 1);
        CHECK(a.req.entries.size() == 1 && a.req.entries[0].index == 4);
        CHECK(a.req.leader_commit == 0 && a.req.term == 1 && a.req.leader_id == 1);
    }

    // Success from peer 2 only: matchIndex[2] = 3 + 1 sent = 4, nextIndex[2] = 5;
    // peer 3 untouched. (And with us, a majority has 4: committed.)
    appends_to(h.client, 2).at(0)->cb(append_reply(1, true));
    p = h.node.progress();
    CHECK(p[2].next_index == 5 && p[2].match_index == 4);
    CHECK(p[3].next_index == 4 && p[3].match_index == 0);
    CHECK(h.node.commit_index() == 4);

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
    CHECK(h.node.progress()[2].next_index == 5 && h.node.progress()[2].match_index == 4);
}

// Success advances matchIndex by exactly the entries the answered request
// carried; a late success for an older, shorter batch can't move it back.
static void test_success_advances_by_count() {
    std::puts("test_success_advances_by_count");
    Harness h(1, {2, 3}, quiet(50ms, 50ms, 20ms));
    h.log.terms = {1, 1, 1, 1, 1};
    make_leader(h);
    // Copies: `appends` reallocates as more heartbeats go out.
    const auto next_to_2 = [&](std::size_t& seen) {
        run_until(h.io, [&] { return !appends_to(h.client, 2, seen).empty(); }, 1000ms);
        auto* call = appends_to(h.client, 2, seen).front();
        seen = static_cast<std::size_t>(call - h.client.appends.data()) + 1;
        return FakeClient::append_call{call->peer, call->req, call->cb};
    };
    // The leader's log: 5 entries, then its no-op at 6.
    std::size_t seen = 0;
    const auto a = next_to_2(seen);   // prev 5 + [6]
    CHECK(a.req.prev_log_index == 5 && a.req.entries.size() == 1 && a.req.entries[0].index == 6);
    a.cb(append_reply(1, false));     // next 6 -> 5
    const auto b = next_to_2(seen);   // prev 4 + [5, 6]
    CHECK(b.req.prev_log_index == 4 && b.req.entries.size() == 2 && b.req.entries[0].index == 5);
    b.cb(append_reply(1, false));     // next 5 -> 4
    const auto c = next_to_2(seen);   // prev 3 + [4, 5, 6]
    CHECK(c.req.prev_log_index == 3 && c.req.entries.size() == 3);
    CHECK(c.req.entries[0].index == 4 && c.req.entries[0].term == 1 && c.req.entries[0].cmd.key == "k4");
    c.cb(append_reply(1, true));      // match = 3 + 3
    auto p = h.node.progress()[2];
    CHECK(p.match_index == 6 && p.next_index == 7);
    // A duplicate of an earlier, shorter batch succeeding late: no change.
    b.cb(append_reply(1, true));      // 4 + 2 = 6: same
    a.cb(append_reply(1, true));      // 5 + 1 = 6: same
    p = h.node.progress()[2];
    CHECK(p.match_index == 6 && p.next_index == 7);
    // Caught up: the next heartbeat carries no entries.
    const auto d = next_to_2(seen);
    CHECK(d.req.prev_log_index == 6 && d.req.prev_log_term == 1 && d.req.entries.empty());
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
    // Log now: 3 entries, term 1's no-op at 4; this term's no-op goes at 5.
    CHECK(h.node.role() == raft_role::leader);
    CHECK(h.node.progress()[3].next_index == 5);
    to3(append_reply(1, false));
    CHECK(h.node.progress()[3].next_index == 5 && h.node.role() == raft_role::leader);
    // A rejection carrying the same term is just a mismatch, not a step-down.
    appends_to(h.client, 3).back()->cb(append_reply(5, false));
    CHECK(h.node.role() == raft_role::leader && h.node.progress()[3].next_index == 4);
}

// Writes a log file of fake_entry(i, terms[i - 1]) and opens it.
static void build_log(PersistentLog& log, const std::filesystem::path& path,
                      const std::vector<std::uint64_t>& terms) {
    write_log_file(path, terms);
    log.open(path.string());
    StateMachine sm;
    CHECK(log.replay(sm) == terms.size());
}

static std::vector<std::uint64_t> terms_of(const PersistentLog& log) {
    std::vector<std::uint64_t> out;
    for (std::uint64_t i = 1; i <= log.last_index(); ++i) { out.push_back(*log.term_at(i)); }
    return out;
}

// ---- follower side: storing entries ----

// One follower on a real PersistentLog, fed AppendEntries by hand.
struct DiskFollower
{
    std::filesystem::path path;
    PersistentLog disk;
    std::unique_ptr<PersistentRaftLog> log;
    asio::io_context io;
    FakeClient client;
    std::unique_ptr<RaftNode> node;

    DiskFollower(const std::filesystem::path& p, const std::vector<std::uint64_t>& terms) : path(p) {
        build_log(disk, path, terms);
        log = std::make_unique<PersistentRaftLog>(disk);
        node = std::make_unique<RaftNode>(io, 2, std::vector<node_id>{1, 3}, client, *log, quiet(10s, 10s));
        node->start();
    }
};

static append_entries_request with_entries(std::uint64_t term, std::uint64_t prev_index, std::uint64_t prev_term,
                                           const std::vector<std::pair<std::uint64_t, std::uint64_t>>& index_terms) {
    append_entries_request req{term, 1, prev_index, prev_term, {}, 0};
    for (const auto& [i, t] : index_terms) { req.entries.push_back(fake_entry(i, t)); }
    return req;
}

static void test_follower_stores_entries() {
    std::puts("test_follower_stores_entries");
    TempDir dir;
    {
        // Missing entries: appended after the matching prev entry.
        DiskFollower f(dir.path / "append" / "kv.log", {1, 1});
        const auto r = f.node->handle_append_entries(with_entries(3, 2, 1, {{3, 2}, {4, 2}, {5, 3}}));
        CHECK(r.success && r.term == 3);
        CHECK(terms_of(f.disk) == std::vector<std::uint64_t>({1, 1, 2, 2, 3}));
        write_log_file(dir.path / "append" / "want.log", {1, 1, 2, 2, 3});
        CHECK(read_file(f.path) == read_file(dir.path / "append" / "want.log"));  // byte-identical
        // Nothing reached a state machine or commit index: this chunk only stores.
        CHECK(f.node->role() == raft_role::follower && f.node->status().leader_id == 1);
    }
    {
        // An empty log takes entries from index 1 (prev_log_index 0). A
        // success here that stored nothing would be a false acknowledgement.
        DiskFollower f(dir.path / "empty" / "kv.log", {});
        CHECK(f.node->handle_append_entries(with_entries(2, 0, 0, {{1, 1}, {2, 2}})).success);
        CHECK(terms_of(f.disk) == std::vector<std::uint64_t>({1, 2}));
        // And a non-empty one whose very first entry conflicts is replaced whole.
        DiskFollower g(dir.path / "first" / "kv.log", {1, 1});
        CHECK(g.node->handle_append_entries(with_entries(3, 0, 0, {{1, 3}})).success);
        CHECK(terms_of(g.disk) == std::vector<std::uint64_t>({3}));
    }
    {
        // Divergent tail: entries 3..5 are in term 2, the leader has term 3 from
        // index 3. Delete 3..5, store the leader's; 1..2 untouched.
        DiskFollower f(dir.path / "conflict" / "kv.log", {1, 1, 2, 2, 2});
        const std::string before = read_file(f.path);
        const std::size_t prefix = PersistentLog::encode_frame(fake_entry(1, 1)).size()
                                 + PersistentLog::encode_frame(fake_entry(2, 1)).size();
        const auto r = f.node->handle_append_entries(with_entries(3, 2, 1, {{3, 3}, {4, 3}}));
        CHECK(r.success);
        CHECK(terms_of(f.disk) == std::vector<std::uint64_t>({1, 1, 3, 3}));
        const std::string after = read_file(f.path);
        CHECK(after.compare(0, prefix, before, 0, prefix) == 0);
        write_log_file(dir.path / "conflict" / "want.log", {1, 1, 3, 3});
        CHECK(after == read_file(dir.path / "conflict" / "want.log"));
    }
    {
        // A conflict found partway through the batch: 3 matches, 4 conflicts.
        DiskFollower f(dir.path / "midbatch" / "kv.log", {1, 1, 2, 2, 2, 2});
        CHECK(f.node->handle_append_entries(with_entries(4, 2, 1, {{3, 2}, {4, 4}, {5, 4}})).success);
        CHECK(terms_of(f.disk) == std::vector<std::uint64_t>({1, 1, 2, 4, 4}));
    }
    {
        // Idempotency: the same request again changes nothing, still succeeds.
        DiskFollower f(dir.path / "idem" / "kv.log", {1, 1});
        const auto req = with_entries(3, 1, 1, {{2, 1}, {3, 3}, {4, 3}});
        CHECK(f.node->handle_append_entries(req).success);
        const std::string once = read_file(f.path);
        CHECK(f.node->handle_append_entries(req).success);
        CHECK(read_file(f.path) == once);
        // An older, shorter request (a retransmission overtaken by a later
        // one) must not delete the entries after it: they match, so no conflict.
        CHECK(f.node->handle_append_entries(with_entries(3, 1, 1, {{2, 1}})).success);
        CHECK(f.node->handle_append_entries(with_entries(3, 0, 0, {})).success);
        CHECK(read_file(f.path) == once);
        CHECK(terms_of(f.disk) == std::vector<std::uint64_t>({1, 1, 3, 3}));
    }
    {
        // A failed consistency check stores nothing, even with entries attached.
        DiskFollower f(dir.path / "nocheck" / "kv.log", {1, 1});
        const std::string before = read_file(f.path);
        CHECK(!f.node->handle_append_entries(with_entries(3, 3, 1, {{4, 3}})).success);  // no entry 3
        CHECK(!f.node->handle_append_entries(with_entries(3, 2, 2, {{3, 3}})).success);  // entry 2 is term 1
        // Entries whose indices don't follow prev_log_index are malformed: refused.
        CHECK(!f.node->handle_append_entries(with_entries(3, 2, 1, {{4, 3}})).success);
        CHECK(!f.node->handle_append_entries(with_entries(3, 2, 1, {{3, 3}, {5, 3}})).success);
        CHECK(read_file(f.path) == before);
    }
    {
        // Durability: what the follower acknowledged survives a crash.
        const auto path = dir.path / "crash" / "kv.log";
        std::string acked;
        {
            DiskFollower f(path, {1});
            CHECK(f.node->handle_append_entries(with_entries(2, 1, 1, {{2, 2}, {3, 2}})).success);
            acked = read_file(path);
        }  // crash: node and log gone without any shutdown step
        PersistentLog reopened;
        reopened.open(path.string());
        StateMachine sm;
        CHECK(reopened.replay(sm) == 3);
        CHECK(terms_of(reopened) == std::vector<std::uint64_t>({1, 2, 2}));
        CHECK(reopened.recovered_tail_bytes() == 0 && read_file(path) == acked);
    }
}

// ---- leader side: sending entries ----

// A leader and a follower on real logs, the leader driven by its own timers
// and every AppendEntries to node 2 delivered to the follower by hand.
struct DiskPair
{
    PersistentLog leader_disk;
    std::unique_ptr<PersistentRaftLog> leader_log;
    std::unique_ptr<RaftStateFile> leader_state;
    asio::io_context leader_io;
    FakeClient leader_client;
    std::unique_ptr<RaftNode> leader;
    std::unique_ptr<DiskFollower> follower;
    std::size_t seen = 0;

    struct round { append_entries_request req; append_entries_response resp; peer_progress after; std::uint64_t follower_last; };
    std::vector<round> rounds;

    DiskPair(const std::filesystem::path& dir, const std::vector<std::uint64_t>& leader_terms,
             const std::vector<std::uint64_t>& follower_terms, std::uint64_t start_term,
             std::chrono::milliseconds heartbeat = 20ms) {
        build_log(leader_disk, dir / "leader" / "kv.log", leader_terms);
        leader_log = std::make_unique<PersistentRaftLog>(leader_disk);
        // Resume past every term in either log, as a real leader would be.
        leader_state = std::make_unique<RaftStateFile>((dir / "leader" / "kv.log.raftstate").string(), 1);
        leader_state->save({start_term, 0});
        raft_options lo = quiet(50ms, 50ms, heartbeat);
        lo.state = leader_state.get();
        leader = std::make_unique<RaftNode>(leader_io, 1, std::vector<node_id>{2, 3}, leader_client, *leader_log, lo);
        follower = std::make_unique<DiskFollower>(dir / "follower" / "kv.log", follower_terms);
        leader->start();
        run_until(leader_io, [&] { return !leader_client.votes.empty(); }, 2000ms);
        leader_client.votes.at(0).cb(vote_reply(start_term + 1, true));
        CHECK(leader->role() == raft_role::leader);
    }

    // Delivers the next heartbeat to node 2 and its reply back.
    bool step() {
        if (!run_until(leader_io, [&] { return !appends_to(leader_client, 2, seen).empty(); }, 1000ms)) { return false; }
        auto* call = appends_to(leader_client, 2, seen).front();
        seen = static_cast<std::size_t>(call - leader_client.appends.data()) + 1;
        const append_entries_request req = call->req;
        const append_entries_response resp = follower->node->handle_append_entries(req);
        call->cb({rpc_error::none, resp});
        rounds.push_back({req, resp, leader->progress()[2], follower->disk.last_index()});
        return true;
    }

    bool converged() const { return terms_of(follower->disk) == terms_of(leader_disk); }
};

static std::string describe(const DiskPair::round& r) {
    std::string out = "prev " + std::to_string(r.req.prev_log_index) + "@t" + std::to_string(r.req.prev_log_term);
    if (!r.req.entries.empty()) {
        out += " +" + std::to_string(r.req.entries.size()) + " [" + std::to_string(r.req.entries.front().index)
             + ".." + std::to_string(r.req.entries.back().index) + "]";
    }
    return out + (r.resp.success ? " ok   -> " : " FAIL -> ") + "next " + std::to_string(r.after.next_index)
         + "/match " + std::to_string(r.after.match_index) + ", follower last " + std::to_string(r.follower_last);
}

// The backward walk, then the repair, on real PersistentLogs that diverge after index 4:
//   index:     1 2 3 4 | 5 6 7 8
//   leader:    1 1 1 2 | 2 3 3 5   (8 is the no-op it appends on winning term 5)
//   follower:  1 1 1 2 | 4 4
// nextIndex steps 8 -> 5; the probe at prev 4 matches, the follower deletes
// its 5..6 and stores the leader's 5..8; then matchIndex = 8, the no-op (a
// term-5 entry) is on a majority, so 8 commits, and the follower learns it.
static void test_walk_back_and_repair() {
    std::puts("test_walk_back_and_repair");
    ::alarm(20);
    TempDir dir;
    DiskPair d(dir.path, {1, 1, 1, 2, 2, 3, 3}, {1, 1, 1, 2, 4, 4}, 4);
    CHECK(d.leader->current_term() == 5);
    for (int i = 0; i < 8; ++i) { CHECK(d.step()); }
    std::printf("  start: next 8/match 0, follower last 6\n");
    for (const auto& r : d.rounds) { std::printf("    %s\n", describe(r).c_str()); }

    CHECK(d.rounds.size() == 8);
    if (d.rounds.size() == 8) {
        const std::vector<std::uint64_t> want_next = {7, 6, 5, 9, 9, 9, 9, 9};
        const std::vector<std::uint64_t> want_match = {0, 0, 0, 8, 8, 8, 8, 8};
        const std::vector<std::size_t> want_sent = {1, 2, 3, 4, 0, 0, 0, 0};
        const std::vector<std::uint64_t> want_commit = {0, 0, 0, 0, 8, 8, 8, 8};  // leader_commit carried
        for (std::size_t i = 0; i < d.rounds.size(); ++i) {
            CHECK(d.rounds[i].after.next_index == want_next[i] && d.rounds[i].after.match_index == want_match[i]);
            CHECK(d.rounds[i].req.entries.size() == want_sent[i]);
            CHECK(d.rounds[i].req.leader_commit == want_commit[i]);
        }
    }
    CHECK(d.leader->commit_index() == 8 && d.follower->node->commit_index() == 8);
    CHECK(d.converged());
    CHECK(read_file(d.follower->path) == read_file(dir.path / "leader" / "kv.log"));  // byte-identical
    CHECK(d.follower->node->role() == raft_role::follower && d.follower->node->current_term() == 5);
    CHECK(d.leader->role() == raft_role::leader && d.leader->current_term() == 5);
}

// A follower far behind catches up over several bounded batches.
static void test_batched_catch_up() {
    std::puts("test_batched_catch_up");
    ::alarm(60);
    TempDir dir;
    std::vector<std::uint64_t> leader_terms(200, 1);
    for (std::size_t i = 100; i < 200; ++i) { leader_terms[i] = 2; }
    const std::vector<std::uint64_t> follower_terms(10, 1);
    DiskPair d(dir.path, leader_terms, follower_terms, 2, 1ms);
    while (!d.converged() && d.rounds.size() < 400) { CHECK(d.step()); }
    for (int i = 0; i < 5; ++i) { CHECK(d.step()); }  // and then it stays quiet

    std::size_t max_batch = 0;
    std::vector<std::size_t> batches;  // sizes of the batches that were stored
    std::string trace = "10";
    for (const auto& r : d.rounds) {
        max_batch = std::max(max_batch, r.req.entries.size());
        if (r.resp.success && !r.req.entries.empty()) {
            batches.push_back(r.req.entries.size());
            trace += " -> " + std::to_string(r.follower_last);
        }
    }
    std::printf("  %zu rounds; follower last index: %s; largest batch %zu\n", d.rounds.size(), trace.c_str(), max_batch);
    CHECK(d.converged());
    CHECK(read_file(d.follower->path) == read_file(dir.path / "leader" / "kv.log"));
    CHECK(max_batch == kMaxEntriesPerAppend);
    // 190 behind plus the leader's no-op: 191 entries.
    CHECK(batches == std::vector<std::size_t>({64, 64, 63}));
    CHECK(d.leader->progress()[2].match_index == 201 && d.leader->progress()[2].next_index == 202);
    for (std::size_t i = d.rounds.size() - 5; i < d.rounds.size(); ++i) {
        CHECK(d.rounds[i].resp.success && d.rounds[i].req.entries.empty() && d.rounds[i].follower_last == 201);
    }
}

// Big entries: batches are cut by bytes, not count, and still converge.
static void test_batch_byte_cap() {
    std::puts("test_batch_byte_cap");
    ::alarm(30);
    TempDir dir;
    const std::filesystem::path leader_path = dir.path / "leader" / "kv.log";
    // 12 entries of ~300 KB: three fit under kMaxAppendBytes, four don't.
    std::string frames;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        frames += PersistentLog::encode_frame({i, 1, command{"PUT", "big" + std::to_string(i), std::string(300 * 1024, 'a' + i % 26)}});
    }
    std::filesystem::create_directories(leader_path.parent_path());
    if (std::FILE* f = std::fopen(leader_path.c_str(), "wb")) { std::fwrite(frames.data(), 1, frames.size(), f); std::fclose(f); }

    PersistentLog leader_disk;
    leader_disk.open(leader_path.string());
    StateMachine sm;
    CHECK(leader_disk.replay(sm) == 12);
    PersistentRaftLog leader_log(leader_disk);
    asio::io_context io;
    FakeClient client;
    raft_options lo = quiet(50ms, 50ms, 1ms);
    RaftNode leader(io, 1, {2, 3}, client, leader_log, lo);
    DiskFollower follower(dir.path / "follower" / "kv.log", {});
    leader.start();
    run_until(io, [&] { return !client.votes.empty(); }, 2000ms);
    client.votes.at(0).cb(vote_reply(1, true));

    // With a 1ms heartbeat several requests are in flight at once: deliver
    // all pending ones each round, in order, and record a batch only when it
    // grew the follower's log (re-deliveries are idempotent no-ops).
    std::size_t seen = 0;
    std::vector<std::size_t> stored;
    std::size_t largest_bytes = 0;
    for (int round = 0; round < 200 && follower.disk.last_index() < 13; ++round) {
        if (!run_until(io, [&] { return !appends_to(client, 2, seen).empty(); }, 1000ms)) { break; }
        const std::size_t upto = client.appends.size();
        for (std::size_t i = seen; i < upto; ++i) {
            if (client.appends[i].peer != 2) { continue; }
            const append_entries_request req = client.appends[i].req;
            std::size_t bytes = 0;
            for (const auto& e : req.entries) { bytes += PersistentLog::encode_frame(e).size(); }
            largest_bytes = std::max(largest_bytes, bytes);
            CHECK(req.entries.size() <= 1 || bytes <= kMaxAppendBytes);
            std::string wire;
            CHECK(encode_rpc_frame(req, wire));  // always fits in one RPC frame
            const std::uint64_t before = follower.disk.last_index();
            const auto resp = follower.node->handle_append_entries(req);
            if (follower.disk.last_index() > before) { stored.push_back(follower.disk.last_index() - before); }
            client.appends[i].cb({rpc_error::none, resp});
        }
        seen = upto;
    }
    std::printf("  stored batches of:");
    for (const auto n : stored) { std::printf(" %zu", n); }
    std::printf(" entries (~300 KB each); largest request %zu bytes of frames, cap %zu\n", largest_bytes, kMaxAppendBytes);
    // The last batch also carries the leader's small no-op (13): it still fits.
    CHECK(stored == std::vector<std::size_t>({3, 3, 3, 4}));
    CHECK(read_file(follower.path) == read_file(leader_path));
}

// ---- commit and apply (fake transport) ----

// Records every apply in order, into a real StateMachine.
struct Applier
{
    StateMachine sm;
    std::vector<std::uint64_t> order;  // indices, in the order applied
    std::vector<std::uint64_t> terms;  // their terms

    std::function<std::string(const log_entry&)> fn() {
        return [this](const log_entry& e) {
            order.push_back(e.index);
            terms.push_back(e.term);
            return sm.apply(e.cmd);
        };
    }
};

static raft_options with_apply(raft_options o, Applier& a) {
    o.apply = a.fn();
    return o;
}

// Every call to `peer` since `seen`, copied (`appends` reallocates).
static std::vector<FakeClient::append_call> calls_to(FakeClient& c, node_id peer, std::size_t& seen) {
    std::vector<FakeClient::append_call> out;
    for (std::size_t i = seen; i < c.appends.size(); ++i) {
        if (c.appends[i].peer == peer) { out.push_back(c.appends[i]); }
    }
    seen = c.appends.size();
    return out;
}

// Succeeds the latest request to `peer` (the one covering the most entries).
static void ack_latest(Harness& h, node_id peer, std::uint64_t term) {
    std::size_t seen = 0;
    auto calls = calls_to(h.client, peer, seen);
    CHECK(!calls.empty());
    if (!calls.empty()) { calls.back().cb(append_reply(term, true)); }
}

static void test_commit_needs_majority() {
    std::puts("test_commit_needs_majority");
    for (const std::size_t n : {3, 5}) {
        Applier a;
        std::vector<node_id> peers;
        for (node_id id = 2; id <= n; ++id) { peers.push_back(id); }
        Harness h(1, peers, with_apply(quiet(50ms, 50ms, 10s), a));
        make_leader(h);  // no-op at 1
        std::optional<raft_outcome> done;
        CHECK(h.node.propose(command{"PUT", "k", "v"}, [&](const raft_outcome& o) { done = o; }));
        CHECK(h.node.commit_index() == 0 && !done);

        // Peers ack one by one; the commit lands exactly when a majority
        // (counting the leader) has index 2: 2 of 3, or 3 of 5.
        const std::size_t need = h.node.majority() - 1;  // acks besides the leader's own copy
        for (std::size_t k = 0; k < peers.size(); ++k) {
            ack_latest(h, peers[k], 1);
            const bool should = k + 1 >= need;
            CHECK((h.node.commit_index() == 2) == should);
            CHECK(done.has_value() == should);
        }
        CHECK(done && done->type == raft_outcome::kind::applied && done->result == "OK");
        CHECK(a.order == std::vector<std::uint64_t>({2}));  // the no-op at 1 is never handed to apply
        CHECK(h.node.last_applied() == 2 && a.sm.get(command{"GET", "k", std::nullopt}) == "v");
        std::printf("  %zu nodes: committed index 2 after %zu follower ack(s)\n", n, need);
    }
}

// Figure 8: an entry from an earlier term that has reached a majority is NOT
// committed by that alone; it commits only once an entry from the leader's
// own term above it does.
//   leader log:  1@t1  2@t2  | 3@t4 (its no-op on winning term 4)
static void test_prior_term_entry_not_committed_alone() {
    std::puts("test_prior_term_entry_not_committed_alone");
    TempDir dir;
    RaftStateFile state((dir.path / "leader.raftstate").string(), 1);
    state.save({3, 0, 0});
    Applier a;
    raft_options o = with_apply(quiet(50ms, 50ms, 10s), a);
    o.state = &state;
    Harness h(1, {2, 3}, o);
    h.log.terms = {1, 2};
    h.log.max_batch = 1;  // one entry per request, so a follower can match 2 without 3
    make_leader(h, 4);
    CHECK(h.log.terms == std::vector<std::uint64_t>({1, 2, 4}));

    std::size_t seen = 0;
    auto first = calls_to(h.client, 2, seen);  // prev 2@2 + [3@4]
    CHECK(first.size() == 1 && first[0].req.prev_log_index == 2 && first[0].req.entries.size() == 1);
    first[0].cb(append_reply(4, false));       // follower 2 lacks 2: next 3 -> 2
    // Heartbeats are 10s apart here; a proposal sends to every peer at once,
    // which delivers the next probe now (it also appends 4@4, unused).
    CHECK(h.node.propose(command{"PUT", "later", "x"}, [](const raft_outcome&) {}));
    auto second = calls_to(h.client, 2, seen);
    CHECK(!second.empty());
    const auto probe = second.back();          // prev 1@1 + [2@2]
    CHECK(probe.req.prev_log_index == 1 && probe.req.entries.size() == 1 && probe.req.entries[0].term == 2);
    probe.cb(append_reply(4, true));           // follower 2 now has 1..2

    // Index 2 is on a majority (leader + follower 2), but it's from term 2.
    CHECK(h.node.progress()[2].match_index == 2);
    CHECK(h.node.commit_index() == 0);
    CHECK(a.order.empty());
    std::puts("  index 2 (term 2) on 2 of 3 nodes: not committed");

    // Follower 2 then stores the leader's term-4 no-op at 3: that commits 3,
    // and with it 2 (and 1), indirectly.
    h.node.propose(command{"PUT", "again", "y"}, [](const raft_outcome&) {});  // sends now (and appends 5@4)
    auto third = calls_to(h.client, 2, seen);
    CHECK(!third.empty() && third.back().req.prev_log_index == 2 && third.back().req.entries[0].index == 3);
    third.back().cb(append_reply(4, true));
    CHECK(h.node.commit_index() == 3);
    CHECK(a.order == std::vector<std::uint64_t>({1, 2}));  // 3 is the no-op: not handed to apply
    CHECK(a.terms == std::vector<std::uint64_t>({1, 2}));
    CHECK(state.load().commit_index == 3);
    std::puts("  index 3 (term 4) on 2 of 3 nodes: commits 3, and 1..2 with it");
}

// Applies happen in index order, each index once, however replies arrive.
static void test_apply_in_order_exactly_once() {
    std::puts("test_apply_in_order_exactly_once");
    Applier a;
    Harness h(1, {2, 3}, with_apply(quiet(50ms, 50ms, 10s), a));
    make_leader(h);
    std::vector<std::string> results;
    for (int i = 0; i < 10; ++i) {
        CHECK(h.node.propose(command{"PUT", "k" + std::to_string(i % 3), "v" + std::to_string(i)},
                             [&](const raft_outcome& o) { results.push_back(o.result); }));
    }
    CHECK(h.node.propose(command{"DEL", "k0", std::nullopt}, [&](const raft_outcome& o) { results.push_back(o.result); }));
    CHECK(h.node.propose(command{"DEL", "k0", std::nullopt}, [&](const raft_outcome& o) { results.push_back(o.result); }));
    // Every request ever sent gets a success, newest first, then all again.
    std::size_t seen = 0;
    auto all = calls_to(h.client, 2, seen);
    for (auto it = all.rbegin(); it != all.rend(); ++it) { it->cb(append_reply(1, true)); }
    for (auto& c : all) { c.cb(append_reply(1, true)); }
    ack_latest(h, 3, 1);
    CHECK(h.node.commit_index() == 13 && h.node.last_applied() == 13);
    std::vector<std::uint64_t> want;
    for (std::uint64_t i = 2; i <= 13; ++i) { want.push_back(i); }
    CHECK(a.order == want);  // 2..13 once each, in order (1 is the no-op)
    CHECK(results.size() == 12);
    if (results.size() == 12) {
        CHECK(results[10] == "OK" && results[11] == "NOT_FOUND");  // the second DEL sees the first's effect
    }
    CHECK(a.sm.get(command{"GET", "k1", std::nullopt}) == "v7" && a.sm.get(command{"GET", "k2", std::nullopt}) == "v8");
}

// Followers learn the commit index from leader_commit, capped at the last
// entry the request covered, never further into a tail it didn't check.
static void test_follower_learns_commit() {
    std::puts("test_follower_learns_commit");
    Applier a;
    Harness h(1, {2, 3}, with_apply(quiet(10s, 10s), a));
    h.log.terms = {1, 1, 2, 2};  // 3..4 are a stale term-2 tail the leader doesn't have
    h.node.start();
    // Heartbeat from the term-3 leader: matches at 2, carries no entries,
    // leader_commit 4. Only 1..2 are known to match: commit 2, not 4.
    auto r = h.node.handle_append_entries({3, 2, 2, 1, {}, 4});
    CHECK(r.success && h.node.commit_index() == 2);
    CHECK(a.order == std::vector<std::uint64_t>({1, 2}));
    // Now the leader sends its 3 (term 3): our 3..4 conflict and are replaced,
    // and commit moves to 3, never having applied the stale ones.
    r = h.node.handle_append_entries({3, 2, 2, 1, {fake_entry(3, 3)}, 4});
    CHECK(r.success && h.node.commit_index() == 3 && h.log.terms == std::vector<std::uint64_t>({1, 1, 3}));
    CHECK(a.order == std::vector<std::uint64_t>({1, 2, 3}) && a.terms == std::vector<std::uint64_t>({1, 1, 3}));
    // A lower leader_commit (a stale message) never moves it back.
    CHECK(h.node.handle_append_entries({3, 2, 3, 3, {}, 1}).success);
    CHECK(h.node.commit_index() == 3 && h.node.last_applied() == 3);
    // A rejected request (consistency check fails) teaches nothing.
    CHECK(!h.node.handle_append_entries({3, 2, 9, 3, {}, 9}).success);
    CHECK(h.node.commit_index() == 3);
}

// Losing leadership fails every pending request instead of leaving it waiting.
static void test_lost_leadership_fails_pending() {
    std::puts("test_lost_leadership_fails_pending");
    Applier a;
    Harness h(1, {2, 3}, with_apply(quiet(50ms, 50ms, 10s), a));
    // Not leader: nothing accepted, and the hint names who is (once known).
    CHECK(!h.node.propose(command{"PUT", "k", "v"}, [](const raft_outcome&) { CHECK(false); }));
    CHECK(!h.node.read_barrier([](const raft_outcome&) { CHECK(false); }));
    h.node.start();
    CHECK(h.node.handle_append_entries({1, 3, 0, 0, {}, 0}).success);
    CHECK(h.node.leader_hint() == 3);
    CHECK(!h.node.propose(command{"PUT", "k", "v"}, [](const raft_outcome&) { CHECK(false); }));

    // As leader of term 3: a write and a read wait (the no-op isn't committed yet).
    CHECK(h.await_candidate(2));
    for (auto& v : h.client.votes) { if (v.req.term == 2) { v.cb(vote_reply(2, true)); break; } }
    CHECK(h.node.role() == raft_role::leader);
    std::vector<raft_outcome> outcomes;
    CHECK(h.node.propose(command{"PUT", "k", "v"}, [&](const raft_outcome& o) { outcomes.push_back(o); }));
    CHECK(h.node.read_barrier([&](const raft_outcome& o) { outcomes.push_back(o); }));
    CHECK(outcomes.empty());
    // A higher term arrives: step down, and both fail at once.
    std::size_t seen = 0;
    auto calls = calls_to(h.client, 2, seen);
    CHECK(!calls.empty());
    if (!calls.empty()) { calls.back().cb(append_reply(7, false)); }
    CHECK(h.node.role() == raft_role::follower);
    CHECK(outcomes.size() == 2);
    for (const auto& o : outcomes) { CHECK(o.type == raft_outcome::kind::lost_leadership); }
    CHECK(a.order.empty() && h.node.commit_index() == 0);
    // Replies to the old term's requests change nothing now.
    for (auto& c : calls) { c.cb(append_reply(2, true)); }
    CHECK(h.node.commit_index() == 0 && outcomes.size() == 2);
}

// A read on a new leader waits until it has applied its own no-op, i.e.
// everything committed before its term; then it is served at once.
static void test_read_barrier() {
    std::puts("test_read_barrier");
    Applier a;
    Harness h(1, {2, 3}, with_apply(quiet(50ms, 50ms, 10s), a));
    make_leader(h);
    int served = 0;
    CHECK(h.node.read_barrier([&](const raft_outcome& o) { served += o.type == raft_outcome::kind::applied; }));
    CHECK(served == 0);
    ack_latest(h, 2, 1);  // no-op committed and applied
    CHECK(served == 1);
    CHECK(h.node.read_barrier([&](const raft_outcome& o) { served += o.type == raft_outcome::kind::applied; }));
    CHECK(served == 2);   // immediately this time
}

// ---- restart: apply only what is known committed ----

static void test_restart_applies_only_committed() {
    std::puts("test_restart_applies_only_committed");
    TempDir dir;
    const auto log_path = dir.path / "n2" / "kv.log";
    write_log_file(log_path, {1, 1, 1, 2, 2});  // 5 entries; only 1..3 known committed
    RaftStateFile state(RaftStateFile::path_for_log(log_path.string()), 2);
    state.save({2, 0, 3});

    const auto boot = [&](Applier& a) {
        auto disk = std::make_unique<PersistentLog>();
        disk->open(log_path.string());
        StateMachine unused;
        CHECK(disk->replay(unused, 0) == disk->last_index());  // loads, applies nothing
        CHECK(unused.list() == "(empty)");
        return disk;
    };
    {
        Applier a;
        auto disk = boot(a);
        PersistentRaftLog log(*disk);
        asio::io_context io;
        FakeClient client;
        raft_options o = with_apply(quiet(10s, 10s), a);
        o.state = &state;
        RaftNode node(io, 2, {1, 3}, client, log, o);
        node.start();
        CHECK(a.order == std::vector<std::uint64_t>({1, 2, 3}));  // not 4, 5
        CHECK(node.commit_index() == 3 && node.last_applied() == 3 && disk->last_index() == 5);
        CHECK(a.sm.get(command{"GET", "k4", std::nullopt}) == "NOT_FOUND");
        CHECK(a.sm.get(command{"GET", "k3", std::nullopt}) == "v3t1");
        // 4..5 are still in the log, so the new leader can match them...
        CHECK(node.handle_append_entries({3, 1, 5, 2, {}, 4}).success);
        CHECK(a.order == std::vector<std::uint64_t>({1, 2, 3, 4}));  // ...and commit them
    }
    {
        // Same start, but the new leader has something else at 4: the
        // uncommitted 4..5 are truncated, and were never applied.
        state.save({2, 0, 3});
        write_log_file(log_path, {1, 1, 1, 2, 2});
        Applier a;
        auto disk = boot(a);
        PersistentRaftLog log(*disk);
        asio::io_context io;
        FakeClient client;
        raft_options o = with_apply(quiet(10s, 10s), a);
        o.state = &state;
        RaftNode node(io, 2, {1, 3}, client, log, o);
        node.start();
        CHECK(node.handle_append_entries({3, 1, 3, 1, {fake_entry(4, 3)}, 4}).success);
        CHECK(disk->last_index() == 4 && *disk->term_at(4) == 3);
        CHECK(a.order == std::vector<std::uint64_t>({1, 2, 3, 4}) && a.terms.back() == 3);
        CHECK(a.sm.get(command{"GET", "k4", std::nullopt}) == "v4t3");  // the leader's 4, not ours
        CHECK(a.sm.get(command{"GET", "k5", std::nullopt}) == "NOT_FOUND");
    }
}

// The commit index on disk never leads the real one: it is saved before the
// entries it covers are applied, and a crash in between only leaves it
// behind. A saved value past the end of the log is clamped, not trusted.
static void test_commit_persistence_is_conservative() {
    std::puts("test_commit_persistence_is_conservative");
    TempDir dir;
    RaftStateFile state((dir.path / "n1.raftstate").string(), 1);
    // (1) Saved before apply: when entry i is applied, disk already says >= i.
    {
        Applier a;
        raft_options o = with_apply(quiet(50ms, 50ms, 10s), a);
        o.state = &state;
        bool ok = true;
        o.apply = [&](const log_entry& e) {
            ok = ok && state.load().commit_index >= e.index;
            return a.sm.apply(e.cmd);
        };
        Harness h(1, {2, 3}, o);
        make_leader(h);
        for (int i = 0; i < 5; ++i) { h.node.propose(command{"PUT", "k", "v"}, [](const raft_outcome&) {}); }
        ack_latest(h, 2, 1);
        CHECK(h.node.commit_index() == 6 && ok && state.load().commit_index == 6);
    }
    // (2) A crash after commit but before the save: disk says 2, truth is 5.
    //     Restart applies 1..2 only, then learns the rest from the leader.
    {
        TempDir d2;
        const auto log_path = d2.path / "kv.log";
        write_log_file(log_path, {1, 1, 1, 1, 1});
        RaftStateFile lagging(RaftStateFile::path_for_log(log_path.string()), 2);
        lagging.save({1, 0, 2});
        PersistentLog disk;
        disk.open(log_path.string());
        StateMachine unused;
        disk.replay(unused, 0);
        PersistentRaftLog log(disk);
        Applier a;
        asio::io_context io;
        FakeClient client;
        raft_options o = with_apply(quiet(10s, 10s), a);
        o.state = &lagging;
        RaftNode node(io, 2, {1, 3}, client, log, o);
        node.start();
        CHECK(a.order == std::vector<std::uint64_t>({1, 2}));
        CHECK(node.handle_append_entries({1, 1, 5, 1, {}, 5}).success);
        CHECK(a.order == std::vector<std::uint64_t>({1, 2, 3, 4, 5}) && lagging.load().commit_index == 5);
    }
    // (3) A saved commit index beyond the log (can't happen, but don't trust
    //     it): clamped to the log's end, nothing invented.
    {
        TempDir d3;
        const auto log_path = d3.path / "kv.log";
        write_log_file(log_path, {1, 1});
        RaftStateFile over(RaftStateFile::path_for_log(log_path.string()), 2);
        over.save({1, 0, 99});
        PersistentLog disk;
        disk.open(log_path.string());
        StateMachine unused;
        disk.replay(unused, 0);
        PersistentRaftLog log(disk);
        Applier a;
        asio::io_context io;
        FakeClient client;
        raft_options o = with_apply(quiet(10s, 10s), a);
        o.state = &over;
        RaftNode node(io, 2, {1, 3}, client, log, o);
        node.start();
        CHECK(node.commit_index() == 2 && a.order == std::vector<std::uint64_t>({1, 2}));
    }
}

// Callbacks that arrive after the node is gone must not touch it.
static void test_late_callbacks_after_destruction() {
    std::puts("test_late_callbacks_after_destruction");
    asio::io_context io;
    FakeClient client;
    PeerHandlers handlers;
    {
        FakeLog log;
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
    // With a `log_path` the node runs on a real PersistentLog there (replayed
    // at start); otherwise on a FakeLog holding `log_terms`.
    LiveNode(node_id id, History& history, std::uint16_t port = 0, std::string state_path = {},
             std::vector<std::uint64_t> log_terms = {}, std::string log_path = {})
        : id_(id), history_(history), state_path_(std::move(state_path)), log_path_(std::move(log_path)) {
        fake_log_.terms = std::move(log_terms);
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
        opts.apply = [this](const log_entry& e) { return sm_.apply(e.cmd); };
        RaftLog* log = &fake_log_;
        if (!log_path_.empty()) {
            disk_log_ = std::make_unique<PersistentLog>();
            disk_log_->open(log_path_);
            StateMachine unused;  // load only: Raft applies the committed prefix itself
            disk_log_->replay(unused, 0);
            raft_log_ = std::make_unique<PersistentRaftLog>(*disk_log_);
            log = raft_log_.get();
        }
        raft_ = std::make_unique<RaftNode>(io_, id_, peers, *client_, *log, opts);
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
        raft_log_.reset();
        disk_log_.reset();
    }

    std::uint16_t listen_port() const { return server_->local_endpoint().port(); }

    // The node's leader-side progress, read on its own io thread.
    std::map<node_id, peer_progress> progress() {
        if (!alive()) { return {}; }
        std::promise<std::map<node_id, peer_progress>> result;
        asio::post(io_, [&] { result.set_value(raft_->progress()); });
        return result.get_future().get();
    }

    // Runs `fn` on the node's io thread and returns its result.
    template <typename F>
    auto on_io(F fn) -> decltype(fn()) {
        std::promise<decltype(fn())> result;
        asio::post(io_, [&] { result.set_value(fn()); });
        return result.get_future().get();
    }

    struct write_result
    {
        bool accepted = false;                // false: not the leader
        std::optional<raft_outcome> outcome;  // nullopt: timed out waiting
        node_id leader_hint = 0;
    };

    // A client write through Raft: propose on the io thread, wait (here, on
    // the test thread) for its outcome.
    write_result put(const std::string& key, const std::string& value, std::chrono::milliseconds wait = 3000ms) {
        return submit(command{"PUT", key, value}, wait);
    }

    write_result submit(command cmd, std::chrono::milliseconds wait) {
        auto promised = std::make_shared<std::promise<raft_outcome>>();
        auto future = promised->get_future();
        write_result r;
        on_io([&] {
            r.accepted = raft_->propose(cmd, [promised](const raft_outcome& o) { promised->set_value(o); });
            r.leader_hint = raft_->leader_hint();
            return 0;
        });
        if (r.accepted && future.wait_for(wait) == std::future_status::ready) { r.outcome = future.get(); }
        return r;
    }

    // A leader read: waits for the read barrier, then reads the state machine.
    std::optional<std::string> get(const std::string& key, std::chrono::milliseconds wait = 3000ms) {
        auto promised = std::make_shared<std::promise<std::string>>();
        auto future = promised->get_future();
        const bool accepted = on_io([&] {
            return raft_->read_barrier([this, promised, key](const raft_outcome& o) {
                promised->set_value(o.type == raft_outcome::kind::applied
                                        ? sm_.get(command{"GET", key, std::nullopt}) : "ERR_LEADERSHIP_LOST");
            });
        });
        if (!accepted || future.wait_for(wait) != std::future_status::ready) { return std::nullopt; }
        return future.get();
    }

    // This node's own store (whatever it has applied), no Raft involved.
    std::string stored(const std::string& key) {
        return on_io([&] { return sm_.get(command{"GET", key, std::nullopt}); });
    }
    std::string stored_all() { return on_io([&] { return sm_.list(); }); }
    std::pair<std::uint64_t, std::uint64_t> commit_applied() {
        return on_io([&] { return std::make_pair(raft_->commit_index(), raft_->last_applied()); });
    }

    // The node's last log index, read on its own io thread.
    std::uint64_t last_index() {
        if (!alive()) { return 0; }
        std::promise<std::uint64_t> result;
        asio::post(io_, [&] { result.set_value((raft_log_ ? static_cast<RaftLog&>(*raft_log_) : fake_log_).last().last_index); });
        return result.get_future().get();
    }

private:
    node_id id_;
    History& history_;
    std::string state_path_;
    std::string log_path_;
    StateMachine sm_;  // fed only by Raft's apply
    FakeLog fake_log_;
    std::unique_ptr<PersistentLog> disk_log_;
    std::unique_ptr<PersistentRaftLog> raft_log_;
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
    // With a `state_dir`, node N runs on a real log <state_dir>/nN.log and keeps
    // its term/vote next to it; otherwise on an in-memory FakeLog. `logs` seeds
    // node N's log (terms per entry, see fake_entry); nodes not listed start empty.
    explicit Cluster(std::size_t n, raft_options opts = {}, std::filesystem::path state_dir = {},
                     std::map<node_id, std::vector<std::uint64_t>> logs = {})
        : opts_(std::move(opts)), state_dir_(std::move(state_dir)), logs_(std::move(logs)) {
        for (node_id id = 1; id <= n; ++id) {
            if (!state_dir_.empty() && logs_.count(id)) { write_log_file(log_path(id), logs_.at(id)); }
            nodes_.push_back(std::make_unique<LiveNode>(id, history, 0, state_path(id), log_terms(id), log_path(id)));
        }
        for (const auto& node : nodes_) {
            eps_[node->id()] = tcp::endpoint(asio::ip::address_v4::loopback(), node->listen_port());
        }
        for (const auto& node : nodes_) { node->start(eps_, opts_); }
    }

    ~Cluster() { for (auto& n : nodes_) { n->kill(); } }

    LiveNode& node(node_id id) { return *nodes_.at(id - 1); }

    std::string log_path(node_id id) const {
        if (state_dir_.empty()) { return {}; }
        return (state_dir_ / ("n" + std::to_string(id) + ".log")).string();
    }

    std::string state_path(node_id id) const {
        if (state_dir_.empty()) { return {}; }
        return RaftStateFile::path_for_log(log_path(id));
    }

    // Crash a node, then bring up a fresh one with the same id, port and state file.
    void restart(node_id id) {
        auto& slot = nodes_.at(id - 1);
        const std::uint16_t port = slot->port();
        slot.reset();
        slot = std::make_unique<LiveNode>(id, history, port, state_path(id), log_terms(id), log_path(id));
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

// Identical non-empty logs on disk (three term-0 entries, as client writes
// make today): every probe succeeds the first time, so matchIndex lands on 3,
// nextIndex never moves from 4, and no entries are sent or stored.
static void test_cluster3_identical_logs() {
    std::puts("test_cluster3_identical_logs");
    ::alarm(30);
    TempDir dir;
    const std::vector<std::uint64_t> log = {0, 0, 0};
    Cluster c(3, {}, dir.path, {{1, log}, {2, log}, {3, log}});
    const std::string seeded = read_file(c.log_path(1));
    const auto a = elect_and_hold(c, "3-node identical logs", 1000ms);
    CHECK(c.history.at_most_one_leader_per_term());
    // Nothing to repair: each log keeps its 3 entries untouched and gains
    // only the leader's no-op, identically.
    for (node_id id = 1; id <= 3; ++id) {
        const std::string now = read_file(c.log_path(id));
        CHECK(now.size() > seeded.size() && now.compare(0, seeded.size(), seeded) == 0);
        CHECK(now == read_file(c.log_path(1)));
    }
    if (!a) { return; }
    const auto p = c.node(a->leader).progress();
    CHECK(p.size() == 2);
    for (const auto& [peer, pr] : p) {
        std::printf("  leader %llu: nextIndex[%llu]=%llu matchIndex[%llu]=%llu\n",
                    static_cast<unsigned long long>(a->leader), static_cast<unsigned long long>(peer),
                    static_cast<unsigned long long>(pr.next_index), static_cast<unsigned long long>(peer),
                    static_cast<unsigned long long>(pr.match_index));
        CHECK(pr.next_index == 5 && pr.match_index == 4);
    }
}

// Waits until node `id`'s log has exactly `want` entries; returns the
// distinct last-index values seen along the way (the convergence trace).
static std::vector<std::uint64_t> await_last_index(Cluster& c, node_id id, std::uint64_t want,
                                                   std::chrono::milliseconds deadline) {
    std::vector<std::uint64_t> trace;
    const auto start = steady::now();
    while (ms_since(start) < deadline.count()) {
        const std::uint64_t now = c.node(id).last_index();
        if (trace.empty() || trace.back() != now) { trace.push_back(now); }
        if (now == want) { break; }
        std::this_thread::sleep_for(2ms);
    }
    return trace;
}

static std::string join(const std::vector<std::uint64_t>& v) {
    std::string out;
    for (const auto x : v) { out += (out.empty() ? "" : " -> ") + std::to_string(x); }
    return out;
}

// A 10ms heartbeat: the step-by-one walk back costs a round per index, so
// this keeps a follower ~100 entries behind to about a second.
static raft_options fast_heartbeat() {
    raft_options o;
    o.heartbeat_interval = 10ms;
    return o;
}

// One follower is far behind on disk (10 entries vs 120): it can't win an
// election; the leader walks back to it, ships the rest in bounded batches,
// and its log file ends byte-identical to the leader's. Then everything goes
// quiet, and a kill + restart leaves it converged.
static void test_cluster3_follower_catches_up() {
    std::puts("test_cluster3_follower_catches_up");
    ::alarm(40);
    TempDir dir;
    std::vector<std::uint64_t> full(120, 1);
    for (std::size_t i = 60; i < 120; ++i) { full[i] = 2; }
    // Everyone resumes at term 2, so the elected term is newer than every entry.
    for (node_id id = 1; id <= 3; ++id) {
        RaftStateFile(RaftStateFile::path_for_log((dir.path / ("n" + std::to_string(id) + ".log")).string()), id).save({2, 0});
    }
    Cluster c(3, fast_heartbeat(), dir.path, {{1, full}, {2, full}, {3, std::vector<std::uint64_t>(10, 1)}});
    const auto a = c.await_agreement(3000ms);
    CHECK(a.has_value());
    if (!a) { return; }
    CHECK(a->leader != 3);
    std::printf("  node %llu leads term %llu\n", static_cast<unsigned long long>(a->leader),
                static_cast<unsigned long long>(a->term));

    // 120 seeded entries plus the leader's no-op at 121.
    const auto trace = await_last_index(c, 3, 121, 10000ms);
    std::printf("  node 3 last index: %s\n", join(trace).c_str());
    CHECK(!trace.empty() && trace.back() == 121);

    // Quiet: no more writes to any log, nobody campaigns, the leader holds.
    const long long t0 = c.history.now_ms();
    std::map<node_id, std::string> files;
    for (node_id id = 1; id <= 3; ++id) { files[id] = read_file(c.log_path(id)); }
    std::this_thread::sleep_for(500ms);
    for (node_id id = 1; id <= 3; ++id) { CHECK(read_file(c.log_path(id)) == files[id]); }
    CHECK(files[3] == files[a->leader]);  // byte-identical to the leader
    CHECK(c.history.elections_started_after(t0) == 0);
    const auto p = c.node(a->leader).progress();
    CHECK(p.at(3).match_index == 121 && p.at(3).next_index == 122);

    // Crash and restart the follower: its replayed log is still the leader's,
    // and the leader has nothing to send it.
    c.restart(3);
    const auto b = c.await_agreement(2000ms);
    CHECK(b && b->leader == a->leader && b->term == a->term);
    std::this_thread::sleep_for(300ms);
    CHECK(c.node(3).last_index() == 121);
    CHECK(read_file(c.log_path(3)) == files[a->leader]);
    CHECK(c.history.elections_started_after(t0) == 0);
    CHECK(c.history.at_most_one_leader_per_term());
}

// A follower whose tail is from a term the others never saw: it is truncated
// back to the agreeing prefix and replaced with the leader's entries.
//   nodes 1, 2:  1x5 then 3x5   (10 entries, last term 3)
//   node 3:      1x5 then 2x7   (12 entries, last term 2: it can't win)
static void test_cluster3_divergent_follower() {
    std::puts("test_cluster3_divergent_follower");
    ::alarm(30);
    TempDir dir;
    const std::vector<std::uint64_t> good = {1, 1, 1, 1, 1, 3, 3, 3, 3, 3};
    const std::vector<std::uint64_t> stale = {1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2};
    for (node_id id = 1; id <= 3; ++id) {
        RaftStateFile(RaftStateFile::path_for_log((dir.path / ("n" + std::to_string(id) + ".log")).string()), id).save({3, 0});
    }
    Cluster c(3, fast_heartbeat(), dir.path, {{1, good}, {2, good}, {3, stale}});
    const auto a = c.await_agreement(3000ms);
    CHECK(a.has_value());
    if (!a) { return; }
    CHECK(a->leader != 3 && a->term >= 4);

    // 12 -> 5 (truncated at 6) -> 11 (the leader's 10 plus its no-op) is the
    // expected path, though the 5 can be too brief to sample: the same
    // request truncates and appends.
    const auto trace = await_last_index(c, 3, 11, 5000ms);
    std::printf("  node 3 last index: %s\n", join(trace).c_str());
    std::this_thread::sleep_for(300ms);
    CHECK(c.node(3).last_index() == 11);
    CHECK(read_file(c.log_path(3)) == read_file(c.log_path(a->leader)));
    write_log_file(dir.path / "want.log", good);
    const std::string want = read_file(dir.path / "want.log");
    CHECK(read_file(c.log_path(3)).compare(0, want.size(), want) == 0);  // the leader's 1..10, then its no-op
    CHECK(c.agreed() && c.agreed()->leader == a->leader);
    CHECK(c.history.at_most_one_leader_per_term());
}

// Waits until every live node's store has applied `key` = `want`; returns
// how long that took, or -1.
static long long await_everywhere(Cluster& c, const std::string& key, const std::string& want,
                                  std::chrono::milliseconds deadline) {
    const auto start = steady::now();
    while (ms_since(start) < deadline.count()) {
        bool all = true;
        for (node_id id = 1; id <= 3; ++id) {
            if (c.node(id).alive() && c.node(id).stored(key) != want) { all = false; }
        }
        if (all) { return ms_since(start); }
        std::this_thread::sleep_for(2ms);
    }
    return -1;
}

// Client writes end to end over real sockets: to the leader, they commit on a
// majority, apply everywhere, and read back; to a follower, they're refused
// with the leader's id. Then the leader dies: committed data survives on the
// new leader, new writes work, and the old leader comes back and catches up.
static void test_cluster3_client_writes_and_failover() {
    std::puts("test_cluster3_client_writes_and_failover");
    ::alarm(40);
    TempDir dir;
    Cluster c(3, {}, dir.path);
    const auto a = c.await_agreement(3000ms);
    CHECK(a.has_value());
    if (!a) { return; }
    const node_id leader = a->leader;
    const node_id follower = leader == 1 ? 2 : 1;

    // Non-leader: not accepted, and it names the leader.
    const auto refused = c.node(follower).put("color", "red");
    CHECK(!refused.accepted && refused.leader_hint == leader);

    // Leader: OK comes back only once the entry is on a majority and applied.
    const auto t0 = steady::now();
    const auto ok = c.node(leader).put("color", "blue");
    const long long took = ms_since(t0);
    CHECK(ok.accepted && ok.outcome && ok.outcome->type == raft_outcome::kind::applied && ok.outcome->result == "OK");
    const auto [commit, applied] = c.node(leader).commit_applied();
    std::uint64_t best_match = 0;
    for (const auto& [peer, p] : c.node(leader).progress()) { best_match = std::max(best_match, p.match_index); }
    CHECK(best_match >= commit && applied == commit);  // a follower already had it when OK was sent
    CHECK(c.node(leader).stored("color") == "blue");
    CHECK(c.node(leader).get("color") == std::optional<std::string>("blue"));
    const long long spread = await_everywhere(c, "color", "blue", 2000ms);
    CHECK(spread >= 0);
    std::printf("  PUT color=blue on leader %llu: OK after %lldms (commit %llu, a follower at %llu); "
                "all 3 stores had it %lldms later\n",
                static_cast<unsigned long long>(leader), took, static_cast<unsigned long long>(commit),
                static_cast<unsigned long long>(best_match), spread);

    // More writes, a DEL, and the follower's rejected PUT never applied anywhere.
    const auto t1 = steady::now();
    for (int i = 0; i < 5; ++i) {
        const auto r = c.node(leader).put("k" + std::to_string(i), "v" + std::to_string(i));
        CHECK(r.outcome && r.outcome->result == "OK");
    }
    std::printf("  5 more PUTs, one at a time: %lldms each on average\n", ms_since(t1) / 5);
    const auto del = c.node(leader).submit(command{"DEL", "k0", std::nullopt}, 3000ms);
    CHECK(del.outcome && del.outcome->result == "OK");
    CHECK(await_everywhere(c, "k4", "v4", 2000ms) >= 0 && await_everywhere(c, "k0", "NOT_FOUND", 2000ms) >= 0);
    const std::string all = c.node(leader).stored_all();
    for (node_id id = 1; id <= 3; ++id) { CHECK(c.node(id).stored_all() == all); }
    CHECK(all.find("red") == std::string::npos);
    std::this_thread::sleep_for(100ms);  // let the last commit index reach every log's state file
    for (node_id id = 1; id <= 3; ++id) { CHECK(read_file(c.log_path(id)) == read_file(c.log_path(leader))); }

    // Kill the leader. The survivors elect a new one in a later term, which
    // still has every committed write, and accepts new ones.
    c.node(leader).kill();
    const auto b = c.await_agreement(3000ms);
    CHECK(b && b->leader != leader && b->term > a->term);
    if (!b) { return; }
    CHECK(c.node(b->leader).get("color") == std::optional<std::string>("blue"));
    CHECK(c.node(b->leader).get("k3") == std::optional<std::string>("v3"));
    CHECK(c.node(b->leader).get("k0") == std::optional<std::string>("NOT_FOUND"));
    const auto after = c.node(b->leader).put("after", "failover");
    CHECK(after.outcome && after.outcome->result == "OK");
    std::printf("  killed leader %llu; node %llu leads term %llu and still reads color=blue, k3=v3; "
                "new PUT after=failover -> OK\n",
                static_cast<unsigned long long>(leader), static_cast<unsigned long long>(b->leader),
                static_cast<unsigned long long>(b->term));

    // The old leader restarts on its disk state: it rebuilds its store from
    // its committed prefix and catches up on the rest.
    c.restart(leader);
    CHECK(await_everywhere(c, "after", "failover", 3000ms) >= 0);
    CHECK(c.node(leader).stored("color") == "blue");
    for (node_id id = 1; id <= 3; ++id) { CHECK(c.node(id).stored_all() == c.node(b->leader).stored_all()); }
    CHECK(c.history.at_most_one_leader_per_term());
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
    test_success_advances_by_count();
    test_next_index_floor();
    test_reply_with_higher_term();
    test_follower_stores_entries();
    test_walk_back_and_repair();
    test_batched_catch_up();
    test_batch_byte_cap();
    test_commit_needs_majority();
    test_prior_term_entry_not_committed_alone();
    test_apply_in_order_exactly_once();
    test_follower_learns_commit();
    test_lost_leadership_fails_pending();
    test_read_barrier();
    test_restart_applies_only_committed();
    test_commit_persistence_is_conservative();

    test_cluster3_happy_path();
    test_heartbeats_suppress_elections();
    test_cluster3_failover();
    test_cluster5_with_two_failovers();
    test_minority_cannot_elect();
    test_repeated_elections();
    test_cluster3_persistent_state();
    test_cluster3_follower_restart();
    test_cluster3_identical_logs();
    test_cluster3_follower_catches_up();
    test_cluster3_divergent_follower();
    test_cluster3_client_writes_and_failover();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all raft election tests passed");
    return 0;
}
