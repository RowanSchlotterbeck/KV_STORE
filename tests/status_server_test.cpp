// Tests for the read-only HTTP status endpoint.
//
// Part 1 drives StatusServer in-process (its own io_context on one thread)
// with a fake renderer: routing, malformed/partial/oversized requests,
// stalled clients, and teardown with a session still open.
//
// Part 2 runs real kv_server processes (path in argv[1]) as a 3-node cluster
// with --status-port and checks /status against what the cluster does:
// one leader, consistent term/leader_id, commit/apply after client PUTs,
// stability while /status is polled in a tight loop, port validation, and
// that without --status-port nothing extra listens.
//
// Isolation: every node gets a fresh mkdtemp dir (removed at exit) and ports
// the OS just handed out (bind to port 0, then release: kv_server needs its
// peers' ports up front, so this is the one small race). Each child is
// SIGKILLed by an RAII guard, by the SIGALRM handler if the test hangs, and
// carries its own alarm() across exec as a last backstop.
// Standalone (no framework): exits non-zero if any check fails.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "status_server.h"

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

static fs::path g_tmp_root;
static std::string g_server_binary;

static long long ms_since(steady::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count();
}

// ---- blocking client sockets (the test is the client) ----

static int connect_to(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static bool send_all(int fd, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n <= 0) { return false; }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

struct read_result
{
    std::string data;
    bool closed = false;  // peer closed (EOF or reset) before the deadline
    long long elapsed_ms = 0;
};

// Reads until the peer closes or `deadline` passes.
static read_result read_until_closed(int fd, std::chrono::milliseconds deadline) {
    read_result r;
    const auto start = steady::now();
    char buf[4096];
    while (ms_since(start) < deadline.count()) {
        pollfd p{fd, POLLIN, 0};
        const int left = static_cast<int>(deadline.count() - ms_since(start));
        if (::poll(&p, 1, left > 0 ? left : 0) <= 0) { continue; }
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            r.closed = true;
            break;
        }
        r.data.append(buf, static_cast<std::size_t>(n));
    }
    r.elapsed_ms = ms_since(start);
    return r;
}

struct http_reply
{
    int code = 0;  // 0: no parseable reply
    std::map<std::string, std::string> headers;  // names lowercased
    std::string body;
};

static http_reply parse_http(const std::string& raw) {
    http_reply r;
    const std::size_t head_end = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/1.1 ", 0) != 0 || head_end == std::string::npos) { return r; }
    r.code = std::atoi(raw.c_str() + 9);
    std::istringstream head(raw.substr(0, head_end));
    std::string line;
    std::getline(head, line);
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }
        std::string name = line.substr(0, colon);
        for (char& c : name) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        r.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1));
    }
    r.body = raw.substr(head_end + 4);
    return r;
}

static http_reply http_raw(std::uint16_t port, const std::string& request,
                           std::chrono::milliseconds deadline = 3000ms) {
    const int fd = connect_to(port);
    if (fd < 0) { return {}; }
    send_all(fd, request);
    const read_result got = read_until_closed(fd, deadline);
    ::close(fd);
    return parse_http(got.data);
}

static http_reply http_get(std::uint16_t port, const std::string& path) {
    return http_raw(port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
}

// ---- a minimal JSON reader, enough to check the /status schema ----

struct json
{
    enum class kind { null, boolean, number, string, array, object } type = kind::null;
    bool b = false;
    bool is_uint = false;  // number written as plain decimal digits
    std::uint64_t u = 0;
    std::string s;
    std::vector<json> arr;
    std::map<std::string, json> obj;

    const json* get(const std::string& key) const {
        const auto it = obj.find(key);
        return type == kind::object && it != obj.end() ? &it->second : nullptr;
    }
};

class json_parser
{
public:
    explicit json_parser(std::string_view text) : t_(text) {}

    std::optional<json> parse() {
        json v;
        if (!value(v)) { return std::nullopt; }
        ws();
        if (i_ != t_.size()) { return std::nullopt; }
        return v;
    }

private:
    void ws() { while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) { ++i_; } }
    bool eat(char c) { ws(); if (i_ < t_.size() && t_[i_] == c) { ++i_; return true; } return false; }
    bool literal(std::string_view word) {
        if (t_.substr(i_, word.size()) != word) { return false; }
        i_ += word.size();
        return true;
    }
    bool string(std::string& out) {
        if (!eat('"')) { return false; }
        while (i_ < t_.size() && t_[i_] != '"') {
            if (t_[i_] == '\\') { return false; }  // the schema has no escapes
            out.push_back(t_[i_++]);
        }
        return i_ < t_.size() && t_[i_++] == '"';
    }
    bool value(json& v) {
        if (++depth_ > 16) { return false; }
        ws();
        if (i_ >= t_.size()) { return false; }
        const char c = t_[i_];
        bool ok = false;
        if (c == '{') {
            ++i_;
            v.type = json::kind::object;
            if (eat('}')) { ok = true; } else {
                do {
                    std::string key;
                    json member;
                    if (!string(key) || !eat(':') || !value(member)) { return false; }
                    if (!v.obj.emplace(key, std::move(member)).second) { return false; }  // duplicate key
                } while (eat(','));
                ok = eat('}');
            }
        } else if (c == '[') {
            ++i_;
            v.type = json::kind::array;
            if (eat(']')) { ok = true; } else {
                do {
                    json item;
                    if (!value(item)) { return false; }
                    v.arr.push_back(std::move(item));
                } while (eat(','));
                ok = eat(']');
            }
        } else if (c == '"') {
            v.type = json::kind::string;
            ok = string(v.s);
        } else if (literal("true") || literal("false")) {
            v.type = json::kind::boolean;
            v.b = c == 't';
            ok = true;
        } else if (literal("null")) {
            ok = true;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            v.type = json::kind::number;
            v.is_uint = true;
            while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_]))) {
                v.u = v.u * 10 + static_cast<std::uint64_t>(t_[i_++] - '0');
            }
            ok = i_ >= t_.size() || std::string_view(".eE").find(t_[i_]) == std::string_view::npos;
        }
        --depth_;
        return ok;
    }

    std::string_view t_;
    std::size_t i_ = 0;
    int depth_ = 0;
};

static std::optional<json> parse_json(const std::string& text) { return json_parser(text).parse(); }

// ---- part 1: StatusServer in-process ----

// A StatusServer on its own io_context and thread, rendering a fixed body.
class InProcess
{
public:
    explicit InProcess(std::chrono::milliseconds timeout = 2000ms) {
        server_.emplace(io_, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
                        [this] {
                            ++renders;
                            render_on_io_thread = render_on_io_thread && std::this_thread::get_id() == io_thread_;
                            return std::string(R"({"fake":true})");
                        },
                        timeout);
        port = server_->local_endpoint().port();
        thread_ = std::thread([this] { io_.run(); });
        io_thread_ = thread_.get_id();
    }
    ~InProcess() {
        work_.reset();
        asio::post(io_, [this] { server_.reset(); });
        io_.stop();
        thread_.join();
    }
    // Destroys the server on its io thread and waits until that happened.
    void destroy_server() {
        std::atomic<bool> done{false};
        asio::post(io_, [&] { server_.reset(); done = true; });
        while (!done) { std::this_thread::sleep_for(1ms); }
    }

    std::uint16_t port = 0;
    std::atomic<int> renders{0};
    std::atomic<bool> render_on_io_thread{true};

private:
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_{io_.get_executor()};
    std::optional<StatusServer> server_;
    std::thread thread_;
    std::thread::id io_thread_;
};

static void test_routes() {
    std::puts("test_routes");
    InProcess s;

    http_reply status = http_get(s.port, "/status");
    CHECK(status.code == 200);
    CHECK(status.headers["content-type"] == "application/json");
    CHECK(status.headers["access-control-allow-origin"] == "*");
    CHECK(status.headers["connection"] == "close");
    CHECK(status.headers["content-length"] == std::to_string(status.body.size()));
    CHECK(status.body == "{\"fake\":true}\n");

    const http_reply health = http_get(s.port, "/healthz");
    CHECK(health.code == 200);
    CHECK(health.body == "ok\n");
    CHECK(health.headers.count("access-control-allow-origin") == 0);

    CHECK(http_get(s.port, "/").code == 404);
    CHECK(http_get(s.port, "/status/extra").code == 404);
    CHECK(http_get(s.port, "/STATUS").code == 404);

    http_reply post = http_raw(s.port, "POST /status HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    CHECK(post.code == 405);
    CHECK(post.headers["allow"] == "GET");
    CHECK(http_raw(s.port, "DELETE /status HTTP/1.1\r\n\r\n").code == 405);
    CHECK(http_raw(s.port, "HEAD /healthz HTTP/1.1\r\n\r\n").code == 405);

    // Query strings are ignored; HTTP/1.0 and bare-LF clients are served.
    CHECK(http_get(s.port, "/status?x=1").code == 200);
    CHECK(http_raw(s.port, "GET /status HTTP/1.0\r\n\r\n").code == 200);
    CHECK(http_raw(s.port, "GET /healthz HTTP/1.1\n\n").code == 200);

    // Only /status renders, and always on the io_context thread.
    CHECK(s.renders == 3);
    CHECK(s.render_on_io_thread);
}

static void test_malformed() {
    std::puts("test_malformed");
    InProcess s(500ms);
    for (const std::string bad : std::vector<std::string>{"\r\n\r\n", "GET\r\n\r\n", "GET /status\r\n\r\n", "GET  /status HTTP/1.1\r\n\r\n",
                                  "GET status HTTP/1.1\r\n\r\n", "GET /status HTTP/2\r\n\r\n",
                                  "GET /status HTTP/1.1 extra\r\n\r\n", std::string("\0\x01\xff\n\n", 5)}) {
        const http_reply r = http_raw(s.port, bad);
        CHECK(r.code == 400);
    }

    // A head that fills the cap without a blank line: 400, not unbounded
    // buffering. (Exactly the cap, so no unread bytes turn the close into a
    // reset that could race the reply.)
    std::string big = "GET /status HTTP/1.1\r\nX: ";
    big.resize(kMaxStatusRequestBytes, 'a');
    CHECK(http_raw(s.port, big).code == 400);

    // Partial requests, then the client goes away: nothing to answer, no crash.
    for (const std::string partial : std::vector<std::string>{"", "GET /sta", "GET /status HTTP/1.1\r\n"}) {
        const int fd = connect_to(s.port);
        CHECK(fd >= 0);
        if (fd >= 0) {
            send_all(fd, partial);
            ::shutdown(fd, SHUT_WR);
            const read_result r = read_until_closed(fd, 2000ms);
            CHECK(r.closed);
            CHECK(r.data.empty());
            ::close(fd);
        }
    }
    CHECK(s.renders == 0);
    CHECK(http_get(s.port, "/status").code == 200);  // still serving
}

static void test_stalled_client_dropped() {
    std::puts("test_stalled_client_dropped");
    InProcess s(300ms);

    // Two clients stall (one silent, one mid-request-line). Neither holds up
    // a third, and both are cut off at the connection timeout.
    const int silent = connect_to(s.port);
    const int partial = connect_to(s.port);
    CHECK(silent >= 0 && partial >= 0);
    if (partial >= 0) { send_all(partial, "GET /status HT"); }

    const auto before = steady::now();
    CHECK(http_get(s.port, "/status").code == 200);
    CHECK(ms_since(before) < 250);

    for (const int fd : {silent, partial}) {
        if (fd < 0) { continue; }
        const read_result r = read_until_closed(fd, 3000ms);
        CHECK(r.closed);
        CHECK(r.data.empty());
        CHECK(r.elapsed_ms < 1500);
        ::close(fd);
    }
    std::printf("  stalled clients dropped after the 300ms connection timeout\n");
}

static void test_destroyed_with_open_session() {
    std::puts("test_destroyed_with_open_session");
    InProcess s(5000ms);
    const int fd = connect_to(s.port);
    CHECK(fd >= 0);
    if (fd < 0) { return; }
    send_all(fd, "GET /status HTTP/1.1\r\n");
    std::this_thread::sleep_for(50ms);  // the session is now waiting for the blank line
    s.destroy_server();
    send_all(fd, "\r\n");
    // The session outlives the server: it must not render (or touch freed state).
    const read_result r = read_until_closed(fd, 3000ms);
    CHECK(r.closed);
    CHECK(r.data.empty());
    CHECK(s.renders == 0);
    ::close(fd);
}

// ---- part 2: real kv_server processes ----

// Children to kill if the test is cut off by its alarm.
static pid_t g_children[16];
static std::atomic<int> g_child_count{0};

static void on_alarm(int) {
    for (int i = 0; i < g_child_count; ++i) {
        if (g_children[i] > 0) { ::kill(g_children[i], SIGKILL); }
    }
    static const char msg[] = "status_server_test: timed out\n";
    (void)!::write(2, msg, sizeof msg - 1);
    ::_exit(1);
}

// Ports the OS just handed out, distinct within this run.
static std::uint16_t free_port() {
    static std::set<std::uint16_t> used;
    for (;;) {
        asio::io_context io;
        tcp::acceptor a(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const std::uint16_t port = a.local_endpoint().port();
        if (used.insert(port).second) { return port; }
    }
}

static std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

struct NodeSpec
{
    std::uint64_t id = 0;
    std::uint16_t peer_port = 0;
    std::uint16_t client_port = 0;
    std::uint16_t status_port = 0;  // 0: no --status-port
};

class Node
{
public:
    Node(fs::path dir, const fs::path& config, NodeSpec spec, std::vector<std::string> extra = {})
        : dir_(std::move(dir)), spec_(spec) {
        fs::create_directories(dir_);
        std::vector<std::string> args = {g_server_binary, (dir_ / "node.log").string(), "--cluster", config.string(),
                                         "--node-id", std::to_string(spec.id)};
        if (spec.status_port) {
            args.push_back("--status-port");
            args.push_back(std::to_string(spec.status_port));
        }
        args.insert(args.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& a : args) { argv.push_back(a.data()); }
        argv.push_back(nullptr);
        const std::string out = (dir_ / "stdout").string();
        const std::string err = (dir_ / "stderr").string();

        std::fflush(nullptr);
        pid_ = ::fork();
        if (pid_ == 0) {
            const int o = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            const int e = ::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (o < 0 || e < 0 || ::dup2(o, 1) < 0 || ::dup2(e, 2) < 0) { ::_exit(126); }
            ::alarm(100);  // survives exec: a child left behind still dies
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        if (pid_ > 0) { g_children[g_child_count++] = pid_; }
    }
    ~Node() { kill(); }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    void kill() {
        if (pid_ <= 0) { return; }
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    // Exit code once it has exited on its own within `deadline`, else nullopt.
    std::optional<int> wait_exit(std::chrono::milliseconds deadline) {
        const auto start = steady::now();
        while (ms_since(start) < deadline.count()) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    std::string out() const { return slurp(dir_ / "stdout"); }
    std::string err() const { return slurp(dir_ / "stderr"); }
    pid_t pid() const { return pid_; }
    const NodeSpec& spec() const { return spec_; }

    std::optional<json> status() const {
        const http_reply r = http_get(spec_.status_port, "/status");
        if (r.code != 200) { return std::nullopt; }
        return parse_json(r.body);
    }
    std::string status_body() const { return http_get(spec_.status_port, "/status").body; }

    // One request on the client line protocol; "" if no reply.
    std::string request(const std::string& line) const {
        const int fd = connect_to(spec_.client_port);
        if (fd < 0) { return ""; }
        send_all(fd, line + "\n");
        std::string reply;
        const auto start = steady::now();
        char c;
        while (ms_since(start) < 6000) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) { continue; }
            if (::recv(fd, &c, 1, 0) != 1 || c == '\n') { break; }
            reply.push_back(c);
        }
        ::close(fd);
        return reply;
    }

private:
    fs::path dir_;
    NodeSpec spec_;
    pid_t pid_ = -1;
};

static std::uint64_t num(const json& j, const char* key) {
    const json* v = j.get(key);
    return v && v->is_uint ? v->u : ~std::uint64_t{0};
}
static std::string str(const json& j, const char* key) {
    const json* v = j.get(key);
    return v && v->type == json::kind::string ? v->s : "";
}

// Every documented field is present with the documented type.
static bool schema_ok(const json& j) {
    if (j.type != json::kind::object) { return false; }
    for (const char* key : {"node_id", "current_term", "leader_id", "log_last_index", "commit_index",
                            "last_applied", "seq", "timestamp_ms"}) {
        const json* v = j.get(key);
        if (!v || !v->is_uint) {
            std::fprintf(stderr, "  field %s missing or not an unsigned integer\n", key);
            return false;
        }
    }
    const std::string role = str(j, "role");
    if (role != "follower" && role != "candidate" && role != "leader") { return false; }
    const json* peers = j.get("peers");
    if (!peers || peers->type != json::kind::array) { return false; }
    for (const json& p : peers->arr) {
        if (num(p, "id") == ~std::uint64_t{0} || num(p, "nextIndex") == ~std::uint64_t{0}
            || num(p, "matchIndex") == ~std::uint64_t{0} || p.obj.size() != 3) {
            return false;
        }
    }
    return j.obj.size() == 10;
}

// A 3-node cluster in its own directory, every node with a status port.
struct Cluster
{
    explicit Cluster(const std::string& name) {
        const fs::path dir = g_tmp_root / name;
        fs::create_directories(dir);
        std::string conf;
        for (std::uint64_t id = 1; id <= 3; ++id) {
            specs.push_back({id, free_port(), free_port(), free_port()});
            conf += std::to_string(id) + " 127.0.0.1 " + std::to_string(specs.back().peer_port) + " "
                    + std::to_string(specs.back().client_port) + "\n";
        }
        config = dir / "cluster.conf";
        std::ofstream(config) << conf;
        for (const NodeSpec& s : specs) {
            nodes.push_back(std::make_unique<Node>(dir / ("n" + std::to_string(s.id)), config, s));
        }
    }

    // Statuses of all three nodes, or nullopt if any can't be read.
    std::optional<std::vector<json>> statuses() const {
        std::vector<json> out;
        for (const auto& n : nodes) {
            auto s = n->status();
            if (!s) { return std::nullopt; }
            out.push_back(std::move(*s));
        }
        return out;
    }

    // Waits until exactly one node is leader and the others follow it in its
    // term. Returns the leader's index into `nodes`, or -1.
    int wait_for_stable_leader(std::chrono::milliseconds deadline) const {
        const auto start = steady::now();
        while (ms_since(start) < deadline.count()) {
            if (const auto all = statuses()) {
                int leader = -1, leaders = 0;
                for (int i = 0; i < 3; ++i) {
                    if (str((*all)[i], "role") == "leader") { leader = i; ++leaders; }
                }
                if (leaders == 1) {
                    const std::uint64_t term = num((*all)[leader], "current_term");
                    const std::uint64_t id = num((*all)[leader], "node_id");
                    bool agree = true;
                    for (const json& s : *all) {
                        agree = agree && num(s, "current_term") == term && num(s, "leader_id") == id;
                    }
                    if (agree) { return leader; }
                }
            }
            std::this_thread::sleep_for(20ms);
        }
        return -1;
    }

    // Waits until every node reports commit_index == last_applied == log_last_index == index.
    bool wait_all_applied(std::uint64_t index, std::chrono::milliseconds deadline) const {
        const auto start = steady::now();
        while (ms_since(start) < deadline.count()) {
            if (const auto all = statuses()) {
                bool done = true;
                for (const json& s : *all) {
                    done = done && num(s, "commit_index") == index && num(s, "last_applied") == index
                           && num(s, "log_last_index") == index;
                }
                if (done) { return true; }
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    fs::path config;
    std::vector<NodeSpec> specs;
    std::vector<std::unique_ptr<Node>> nodes;
};

static int count_of(const std::string& text, const std::string& needle) {
    int n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) { ++n; }
    return n;
}

static void test_cluster_status() {
    std::puts("test_cluster_status");
    Cluster c("status");
    const int li = c.wait_for_stable_leader(10000ms);
    CHECK(li >= 0);
    if (li < 0) { return; }
    Node& leader = *c.nodes[li];

    for (const auto& n : c.nodes) {
        CHECK(count_of(n->out(), "status endpoint on http://127.0.0.1:" + std::to_string(n->spec().status_port)
                                     + "/status\n") == 1);
    }

    const auto before = c.statuses();
    CHECK(before.has_value());
    if (!before) { return; }
    for (const json& s : *before) { CHECK(schema_ok(s)); }
    const json& lead = (*before)[li];
    const std::uint64_t term = num(lead, "current_term");
    CHECK(term >= 1);
    CHECK(num(lead, "leader_id") == leader.spec().id);
    CHECK(num(lead, "node_id") == leader.spec().id);
    // The leader lists both followers' progress; followers list none.
    const json* peers = lead.get("peers");
    CHECK(peers && peers->arr.size() == 2);
    for (int i = 0; i < 3; ++i) {
        CHECK(num((*before)[i], "node_id") == c.specs[i].id);
        if (i != li) {
            CHECK(str((*before)[i], "role") == "follower");
            CHECK((*before)[i].get("peers")->arr.empty());
        }
    }

    // The no-op is committed everywhere; then three PUTs and a DEL.
    const std::uint64_t base = num(lead, "log_last_index");
    CHECK(base >= 1);
    CHECK(c.wait_all_applied(base, 5000ms));
    const std::uint64_t applied_before = num(*leader.status(), "last_applied");
    CHECK(leader.request("PUT \"a\" \"1\"") == "OK");
    CHECK(num(*leader.status(), "last_applied") > applied_before);
    CHECK(leader.request("PUT \"b\" \"2\"") == "OK");
    CHECK(leader.request("PUT \"c\" \"3\"") == "OK");
    CHECK(leader.request("DEL \"b\"") == "OK");
    CHECK(c.wait_all_applied(base + 4, 5000ms));

    const auto after = c.statuses();
    CHECK(after.has_value());
    if (!after) { return; }
    const json* lp = (*after)[li].get("peers");
    CHECK(lp && lp->arr.size() == 2);
    if (lp) {
        for (const json& p : lp->arr) {
            CHECK(num(p, "matchIndex") == base + 4);
            CHECK(num(p, "nextIndex") == base + 5);
        }
    }
    for (const json& s : *after) {
        CHECK(num(s, "current_term") == term);
        CHECK(num(s, "leader_id") == leader.spec().id);
    }

    // seq counts responses, so two reads of one node always differ.
    const auto s1 = leader.status(), s2 = leader.status();
    CHECK(s1 && s2 && num(*s2, "seq") > num(*s1, "seq"));

    std::printf("  leader   (node %llu): %s", static_cast<unsigned long long>(leader.spec().id),
                leader.status_body().c_str());
    for (int i = 0; i < 3; ++i) {
        if (i != li) {
            std::printf("  follower (node %llu): %s", static_cast<unsigned long long>(c.specs[i].id),
                        c.nodes[i]->status_body().c_str());
        }
    }
}

// Polling /status as fast as possible must not disturb Raft: no new term, no
// new election, same leader, and client writes keep committing meanwhile.
static void test_polling_does_not_perturb_raft() {
    std::puts("test_polling_does_not_perturb_raft");
    Cluster c("poll");
    const int li = c.wait_for_stable_leader(10000ms);
    CHECK(li >= 0);
    if (li < 0) { return; }
    Node& leader = *c.nodes[li];
    const auto start_status = c.statuses();
    CHECK(start_status.has_value());
    if (!start_status) { return; }
    const std::uint64_t term = num((*start_status)[li], "current_term");
    const std::uint64_t base = num((*start_status)[li], "log_last_index");
    int elections_before = 0;
    for (const auto& n : c.nodes) { elections_before += count_of(n->err(), "election timeout:"); }

    // One more client stalls on every node for the whole run.
    std::vector<int> stalled;
    for (const auto& n : c.nodes) { stalled.push_back(connect_to(n->spec().status_port)); }

    // Hammer all three from 3 threads, while this thread writes.
    std::atomic<bool> stop{false};
    std::atomic<long> polls{0}, bad{0};
    std::vector<std::thread> pollers;
    for (int i = 0; i < 3; ++i) {
        pollers.emplace_back([&, i] {
            while (!stop) {
                const http_reply r = http_get(c.specs[i].status_port, "/status");
                const auto j = parse_json(r.body);
                if (r.code != 200 || !j || !schema_ok(*j) || num(*j, "current_term") != term
                    || num(*j, "leader_id") != c.specs[li].id) {
                    ++bad;
                }
                ++polls;
            }
        });
    }
    int writes = 0, write_ok = 0;
    const auto start = steady::now();
    while (ms_since(start) < 2500) {
        ++writes;
        if (leader.request("PUT \"k" + std::to_string(writes) + "\" \"v\"") == "OK") { ++write_ok; }
        std::this_thread::sleep_for(20ms);
    }
    stop = true;
    for (auto& t : pollers) { t.join(); }

    CHECK(bad == 0);
    CHECK(write_ok == writes);
    CHECK(c.wait_all_applied(base + static_cast<std::uint64_t>(writes), 5000ms));
    const auto end_status = c.statuses();
    CHECK(end_status.has_value());
    if (end_status) {
        for (const json& s : *end_status) {
            CHECK(num(s, "current_term") == term);
            CHECK(num(s, "leader_id") == c.specs[li].id);
        }
    }
    int elections_after = 0;
    for (const auto& n : c.nodes) { elections_after += count_of(n->err(), "election timeout:"); }
    CHECK(elections_after == elections_before);

    // The stalled clients were cut off at the 2s timeout, never answered.
    for (const int fd : stalled) {
        CHECK(fd >= 0);
        if (fd < 0) { continue; }
        const read_result r = read_until_closed(fd, 3000ms);
        CHECK(r.closed);
        CHECK(r.data.empty());
        ::close(fd);
    }
    std::printf("  %ld polls (%ld/s) across 3 nodes in 2.5s; %d/%d PUTs OK; term %llu throughout; "
                "elections %d -> %d\n",
                polls.load(), polls.load() * 10 / 25, write_ok, writes, static_cast<unsigned long long>(term),
                elections_before, elections_after);
}

// Without --status-port, a node listens on exactly its client and peer
// ports, as before.
static void test_opt_out() {
    std::puts("test_opt_out");
    const fs::path dir = g_tmp_root / "optout";
    fs::create_directories(dir);
    const NodeSpec spec{1, free_port(), free_port(), 0};
    const fs::path config = dir / "cluster.conf";
    std::ofstream(config) << "1 127.0.0.1 " << spec.peer_port << " " << spec.client_port << "\n";
    Node n(dir / "n1", config, spec);

    const auto start = steady::now();
    while (ms_since(start) < 5000 && n.out().find("KV server listening") == std::string::npos) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(n.out().find("KV server listening") != std::string::npos);
    CHECK(n.out().find("status endpoint") == std::string::npos);

    const std::string lsof = (dir / "lsof.txt").string();
    const std::string cmd = "lsof -nP -a -p " + std::to_string(n.pid()) + " -iTCP -sTCP:LISTEN -Fn > " + lsof
                            + " 2>/dev/null";
    if (std::system(cmd.c_str()) == 0) {
        std::set<std::string> listening;
        std::istringstream lines(slurp(lsof));
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind('n', 0) == 0) { listening.insert(line.substr(line.rfind(':') + 1)); }
        }
        CHECK(listening == (std::set<std::string>{std::to_string(spec.peer_port), std::to_string(spec.client_port)}));
    } else {
        std::puts("  (lsof unavailable: listening-socket check skipped)");
    }
}

static void test_port_validation() {
    std::puts("test_port_validation");
    const fs::path dir = g_tmp_root / "ports";
    fs::create_directories(dir);
    const std::uint16_t p1 = free_port(), c1 = free_port(), p2 = free_port(), c2 = free_port();
    const fs::path config = dir / "cluster.conf";
    std::ofstream(config) << "1 127.0.0.1 " << p1 << " " << c1 << "\n2 127.0.0.1 " << p2 << " " << c2 << "\n";

    struct reject_case
    {
        const char* name;
        std::uint16_t status_port;
        int code;
        std::string message;
    };
    // A port something else already holds.
    asio::io_context io;
    tcp::acceptor taken(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const std::uint16_t busy = taken.local_endpoint().port();

    const std::vector<reject_case> cases = {
        {"own client port", c1, 1, "--status-port " + std::to_string(c1) + " collides with this node's client port"},
        {"own peer port", p1, 1, "collides with node 1's peer port on 127.0.0.1"},
        {"other node's peer port", p2, 1, "collides with node 2's peer port on 127.0.0.1"},
        {"other node's client port", c2, 1, "collides with node 2's client port on 127.0.0.1"},
        {"port in use", busy, 1, "Failed to listen for status requests on 127.0.0.1:" + std::to_string(busy)},
    };
    int i = 0;
    for (const auto& rc : cases) {
        Node n(dir / ("case" + std::to_string(i++)), config, {1, p1, c1, rc.status_port});
        const auto code = n.wait_exit(10000ms);
        CHECK(code && *code == rc.code);
        CHECK(n.err().find(rc.message) != std::string::npos);
        std::printf("  %-24s -> exit %d: %s", rc.name, code ? *code : -1, n.err().c_str());
    }

    // --client-port overriding to the status port is caught too, and
    // --status-port without --cluster is a usage error.
    const fs::path config3 = dir / "three.conf";
    std::ofstream(config3) << "1 127.0.0.1 " << p1 << "\n";
    {
        const std::uint16_t same = free_port();
        Node n(dir / "client_flag", config3, {1, p1, 0, same}, {"--client-port", std::to_string(same)});
        const auto code = n.wait_exit(10000ms);
        CHECK(code && *code == 1);
        CHECK(n.err().find("collides with this node's client port") != std::string::npos);
    }
    {
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            const int null = ::open("/dev/null", O_WRONLY);
            ::dup2(null, 1);
            ::dup2(null, 2);
            ::alarm(20);
            const std::string log = (dir / "plain.log").string();
            ::execl(g_server_binary.c_str(), g_server_binary.c_str(), log.c_str(), "--status-port", "7000", nullptr);
            ::_exit(127);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    }
}

// A client port something else already holds: a clean exit 1 naming the
// port, like the peer and status listeners, not an uncaught-exception abort.
static void test_client_port_in_use() {
    std::puts("test_client_port_in_use");
    const fs::path dir = g_tmp_root / "client_port";
    fs::create_directories(dir);
    // Held on the wildcard address, which is what the client listener binds.
    asio::io_context io;
    tcp::acceptor taken(io, tcp::endpoint(tcp::v4(), 0));
    const std::uint16_t busy = taken.local_endpoint().port();
    const std::uint16_t peer = free_port();
    const fs::path config = dir / "cluster.conf";
    std::ofstream(config) << "1 127.0.0.1 " << peer << " " << busy << "\n";

    Node n(dir / "node", config, {1, peer, busy, 0});
    const auto code = n.wait_exit(10000ms);
    CHECK(code && *code == 1);
    const std::string want = "Failed to listen for clients on port " + std::to_string(busy);
    CHECK(n.err().find(want) != std::string::npos);
    std::printf("  client port in use -> exit %d: %s", code ? *code : -1, n.err().c_str());
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <path to kv_server>\n", argv[0]);
        return 2;
    }
    g_server_binary = argv[1];
    std::signal(SIGALRM, on_alarm);
    std::signal(SIGPIPE, SIG_IGN);
    ::alarm(110);  // backstop below the CTest TIMEOUT

    std::string tmpl = (fs::temp_directory_path() / "kv_status_test.XXXXXX").string();
    const char* tmp = ::mkdtemp(tmpl.data());
    if (!tmp) {
        std::perror("mkdtemp");
        return 1;
    }
    g_tmp_root = tmp;

    test_routes();
    test_malformed();
    test_stalled_client_dropped();
    test_destroyed_with_open_session();
    test_cluster_status();
    test_polling_does_not_perturb_raft();
    test_opt_out();
    test_port_validation();
    test_client_port_in_use();

    std::error_code ec;
    fs::remove_all(g_tmp_root, ec);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all status server tests passed");
    return 0;
}
