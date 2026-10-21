// Durable term/vote tests: the RaftStateFile format, and RaftNode across
// simulated crashes (destroy the node, build a new one on the same file).
//
// Nodes run against a fake PeerClient on this thread, so the test sees the
// exact moment each RequestVote is handed to the transport and can check the
// file on disk at that instant: persistence must come before the send.
//
// Isolation: every test works in its own directory under a fresh mkdtemp
// root, removed at exit; no sockets are opened.
// Standalone (no framework): exits non-zero if any check fails.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include "peer_transport.h"
#include "../include/crc32c.h"
#include "../include/raft_node.h"
#include "../include/raft_state.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static const bool kVerbose = std::getenv("RAFT_TEST_VERBOSE") != nullptr;
static fs::path g_tmp_root;

// A fresh, empty directory for one test; returns the state file path in it.
static std::string state_path(const std::string& test) {
    const fs::path dir = g_tmp_root / test;
    fs::create_directories(dir);
    return (dir / "node.log.raftstate").string();
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void write_file(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static bool same(const raft_hard_state& a, const raft_hard_state& b) {
    return a.term == b.term && a.voted_for == b.voted_for;
}

// What a separate reader (i.e. a restarted process) would find on disk now.
static raft_hard_state on_disk(const std::string& path, node_id owner) {
    return RaftStateFile(path, owner).load();
}

// Returns the raft_state_error message, or nullopt if nothing was thrown.
static std::optional<std::string> load_error(const std::string& path, node_id owner) {
    try { RaftStateFile(path, owner).load(); } catch (const raft_state_error& e) { return std::string(e.what()); }
    return std::nullopt;
}

class FakeClient final : public PeerClient
{
public:
    void send_request_vote(node_id peer, const request_vote_request& req,
                           request_vote_callback cb) override {
        if (on_send_vote) { on_send_vote(req); }
        votes.push_back({peer, req, std::move(cb)});
    }
    void send_append_entries(node_id, const append_entries_request&, append_entries_callback) override {}

    struct vote_call { node_id peer; request_vote_request req; request_vote_callback cb; };
    std::vector<vote_call> votes;
    std::function<void(const request_vote_request&)> on_send_vote;
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
};

// One node "process": the state file object and the node built on it. A
// crash is destroying this and building another on the same path.
struct Incarnation
{
    asio::io_context io;
    FakeClient client;
    FakeLog log;
    RaftStateFile state;
    RaftNode node;

    Incarnation(const std::string& path, std::chrono::milliseconds timeout = 10s)
        : state(path, 1), node(io, 1, {2, 3}, client, log, options(timeout)) {}

    raft_options options(std::chrono::milliseconds timeout) {
        raft_options o;
        o.election_timeout_min = timeout;
        o.election_timeout_max = timeout;
        o.seed = 42;
        o.log = kVerbose;
        o.state = &state;
        return o;
    }
};

// ---- file format ----

static void test_first_run_defaults() {
    std::puts("test_first_run_defaults");
    const std::string path = state_path("first_run");
    CHECK(same(on_disk(path, 1), {0, 0}));
    CHECK(!fs::exists(path));  // loading never creates the file
    Incarnation n(path);
    CHECK(n.node.current_term() == 0 && n.node.status().voted_for == 0);
    CHECK(!fs::exists(path));  // nothing changed, nothing written
}

static void test_save_then_reload() {
    std::puts("test_save_then_reload");
    const std::string path = state_path("save_reload");
    {
        RaftStateFile f(path, 1);
        f.save({7, 3});
    }
    CHECK(same(on_disk(path, 1), {7, 3}));
    CHECK(read_file(path) == RaftStateFile::encode(1, {7, 3}));
    CHECK(fs::file_size(path) == RaftStateFile::kSize);
    {
        RaftStateFile f(path, 1);
        f.save({8, 0});  // overwrite in place of the old state
        f.save({0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull});
    }
    CHECK(same(on_disk(path, 1), {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}));
    CHECK(!fs::exists(path + ".tmp"));  // temp file was renamed, not left behind
}

static void test_layout() {
    std::puts("test_layout");
    const std::string img = RaftStateFile::encode(0x0102, {0x0A0B, 0x03});
    CHECK(img.size() == RaftStateFile::kSize);
    CHECK(img.substr(0, 4) == "KVRS");
    CHECK(img[4] == 1 && img[5] == 0 && img[6] == 0 && img[7] == 0);       // version 1, LE
    CHECK(img[8] == 0x02 && img[9] == 0x01);                               // owner, LE
    CHECK(img[16] == 0x0B && img[17] == 0x0A);                             // term, LE
    CHECK(img[24] == 0x03);                                                // voted_for
    const std::uint32_t crc = crc32c(img.data(), 32);
    CHECK(static_cast<unsigned char>(img[32]) == (crc & 0xFF));
    CHECK(static_cast<unsigned char>(img[35]) == (crc >> 24));
}

static void test_corrupt_refuses_to_start() {
    std::puts("test_corrupt_refuses_to_start");
    const std::string path = state_path("corrupt");
    const std::string good = RaftStateFile::encode(1, {5, 2});

    struct variant { const char* name; std::string bytes; const char* expect; };
    std::string flipped_term = good;     flipped_term[16] ^= 0x01;
    std::string flipped_crc = good;      flipped_crc[33] ^= 0x80;
    std::string bad_magic = good;        bad_magic[0] = 'X';
    // A well-formed file for version 2: the CRC is right, the version isn't.
    std::string v2 = good.substr(0, 32); v2[4] = 2;
    const std::uint32_t v2crc = crc32c(v2.data(), v2.size());
    for (int i = 0; i < 4; ++i) { v2.push_back(static_cast<char>((v2crc >> (8 * i)) & 0xFF)); }

    const std::vector<variant> variants = {
        {"empty", "", "size is 0 bytes"},
        {"truncated", good.substr(0, good.size() - 1), "size is 35 bytes"},
        {"half", good.substr(0, 16), "size is 16 bytes"},
        {"oversized", good + '\0', "size is over 36 bytes"},
        {"zeroed", std::string(good.size(), '\0'), "bad magic"},
        {"flipped term bit", flipped_term, "checksum mismatch"},
        {"flipped crc bit", flipped_crc, "checksum mismatch"},
        {"bad magic", bad_magic, "bad magic"},
        {"future version", v2, "unsupported version 2"},
        {"other node's file", RaftStateFile::encode(2, {5, 2}), "written by node 2, but this is node 1"},
    };
    for (const auto& v : variants) {
        write_file(path, v.bytes);
        const auto err = load_error(path, 1);
        CHECK(err.has_value());
        if (!err) { std::fprintf(stderr, "    %s: loaded without error\n", v.name); continue; }
        const bool ok = err->find(v.expect) != std::string::npos
                     && err->find("refusing to start") != std::string::npos
                     && err->find(path) != std::string::npos;
        CHECK(ok);
        if (!ok) { std::fprintf(stderr, "    %s: unexpected message: %s\n", v.name, err->c_str()); }
        if (kVerbose) { std::printf("  %-18s -> %s\n", v.name, err->c_str()); }
    }

    // The node itself refuses to come up on a corrupt file, and leaves it alone.
    write_file(path, flipped_term);
    bool threw = false;
    try { Incarnation n(path); } catch (const raft_state_error&) { threw = true; }
    CHECK(threw);
    CHECK(read_file(path) == flipped_term);
}

static void test_save_failure_latches() {
    std::puts("test_save_failure_latches");
    const std::string path = (g_tmp_root / "no_such_dir" / "node.log.raftstate").string();
    RaftStateFile f(path, 1);
    bool threw = false;
    try { f.save({1, 1}); } catch (const raft_state_error&) { threw = true; }
    CHECK(threw);
    fs::create_directories(fs::path(path).parent_path());  // even once the cause is gone...
    std::optional<std::string> second;
    try { f.save({1, 1}); } catch (const raft_state_error& e) { second = e.what(); }
    CHECK(second && second->find("restart to recover") != std::string::npos);  // ...it stays disabled
    CHECK(!fs::exists(path));
}

// ---- RaftNode across crashes ----

// The core safety property: a vote survives a crash, so a restarted node
// can't hand a second vote in the same term to a different candidate.
static void test_vote_survives_restart() {
    std::puts("test_vote_survives_restart");
    const std::string path = state_path("vote_restart");
    {
        Incarnation n(path);
        n.node.start();
        const auto r = n.node.handle_request_vote({1, 2, 0, 0});
        CHECK(r.vote_granted && r.term == 1);
        // The grant was on disk by the time the reply was returned.
        CHECK(same(on_disk(path, 1), {1, 2}));
    }  // crash
    {
        Incarnation n(path);
        n.node.start();
        CHECK(n.node.current_term() == 1 && n.node.status().voted_for == 2);
        const auto r = n.node.handle_request_vote({1, 3, 0, 0});
        CHECK(!r.vote_granted && r.term == 1);  // already voted for 2 in term 1
        CHECK(n.node.handle_request_vote({1, 2, 0, 0}).vote_granted);  // retry from 2 is fine
        CHECK(same(on_disk(path, 1), {1, 2}));
    }

    // Control: with nothing persisted, the same sequence double-votes. This is
    // the bug the file exists to prevent, so the test above is meaningful.
    {
        asio::io_context io;
        FakeClient c;
        raft_options o;
        o.election_timeout_min = o.election_timeout_max = 10s;
        o.log = kVerbose;
        const FakeLog log;
        RaftNode before(io, 1, {2, 3}, c, log, o);
        CHECK(before.handle_request_vote({1, 2, 0, 0}).vote_granted);
    }
    {
        asio::io_context io;
        FakeClient c;
        raft_options o;
        o.election_timeout_min = o.election_timeout_max = 10s;
        o.log = kVerbose;
        const FakeLog log;
        RaftNode after(io, 1, {2, 3}, c, log, o);
        CHECK(after.handle_request_vote({1, 3, 0, 0}).vote_granted);  // second vote in term 1
    }
}

// Starting an election persists (term+1, self) before any RequestVote is sent.
static void test_election_persisted_before_request_vote() {
    std::puts("test_election_persisted_before_request_vote");
    const std::string path = state_path("election");
    write_file(path, RaftStateFile::encode(1, {4, 3}));  // resumes at term 4
    {
        Incarnation n(path, 50ms);
        std::vector<raft_hard_state> seen_at_send;
        n.client.on_send_vote = [&](const request_vote_request& req) {
            CHECK(req.term == 5 && req.candidate_id == 1);
            seen_at_send.push_back(on_disk(path, 1));
        };
        n.node.start();
        CHECK(n.node.current_term() == 4);
        n.io.run_one_for(2s);  // the election timer
        CHECK(n.node.role() == raft_role::candidate && n.node.current_term() == 5);
        CHECK(seen_at_send.size() == 2);
        for (const auto& s : seen_at_send) { CHECK(same(s, {5, 1})); }
    }  // crash mid-election
    {
        Incarnation n(path);
        n.node.start();
        CHECK(n.node.current_term() == 5 && n.node.status().voted_for == 1);
        CHECK(n.node.role() == raft_role::follower);  // restarts as follower, vote kept
        CHECK(!n.node.handle_request_vote({5, 2, 0, 0}).vote_granted);  // voted for itself in 5
        CHECK(n.node.handle_request_vote({6, 2, 0, 0}).vote_granted);   // a new term is fair game
        CHECK(same(on_disk(path, 1), {6, 2}));
    }
}

// Adopting a higher term from any RPC persists (N, none) before acting on it.
static void test_higher_term_adoption_persisted() {
    std::puts("test_higher_term_adoption_persisted");
    {
        // AppendEntries from a newer leader, after having voted in an older term.
        const std::string path = state_path("adopt_append");
        {
            Incarnation n(path);
            n.node.start();
            CHECK(n.node.handle_request_vote({1, 2, 0, 0}).vote_granted);
            const auto r = n.node.handle_append_entries({5, 3, 0, 0, {}, 0});
            CHECK(r.success && r.term == 5);
            CHECK(same(on_disk(path, 1), {5, 0}));
        }
        Incarnation n(path);
        CHECK(n.node.current_term() == 5 && n.node.status().voted_for == 0);
        CHECK(n.node.handle_request_vote({5, 2, 0, 0}).vote_granted);  // vote in 5 still free
    }
    {
        // RequestVote at a higher term that is refused (log behind): the term
        // is still adopted and persisted, with no vote.
        const std::string path = state_path("adopt_refused_vote");
        {
            asio::io_context io;
            FakeClient c;
            RaftStateFile state(path, 1);
            raft_options o;
            o.election_timeout_min = o.election_timeout_max = 10s;
            o.log = kVerbose;
            o.state = &state;
            FakeLog log;
            log.terms.assign(4, 2);  // 4 entries, last in term 2
            RaftNode node(io, 1, {2, 3}, c, log, o);
            const auto r = node.handle_request_vote({7, 2, 1, 1});
            CHECK(!r.vote_granted && r.term == 7);
            CHECK(same(on_disk(path, 1), {7, 0}));
        }
        Incarnation n(path);
        CHECK(n.node.current_term() == 7 && n.node.status().voted_for == 0);
    }
    {
        // A higher term in a RequestVote reply, while a candidate.
        const std::string path = state_path("adopt_vote_reply");
        {
            Incarnation n(path, 50ms);
            n.node.start();
            n.io.run_one_for(2s);
            CHECK(n.node.role() == raft_role::candidate && same(on_disk(path, 1), {1, 1}));
            n.client.votes.at(0).cb({rpc_error::none, {9, false}});
            CHECK(n.node.role() == raft_role::follower);
            CHECK(same(on_disk(path, 1), {9, 0}));
        }
        Incarnation n(path);
        CHECK(n.node.current_term() == 9 && n.node.status().voted_for == 0);
    }
}

int main() {
    ::alarm(30);
    std::string tmpl = (fs::temp_directory_path() / "kv_raft_state_test.XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) {
        std::perror("mkdtemp");
        return 2;
    }
    g_tmp_root = tmpl;

    test_first_run_defaults();
    test_save_then_reload();
    test_layout();
    test_corrupt_refuses_to_start();
    test_save_failure_latches();
    test_vote_survives_restart();
    test_election_persisted_before_request_vote();
    test_higher_term_adoption_persisted();

    fs::remove_all(g_tmp_root);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all raft state tests passed");
    return 0;
}
