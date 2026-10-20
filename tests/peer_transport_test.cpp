// Tests for the peer RPC transport: frame codec, cluster config, and real
// socket round trips (in-process nodes on their own io_context + thread, and
// a forked second process).
//
// Isolation: every listener binds 127.0.0.1 port 0 (OS-assigned), config
// files live in a fresh mkdtemp dir removed at exit, forked children are
// SIGKILLed by an RAII guard and carry their own alarm() backstop, and the
// whole binary has an alarm() so a hang fails instead of stalling CTest.
// Standalone (no framework): exits non-zero if any check fails.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "peer_transport.h"
#include "../include/peer_config.h"
#include "../include/rpc_frame.h"

namespace fs = std::filesystem;
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

// ---- comparisons ----

static bool same(const command& a, const command& b) {
    return a.operation == b.operation && a.key == b.key && a.value == b.value;
}

static bool same(const log_entry& a, const log_entry& b) {
    return a.index == b.index && a.term == b.term && same(a.cmd, b.cmd);
}

static bool same(const request_vote_request& a, const request_vote_request& b) {
    return a.term == b.term && a.candidate_id == b.candidate_id
        && a.last_log_index == b.last_log_index && a.last_log_term == b.last_log_term;
}

static bool same(const request_vote_response& a, const request_vote_response& b) {
    return a.term == b.term && a.vote_granted == b.vote_granted;
}

static bool same(const append_entries_request& a, const append_entries_request& b) {
    if (a.term != b.term || a.leader_id != b.leader_id || a.prev_log_index != b.prev_log_index
        || a.prev_log_term != b.prev_log_term || a.leader_commit != b.leader_commit
        || a.entries.size() != b.entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        if (!same(a.entries[i], b.entries[i])) { return false; }
    }
    return true;
}

static bool same(const append_entries_response& a, const append_entries_response& b) {
    return a.term == b.term && a.success == b.success;
}

static bool same(const rpc_message& a, const rpc_message& b) {
    if (a.index() != b.index()) { return false; }
    return std::visit([&b](const auto& x) {
        return same(x, std::get<std::decay_t<decltype(x)>>(b));
    }, a);
}

// ---- fixtures ----

static append_entries_request sample_append() {
    append_entries_request req;
    req.term = 4;
    req.leader_id = 3;
    req.prev_log_index = 10;
    req.prev_log_term = 3;
    req.leader_commit = 10;
    req.entries = {
        log_entry{11, 4, command{"PUT", "user", std::string("alice")}},
        log_entry{12, 4, command{"PUT", "empty", std::string{}}},
        log_entry{13, 4, command{"DEL", "user", std::nullopt}},
        log_entry{14, 4, command{"PUT", "k\xFF", std::string("\x00\xFF\xC3", 3)}},
    };
    return req;
}

static const request_vote_request kVote{7, 2, 41, 6};
static const append_entries_request kHeartbeat{9, 1, 20, 8, {}, 19};

static fs::path g_tmp_root;

static fs::path write_file(const std::string& name, const std::string& text) {
    const fs::path p = g_tmp_root / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

static long long ms_since(steady::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count();
}

static tcp::endpoint loopback(std::uint16_t port) {
    return {asio::ip::address_v4::loopback(), port};
}

// A port nothing listens on: bind an ephemeral port, then release it.
static std::uint16_t dead_port() {
    asio::io_context io;
    tcp::acceptor a(io, loopback(0));
    const auto port = a.local_endpoint().port();
    a.close();
    return port;
}

// A peer node on its own io_context and thread (the node itself stays
// single-threaded). Stopped and joined on destruction, even on failure.
class NodeThread
{
public:
    explicit NodeThread(PeerHandlers handlers,
                        std::chrono::milliseconds connection_timeout = kDefaultPeerConnectionTimeout)
        : server_(io_, loopback(0), std::move(handlers), connection_timeout),
          port_(server_.local_endpoint().port()),
          thread_([this] { io_.run(); }) {}

    ~NodeThread() {
        io_.stop();
        thread_.join();
    }

    std::uint16_t port() const { return port_; }

private:
    asio::io_context io_;
    PeerServer server_;
    std::uint16_t port_;
    std::thread thread_;
};

// Requests a node's handlers saw, guarded because the node runs on another thread.
struct Recorder
{
    std::mutex mu;
    std::vector<request_vote_request> votes;
    std::vector<append_entries_request> appends;

    PeerHandlers handlers() {
        const PeerHandlers stub = stub_peer_handlers();
        PeerHandlers h;
        h.on_request_vote = [this, stub](const request_vote_request& req) {
            std::lock_guard lock(mu);
            votes.push_back(req);
            return stub.on_request_vote(req);
        };
        h.on_append_entries = [this, stub](const append_entries_request& req) {
            std::lock_guard lock(mu);
            appends.push_back(req);
            return stub.on_append_entries(req);
        };
        return h;
    }
};

// Runs one client io_context to completion on this thread.
static void with_client(peer_endpoints peers, std::chrono::milliseconds timeout,
                        const std::function<void(PeerClient&)>& issue) {
    asio::io_context io;
    ConnectPerRpcClient client(io, std::move(peers), timeout);
    issue(client);
    io.run();
}

static rpc_result<request_vote_response> vote_once(peer_endpoints peers, node_id to,
                                                   std::chrono::milliseconds timeout = 2000ms) {
    rpc_result<request_vote_response> out;
    out.error = rpc_error::io_error;
    bool called = false;
    with_client(std::move(peers), timeout, [&](PeerClient& c) {
        c.send_request_vote(to, kVote, [&](rpc_result<request_vote_response> r) {
            CHECK(!called);
            called = true;
            out = r;
        });
    });
    CHECK(called);
    return out;
}

// ---- raw POSIX socket helpers (bounded by poll timeouts, never block forever) ----

static int raw_connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static void send_all(int fd, const std::string& bytes) {
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
        if (n <= 0) { return; }
        off += static_cast<std::size_t>(n);
    }
}

struct raw_outcome
{
    bool closed = false;     // peer closed (EOF or reset) before the deadline
    std::string received;
    long long elapsed_ms = 0;
};

// Reads until the peer closes or `deadline` passes.
static raw_outcome read_until_closed(int fd, std::chrono::milliseconds deadline) {
    raw_outcome out;
    const auto start = steady::now();
    while (ms_since(start) < deadline.count()) {
        pollfd p{fd, POLLIN, 0};
        const int left = static_cast<int>(deadline.count() - ms_since(start));
        if (::poll(&p, 1, left > 0 ? left : 0) <= 0) { continue; }
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { out.closed = true; break; }
        out.received.append(buf, static_cast<std::size_t>(n));
    }
    out.elapsed_ms = ms_since(start);
    return out;
}

static std::string raw_header(std::uint32_t len, std::uint8_t tag) {
    std::string h(kRpcHeaderSize, '\0');
    for (int i = 0; i < 4; ++i) { h[i] = static_cast<char>((len >> (8 * i)) & 0xFF); }
    h[4] = static_cast<char>(tag);
    return h;
}

static std::string frame_of(const rpc_message& msg) {
    std::string f;
    CHECK(encode_rpc_frame(msg, f));
    return f;
}

// ---- frame codec ----

static void test_frame_round_trip() {
    std::puts("test_frame_round_trip");
    const std::vector<rpc_message> msgs = {
        kVote,
        request_vote_request{},
        request_vote_response{7, true},
        request_vote_response{},
        sample_append(),
        kHeartbeat,
        append_entries_request{},
        append_entries_response{9, true},
        append_entries_response{UINT64_MAX, false},
    };
    std::string stream;
    for (const auto& m : msgs) {
        std::string frame;
        CHECK(encode_rpc_frame(m, frame));
        CHECK(frame.size() >= kRpcHeaderSize);
        // Layout: u32 LE payload length, then the type tag.
        const auto* p = reinterpret_cast<const unsigned char*>(frame.data());
        const std::uint32_t len = p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
        CHECK(len == frame.size() - kRpcHeaderSize);
        CHECK(p[4] == static_cast<unsigned char>(type_of(m)));

        std::size_t off = 0;
        rpc_message back;
        CHECK(decode_rpc_frame(frame, off, back) == rpc_decode_status::ok);
        CHECK(off == frame.size());
        CHECK(same(back, m));
        stream += frame;
    }
    // All-default messages encode to an empty payload: a bare header.
    CHECK(frame_of(request_vote_response{}).size() == kRpcHeaderSize);

    // Concatenated frames decode back in order, then end_of_input.
    std::size_t off = 0;
    for (const auto& m : msgs) {
        rpc_message back;
        CHECK(decode_rpc_frame(stream, off, back) == rpc_decode_status::ok);
        CHECK(same(back, m));
    }
    rpc_message unused;
    CHECK(decode_rpc_frame(stream, off, unused) == rpc_decode_status::end_of_input);

    CHECK(type_of(kVote) == rpc_type::request_vote_request);
    CHECK(type_of(append_entries_response{}) == rpc_type::append_entries_response);
    CHECK(is_request(rpc_type::request_vote_request) && is_request(rpc_type::append_entries_request));
    CHECK(!is_request(rpc_type::request_vote_response) && !is_request(rpc_type::append_entries_response));
}

static void test_frame_truncated() {
    std::puts("test_frame_truncated");
    const std::string frame = frame_of(sample_append());
    for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        std::size_t off = 0;
        rpc_message out = request_vote_response{123, true};
        const auto st = decode_rpc_frame(std::string_view(frame).substr(0, cut), off, out);
        CHECK(st == rpc_decode_status::truncated);
        CHECK(off == 0);
        CHECK(same(out, request_vote_response{123, true}));  // untouched
    }
    rpc_frame_header h;
    CHECK(decode_rpc_header(std::string_view(frame).substr(0, kRpcHeaderSize - 1), h)
          == rpc_decode_status::truncated);
}

static void test_frame_malformed() {
    std::puts("test_frame_malformed");
    auto status_of = [](const std::string& buf) {
        std::size_t off = 0;
        rpc_message out;
        const auto st = decode_rpc_frame(buf, off, out);
        CHECK(st == rpc_decode_status::ok || off == 0);
        return st;
    };
    const std::string payload = frame_of(kVote).substr(kRpcHeaderSize);

    for (const int tag : {0, 5, 0x7F, 0xFF}) {
        CHECK(status_of(raw_header(static_cast<std::uint32_t>(payload.size()),
                                   static_cast<std::uint8_t>(tag)) + payload)
              == rpc_decode_status::malformed);
    }
    // Oversized length: rejected from the header alone, nothing allocated or awaited.
    CHECK(status_of(raw_header(kMaxRpcPayloadSize + 1, 1)) == rpc_decode_status::malformed);
    CHECK(status_of(raw_header(0xFFFFFFFFu, 3)) == rpc_decode_status::malformed);
    // Exactly at the cap is merely truncated (the bytes haven't arrived).
    CHECK(status_of(raw_header(kMaxRpcPayloadSize, 1)) == rpc_decode_status::truncated);
    // Payload that isn't valid Protobuf (field 0 / wire type 7).
    CHECK(status_of(raw_header(2, 1) + std::string("\x07\x07", 2)) == rpc_decode_status::malformed);
    CHECK(status_of(raw_header(3, 3) + std::string("\xFF\xFF\xFF", 3)) == rpc_decode_status::malformed);
    // Line-protocol text sent to the peer port: 'GET ' is a ~542 MB length.
    CHECK(status_of("GET \"a\"\n") == rpc_decode_status::malformed);

    std::size_t past_end = 10;
    rpc_message out;
    CHECK(decode_rpc_frame("abc", past_end, out) == rpc_decode_status::malformed);

    // Random garbage never crashes or reads out of bounds (ASan watches).
    std::mt19937 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        std::string buf(rng() % 40, '\0');
        for (auto& c : buf) { c = static_cast<char>(rng()); }
        if (buf.size() >= kRpcHeaderSize && (i & 1)) {
            buf[4] = static_cast<char>(1 + rng() % 4);       // valid tag...
            buf[0] = static_cast<char>(buf.size() - kRpcHeaderSize);
            buf[1] = buf[2] = buf[3] = 0;                     // ...and a length that fits
        }
        std::size_t off = 0;
        rpc_message m;
        (void)decode_rpc_frame(buf, off, m);
        CHECK(off <= buf.size());
    }
}

// ---- config ----

static bool config_rejects(const std::string& text, node_id self, const std::string& needle) {
    try {
        (void)ClusterConfig::parse(text, self, "test.conf");
    } catch (const config_error& e) {
        const bool found = std::string(e.what()).find(needle) != std::string::npos;
        if (!found) { std::fprintf(stderr, "  message was: %s\n", e.what()); }
        return found;
    }
    return false;
}

static void test_config() {
    std::puts("test_config");
    const std::string text =
        "# cluster\n"
        "\n"
        "1 127.0.0.1 6001\n"
        "   2\tlocalhost   6002   # trailing comment\r\n"
        "3 10.0.0.3 65535\n";
    const fs::path path = write_file("cluster.conf", text);
    const ClusterConfig cfg = ClusterConfig::load(path.string(), 2);
    CHECK(cfg.self_id() == 2);
    CHECK(cfg.self_address().host == "localhost" && cfg.self_address().port == 6002);
    CHECK(cfg.nodes().size() == 3);
    CHECK((cfg.peers() == std::vector<node_id>{1, 3}));
    CHECK(cfg.address_of(3).has_value() && cfg.address_of(3)->port == 65535);
    CHECK(cfg.address_of(1)->host == "127.0.0.1");
    CHECK(!cfg.address_of(4).has_value());

    // Single-node cluster: no peers.
    CHECK(ClusterConfig::parse("1 127.0.0.1 7000\n", 1, "x").peers().empty());
    // No trailing newline.
    CHECK(ClusterConfig::parse("1 127.0.0.1 7000", 1, "x").nodes().size() == 1);

    CHECK(config_rejects("1 127.0.0.1\n", 1, "test.conf:1: expected"));
    CHECK(config_rejects("1 127.0.0.1 6001\n2 h 6002 extra\n", 1, "test.conf:2:"));
    CHECK(config_rejects("0 127.0.0.1 6001\n", 1, "reserved"));
    CHECK(config_rejects("x 127.0.0.1 6001\n", 1, "invalid node id"));
    CHECK(config_rejects("-1 127.0.0.1 6001\n", 1, "invalid node id"));
    CHECK(config_rejects("1x 127.0.0.1 6001\n", 1, "invalid node id"));
    CHECK(config_rejects("99999999999999999999 h 6001\n", 1, "invalid node id"));
    CHECK(config_rejects("1 127.0.0.1 0\n", 1, "invalid peer port"));
    CHECK(config_rejects("1 127.0.0.1 65536\n", 1, "invalid peer port"));
    CHECK(config_rejects("1 127.0.0.1 60x\n", 1, "invalid peer port"));
    CHECK(config_rejects("1 127.0.0.1 +6001\n", 1, "invalid peer port"));
    CHECK(config_rejects("1 a 6001\n1 b 6002\n", 1, "test.conf:2: duplicate node id 1"));
    CHECK(config_rejects("1 a 6001\n2 a 6001\n", 1, "reuses address a:6001"));
    CHECK(config_rejects("1 a 6001\n2 b 6002\n", 3, "id 3 is not listed"));
    CHECK(config_rejects("", 1, "not listed"));
    CHECK(config_rejects("# only comments\n\n", 1, "not listed"));

    bool missing_rejected = false;
    try {
        (void)ClusterConfig::load((g_tmp_root / "nope.conf").string(), 1);
    } catch (const config_error& e) {
        missing_rejected = std::string(e.what()).find("cannot open") != std::string::npos;
    }
    CHECK(missing_rejected);

    CHECK(parse_node_id("42") == 42);
    for (const char* bad : {"", "0", "-3", " 4", "4 ", "4a", "0x10"}) {
        bool threw = false;
        try { (void)parse_node_id(bad); } catch (const config_error&) { threw = true; }
        CHECK(threw);
    }
}

static void test_resolve_cluster() {
    std::puts("test_resolve_cluster");
    asio::io_context io;
    const ClusterConfig cfg(1, {{1, {"127.0.0.1", 6001}}, {2, {"localhost", 6002}}, {3, {"::1", 6003}}});
    const peer_endpoints eps = resolve_cluster(io, cfg);
    CHECK(eps.size() == 3);
    CHECK(eps.at(1) == loopback(6001));
    CHECK(eps.at(2).port() == 6002 && eps.at(2).address().is_loopback());
    CHECK(eps.at(3).address().is_v6() && eps.at(3).port() == 6003);

    bool unresolvable = false;
    try {
        (void)resolve_cluster(io, ClusterConfig(1, {{1, {"no-such-host.invalid", 6001}}}));
    } catch (const config_error& e) {
        unresolvable = std::string(e.what()).find("cannot resolve node 1") != std::string::npos;
    }
    CHECK(unresolvable);
}

// ---- real sockets ----

// Kills the child on scope exit, whatever happened in between.
struct ChildGuard
{
    pid_t pid = -1;
    ~ChildGuard() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

// Node 2 runs in a forked process. Its handlers check the request they
// decoded against what the parent sent and report the verdict in the reply,
// so the parent can see the request crossed the process boundary intact.
// Runs first, before any test thread exists, so fork() is safe.
static void test_two_process_round_trip() {
    std::puts("test_two_process_round_trip");
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    std::fflush(nullptr);

    ChildGuard child;
    child.pid = ::fork();
    if (child.pid == 0) {
        ::close(pipefd[0]);
        ::alarm(20);  // backstop if the parent dies without killing us
        asio::io_context io;
        PeerHandlers h;
        h.on_request_vote = [](const request_vote_request& req) {
            return request_vote_response{req.term + 1000, same(req, kVote)};
        };
        h.on_append_entries = [](const append_entries_request& req) {
            const bool ok = same(req, sample_append()) || same(req, kHeartbeat);
            return append_entries_response{req.term + 1000 + req.entries.size(), ok};
        };
        PeerServer server(io, loopback(0), std::move(h));
        const std::uint16_t port = server.local_endpoint().port();
        if (::write(pipefd[1], &port, sizeof port) != sizeof port) { ::_exit(3); }
        io.run();
        ::_exit(0);
    }
    ::close(pipefd[1]);
    CHECK(child.pid > 0);
    if (child.pid <= 0) { ::close(pipefd[0]); return; }

    std::uint16_t port = 0;
    pollfd p{pipefd[0], POLLIN, 0};
    if (::poll(&p, 1, 5000) == 1) {
        CHECK(::read(pipefd[0], &port, sizeof port) == sizeof port);
    }
    ::close(pipefd[0]);
    CHECK(port != 0);
    if (port == 0) { return; }

    // Reach the child through a real config file, as kv_server would.
    const fs::path path = write_file("two_process.conf",
        "1 127.0.0.1 " + std::to_string(dead_port()) + "\n2 127.0.0.1 " + std::to_string(port) + "\n");
    asio::io_context resolve_io;
    const peer_endpoints peers = resolve_cluster(resolve_io, ClusterConfig::load(path.string(), 1));

    std::optional<rpc_result<request_vote_response>> vote;
    std::optional<rpc_result<append_entries_response>> append, heartbeat;
    with_client(peers, 2000ms, [&](PeerClient& c) {
        c.send_request_vote(2, kVote, [&](auto r) { vote = r; });
        c.send_append_entries(2, sample_append(), [&](auto r) { append = r; });
        c.send_append_entries(2, kHeartbeat, [&](auto r) { heartbeat = r; });
    });
    CHECK(vote && vote->ok());
    CHECK(vote && same(vote->response, request_vote_response{kVote.term + 1000, true}));
    CHECK(append && append->ok());
    CHECK(append && same(append->response, append_entries_response{4 + 1000 + 4, true}));
    CHECK(heartbeat && heartbeat->ok());
    CHECK(heartbeat && same(heartbeat->response, append_entries_response{9 + 1000, true}));
}

static void test_in_process_round_trip() {
    std::puts("test_in_process_round_trip");
    Recorder rec;
    NodeThread node(rec.handlers());
    const peer_endpoints peers{{1, loopback(dead_port())}, {2, loopback(node.port())}};

    std::optional<rpc_result<request_vote_response>> vote;
    std::optional<rpc_result<append_entries_response>> append, heartbeat;
    with_client(peers, 2000ms, [&](PeerClient& c) {
        c.send_request_vote(2, kVote, [&](auto r) { vote = r; });
        c.send_append_entries(2, sample_append(), [&](auto r) { append = r; });
        c.send_append_entries(2, kHeartbeat, [&](auto r) { heartbeat = r; });
    });

    // Caller got the stub's typed replies: term echoed, always refused.
    CHECK(vote && vote->ok() && same(vote->response, request_vote_response{7, false}));
    CHECK(append && append->ok() && same(append->response, append_entries_response{4, false}));
    CHECK(heartbeat && heartbeat->ok() && same(heartbeat->response, append_entries_response{9, false}));

    // The handler saw exactly what was sent.
    std::lock_guard lock(rec.mu);
    CHECK(rec.votes.size() == 1 && same(rec.votes[0], kVote));
    CHECK(rec.appends.size() == 2);
    bool saw_entries = false, saw_heartbeat = false;
    for (const auto& a : rec.appends) {
        saw_entries |= same(a, sample_append());
        saw_heartbeat |= same(a, kHeartbeat);
    }
    CHECK(saw_entries && saw_heartbeat);
}

// Many RPCs in flight at once from one single-threaded client.
static void test_concurrent_calls() {
    std::puts("test_concurrent_calls");
    Recorder rec;
    NodeThread node(rec.handlers());
    const peer_endpoints peers{{2, loopback(node.port())}};
    constexpr int kCalls = 50;
    int ok = 0;
    with_client(peers, 3000ms, [&](PeerClient& c) {
        for (int i = 0; i < kCalls; ++i) {
            request_vote_request req = kVote;
            req.term = 100 + static_cast<std::uint64_t>(i);
            c.send_request_vote(2, req, [&ok, term = req.term](rpc_result<request_vote_response> r) {
                if (r.ok() && r.response.term == term && !r.response.vote_granted) { ++ok; }
            });
        }
    });
    CHECK(ok == kCalls);
    std::lock_guard lock(rec.mu);
    CHECK(rec.votes.size() == kCalls);
}

// Garbage, truncated and wrong-typed frames sent to a real PeerServer: each
// connection is closed with no reply, and the node keeps serving.
static void test_server_rejects_bad_frames() {
    std::puts("test_server_rejects_bad_frames");
    Recorder rec;
    NodeThread node(rec.handlers(), 300ms);

    struct bad_case { const char* name; std::string bytes; bool half_close; };
    const std::string vote_payload = frame_of(kVote).substr(kRpcHeaderSize);
    const std::vector<bad_case> cases = {
        {"unknown tag", raw_header(static_cast<std::uint32_t>(vote_payload.size()), 9) + vote_payload, false},
        {"tag 0", raw_header(0, 0), false},
        {"oversized length", raw_header(0xFFFFFFFFu, 1), false},
        {"response sent as request", frame_of(request_vote_response{1, true}), false},
        {"unparseable payload", raw_header(2, 1) + std::string("\x07\x07", 2), false},
        {"line protocol text", "GET \"a\"\n", false},
        {"truncated header, then EOF", frame_of(kVote).substr(0, 3), true},
        {"truncated payload, then EOF", frame_of(sample_append()).substr(0, 20), true},
        {"empty connection, then EOF", "", true},
        // No EOF: the server's own connection deadline (300ms) must close these.
        {"truncated header, then stall", frame_of(kVote).substr(0, 2), false},
        {"truncated payload, then stall", frame_of(sample_append()).substr(0, 20), false},
        {"silent connection", "", false},
    };
    for (const auto& c : cases) {
        const int fd = raw_connect(node.port());
        CHECK(fd >= 0);
        if (fd < 0) { continue; }
        send_all(fd, c.bytes);
        if (c.half_close) { ::shutdown(fd, SHUT_WR); }
        const raw_outcome out = read_until_closed(fd, 3000ms);
        ::close(fd);
        if (!out.closed || !out.received.empty() || out.elapsed_ms > 2000) {
            std::fprintf(stderr, "  case '%s': closed=%d received=%zu elapsed=%lldms\n",
                         c.name, out.closed, out.received.size(), out.elapsed_ms);
        }
        CHECK(out.closed);
        CHECK(out.received.empty());
        CHECK(out.elapsed_ms < 2000);
    }

    // Still alive and serving valid requests.
    const auto r = vote_once({{2, loopback(node.port())}}, 2);
    CHECK(r.ok() && same(r.response, request_vote_response{7, false}));
    std::lock_guard lock(rec.mu);
    CHECK(rec.votes.size() == 1);
    CHECK(rec.appends.empty());
}

static void test_client_down_peer() {
    std::puts("test_client_down_peer");
    const auto start = steady::now();
    const auto r = vote_once({{2, loopback(dead_port())}}, 2, 2000ms);
    const auto elapsed = ms_since(start);
    CHECK(r.error == rpc_error::connect_failed);
    CHECK(elapsed < 1000);  // refused promptly, well inside the 2s deadline

    // Unknown peer id: clean error, delivered later rather than inline.
    asio::io_context io;
    ConnectPerRpcClient client(io, {}, 2000ms);
    std::optional<rpc_error> err;
    client.send_append_entries(42, kHeartbeat, [&](auto res) { err = res.error; });
    CHECK(!err.has_value());
    io.run();
    CHECK(err == rpc_error::unknown_peer);

    CHECK(std::string(to_string(rpc_error::timeout)) == "timed out");
}

// A listener that never accepts: the kernel completes the handshake from the
// backlog, the request is buffered, and no reply ever comes.
static void test_client_silent_peer() {
    std::puts("test_client_silent_peer");
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(lfd, 8) == 0);
    socklen_t len = sizeof addr;
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    const std::uint16_t port = ntohs(addr.sin_port);

    const auto start = steady::now();
    const auto r = vote_once({{2, loopback(port)}}, 2, 300ms);
    const auto elapsed = ms_since(start);
    ::close(lfd);
    CHECK(r.error == rpc_error::timeout);
    CHECK(elapsed >= 250 && elapsed < 2000);
}

// A peer that accepts and replies with scripted bytes, one connection per entry.
static void test_client_bad_replies() {
    std::puts("test_client_bad_replies");
    struct script { const char* name; std::string reply; bool stall; rpc_error expect; };
    const std::string ae_payload = frame_of(append_entries_response{1, true}).substr(kRpcHeaderSize);
    const std::vector<script> scripts = {
        {"garbage header", raw_header(0, 77), false, rpc_error::bad_response},
        {"oversized length", raw_header(0xFFFFFFFFu, 2), false, rpc_error::bad_response},
        {"wrong response type", frame_of(append_entries_response{1, true}), false, rpc_error::bad_response},
        {"request echoed back", frame_of(kVote), false, rpc_error::bad_response},
        {"unparseable payload", raw_header(3, 2) + std::string("\xFF\xFF\xFF", 3), false, rpc_error::bad_response},
        {"close without reply", "", false, rpc_error::io_error},
        {"truncated reply, then close", frame_of(request_vote_response{5, true}).substr(0, 3), false, rpc_error::io_error},
        {"truncated reply, then stall", raw_header(10, 2) + "ab", true, rpc_error::timeout},
    };

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(lfd, 8) == 0);
    socklen_t len = sizeof addr;
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    const std::uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> client_done{false};
    std::thread peer([&] {
        std::vector<int> held;  // stalled connections, held until the client is done
        for (const auto& s : scripts) {
            pollfd p{lfd, POLLIN, 0};
            if (::poll(&p, 1, 5000) != 1) { break; }
            const int fd = ::accept(lfd, nullptr, nullptr);
            if (fd < 0) { break; }
            // Wait for the request to arrive before replying.
            pollfd q{fd, POLLIN, 0};
            if (::poll(&q, 1, 2000) == 1) {
                char buf[4096];
                (void)::recv(fd, buf, sizeof buf, 0);
            }
            send_all(fd, s.reply);
            if (s.stall) { held.push_back(fd); } else { ::close(fd); }
        }
        const auto start = steady::now();
        while (!client_done && ms_since(start) < 5000) { std::this_thread::sleep_for(10ms); }
        for (const int fd : held) { ::close(fd); }
    });

    for (const auto& s : scripts) {
        const auto start = steady::now();
        const auto r = vote_once({{2, loopback(port)}}, 2, 400ms);
        const auto elapsed = ms_since(start);
        if (r.error != s.expect) {
            std::fprintf(stderr, "  script '%s': got '%s', expected '%s'\n",
                         s.name, to_string(r.error), to_string(s.expect));
        }
        CHECK(r.error == s.expect);
        CHECK(elapsed < 2000);
    }
    client_done = true;
    peer.join();
    ::close(lfd);
}

int main() {
    ::alarm(50);  // backstop below the CTest TIMEOUT; an alarm kill is a failure

    std::string tmpl = (fs::temp_directory_path() / "kv_peer_test.XXXXXX").string();
    const char* tmp = ::mkdtemp(tmpl.data());
    if (!tmp) {
        std::perror("mkdtemp");
        return 1;
    }
    g_tmp_root = tmp;

    test_two_process_round_trip();  // must run before any thread is started
    test_frame_round_trip();
    test_frame_truncated();
    test_frame_malformed();
    test_config();
    test_resolve_cluster();
    test_in_process_round_trip();
    test_concurrent_calls();
    test_server_rejects_bad_frames();
    test_client_down_peer();
    test_client_silent_peer();
    test_client_bad_replies();

    std::error_code ec;
    fs::remove_all(g_tmp_root, ec);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all peer transport tests passed");
    return 0;
}
