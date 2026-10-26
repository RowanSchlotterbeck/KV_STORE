// Tests for kv_client (path in argv[1]), run as a child process with its
// stdin scripted, against fake servers this test plays on loopback ports it
// owns: one that answers every line, a "follower" that answers REDIRECT, and
// ones that hang up. Checks that the client connects where it is told,
// follows REDIRECT <id> <host>:<port> to the leader and resends the request
// there, and exits cleanly with a message (never an uncaught-exception
// abort) when it can't connect or the server closes the connection.
//
// The fake servers run in this process on a poll() loop while the child
// runs; the child carries an alarm() across exec so it can't hang the test.
// Standalone (no framework): exits non-zero if any check fails.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

static std::string g_client_binary;

static long long ms_since(steady::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - start).count();
}

// A fake server: a listening socket on 127.0.0.1 and, per accepted
// connection, a reply for each line it receives. `reply` returning nullopt
// closes that connection without answering.
struct FakeServer
{
    using reply_fn = std::function<std::optional<std::string>(const std::string& line)>;

    explicit FakeServer(reply_fn r) : reply(std::move(r)) {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        CHECK(::listen(listen_fd, 8) == 0);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }
    ~FakeServer() {
        for (auto& [fd, buf] : conns) { ::close(fd); }
        ::close(listen_fd);
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    reply_fn reply;
    int listen_fd = -1;
    std::uint16_t port = 0;
    std::map<int, std::string> conns;  // fd -> bytes received, not yet a full line
    std::vector<std::string> lines;    // every line received, in order
    int accepted = 0;

    void add_fds(std::vector<pollfd>& fds) const {
        fds.push_back({listen_fd, POLLIN, 0});
        for (const auto& [fd, buf] : conns) { fds.push_back({fd, POLLIN, 0}); }
    }

    // Handles whatever is ready on this server's sockets.
    void service(const std::vector<pollfd>& fds) {
        for (const pollfd& p : fds) {
            if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) { continue; }
            if (p.fd == listen_fd) {
                const int fd = ::accept(listen_fd, nullptr, nullptr);
                if (fd >= 0) { conns[fd]; ++accepted; }
                continue;
            }
            const auto it = conns.find(p.fd);
            if (it == conns.end()) { continue; }
            char buf[4096];
            const ssize_t n = ::recv(p.fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(p.fd);
                conns.erase(it);
                continue;
            }
            it->second.append(buf, static_cast<std::size_t>(n));
            std::size_t nl;
            while ((nl = it->second.find('\n')) != std::string::npos) {
                const std::string line = it->second.substr(0, nl);
                it->second.erase(0, nl + 1);
                lines.push_back(line);
                const std::optional<std::string> r = reply(line);
                if (!r) {
                    ::close(p.fd);
                    conns.erase(it);
                    break;
                }
                const std::string out = *r + "\n";
                CHECK(::send(p.fd, out.data(), out.size(), 0) == static_cast<ssize_t>(out.size()));
            }
        }
    }
};

struct client_run
{
    bool exited = false;  // exited normally (not killed by a signal)
    int code = -1;        // exit code if exited
    int signal = 0;       // terminating signal if not
    std::string out;
    std::string err;
};

static std::string read_all(int fd) {
    std::string s;
    char buf[4096];
    ssize_t n;
    ::lseek(fd, 0, SEEK_SET);
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) { s.append(buf, static_cast<std::size_t>(n)); }
    return s;
}

// Runs kv_client with `args`, feeding it `input` on stdin, while serving
// `servers` until it exits (or 10s pass, which kills it).
static client_run run_client(const std::vector<std::string>& args, const std::string& input,
                             const std::vector<FakeServer*>& servers) {
    char out_tmpl[] = "/tmp/kv_client_test_out.XXXXXX";
    char err_tmpl[] = "/tmp/kv_client_test_err.XXXXXX";
    const int out_fd = ::mkstemp(out_tmpl);
    const int err_fd = ::mkstemp(err_tmpl);
    ::unlink(out_tmpl);
    ::unlink(err_tmpl);
    int in_pipe[2];
    CHECK(::pipe(in_pipe) == 0);

    std::vector<std::string> argv_store = {g_client_binary};
    argv_store.insert(argv_store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argv_store) { argv.push_back(a.data()); }
    argv.push_back(nullptr);

    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(in_pipe[0], 0);
        ::dup2(out_fd, 1);
        ::dup2(err_fd, 2);
        ::close(in_pipe[1]);
        ::alarm(15);  // survives exec: a stuck client still dies
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(in_pipe[0]);
    // The whole script at once, then EOF: the client reads it line by line.
    CHECK(::write(in_pipe[1], input.data(), input.size()) == static_cast<ssize_t>(input.size()));
    ::close(in_pipe[1]);

    client_run run;
    const auto start = steady::now();
    int status = 0;
    while (true) {
        if (::waitpid(pid, &status, WNOHANG) == pid) { break; }
        if (ms_since(start) > 10000) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        std::vector<pollfd> fds;
        for (FakeServer* s : servers) { s->add_fds(fds); }
        if (fds.empty()) {
            ::usleep(10000);
            continue;
        }
        if (::poll(fds.data(), fds.size(), 10) > 0) {
            for (FakeServer* s : servers) { s->service(fds); }
        }
    }
    run.exited = WIFEXITED(status);
    run.code = run.exited ? WEXITSTATUS(status) : -1;
    run.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    run.out = read_all(out_fd);
    run.err = read_all(err_fd);
    ::close(out_fd);
    ::close(err_fd);
    return run;
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static void show(const char* label, const client_run& r) {
    std::printf("  %s: %s %d\n", label, r.exited ? "exit" : "signal", r.exited ? r.code : r.signal);
    if (!r.err.empty()) { std::printf("    stderr: %s", r.err.c_str()); }
}

// [host] [port] picks the server; replies are printed as before.
static void test_host_and_port() {
    std::puts("test_host_and_port");
    FakeServer s([](const std::string& line) -> std::optional<std::string> {
        return line == "EXIT" ? "EXIT" : "OK";
    });
    const auto r = run_client({"127.0.0.1", std::to_string(s.port)}, "PUT \"k\" \"v\"\nEXIT\n", {&s});
    show("host/port", r);
    CHECK(r.exited && r.code == 0);
    CHECK(s.lines == std::vector<std::string>({"PUT \"k\" \"v\"", "EXIT"}));
    CHECK(contains(r.out, "OK\n") && contains(r.out, "EXIT\n"));
}

// A follower's REDIRECT <id> <host>:<port>: the client reconnects to the
// leader and resends the same request there; later requests go to the
// leader directly.
static void test_follows_redirect() {
    std::puts("test_follows_redirect");
    FakeServer leader([](const std::string& line) -> std::optional<std::string> {
        return line == "EXIT" ? "EXIT" : "OK";
    });
    const std::string redirect = "REDIRECT 2 127.0.0.1:" + std::to_string(leader.port);
    FakeServer follower([&](const std::string&) -> std::optional<std::string> { return redirect; });
    const auto r = run_client({"127.0.0.1", std::to_string(follower.port)},
                              "PUT \"color\" \"blue\"\nGET \"color\"\nEXIT\n", {&follower, &leader});
    show("redirect", r);
    CHECK(r.exited && r.code == 0);
    CHECK(follower.lines == std::vector<std::string>({"PUT \"color\" \"blue\""}));
    CHECK(leader.lines == std::vector<std::string>({"PUT \"color\" \"blue\"", "GET \"color\"", "EXIT"}));
    CHECK(contains(r.out, "OK\n"));
    CHECK(!contains(r.out, "\nREDIRECT"));  // the redirect was followed, not shown as the answer
}

// REDIRECT without an address (the config lists no client port for the
// leader) can't be followed: it is shown as the reply. And two nodes that
// keep redirecting to each other don't loop forever.
static void test_unfollowable_redirects() {
    std::puts("test_unfollowable_redirects");
    {
        FakeServer s([](const std::string& line) -> std::optional<std::string> {
            return line == "EXIT" ? "EXIT" : "REDIRECT 3";
        });
        const auto r = run_client({"127.0.0.1", std::to_string(s.port)}, "GET \"k\"\nEXIT\n", {&s});
        show("no address", r);
        CHECK(r.exited && r.code == 0);
        CHECK(contains(r.out, "REDIRECT 3\n") && s.accepted == 1);
    }
    {
        FakeServer a([](const std::string&) -> std::optional<std::string> { return std::nullopt; });
        FakeServer b([](const std::string&) -> std::optional<std::string> { return std::nullopt; });
        const std::string to_a = "REDIRECT 1 127.0.0.1:" + std::to_string(a.port);
        const std::string to_b = "REDIRECT 2 127.0.0.1:" + std::to_string(b.port);
        a.reply = [&](const std::string& line) -> std::optional<std::string> {
            return line == "EXIT" ? std::optional<std::string>("EXIT") : to_b;
        };
        b.reply = [&](const std::string& line) -> std::optional<std::string> {
            return line == "EXIT" ? std::optional<std::string>("EXIT") : to_a;
        };
        const auto r = run_client({"127.0.0.1", std::to_string(a.port)}, "GET \"k\"\nEXIT\n", {&a, &b});
        show("redirect loop", r);
        CHECK(r.exited && r.code == 0);
        CHECK(a.lines.size() + b.lines.size() <= 6);  // a bounded number of hops, then shown as is
        CHECK(contains(r.out, "REDIRECT "));
    }
}

// The server hangs up (e.g. after ERR_LINE_TOO_LONG, or it exits): a clear
// message and exit 1, not an uncaught exception.
static void test_disconnect() {
    std::puts("test_disconnect");
    {
        FakeServer s([](const std::string&) -> std::optional<std::string> { return std::nullopt; });
        const auto r = run_client({"127.0.0.1", std::to_string(s.port)}, "GET \"k\"\nGET \"k\"\n", {&s});
        show("closed mid-request", r);
        CHECK(r.exited && r.code == 1);
        CHECK(contains(r.err, "kv_client: connection to 127.0.0.1:" + std::to_string(s.port)));
        CHECK(!contains(r.err, "terminating"));
    }
    {
        // Answers once, then hangs up after its reply.
        int seen = 0;
        FakeServer s([&](const std::string&) -> std::optional<std::string> {
            if (seen++ == 0) { return "ERR_LINE_TOO_LONG"; }
            return std::nullopt;
        });
        const auto r = run_client({"127.0.0.1", std::to_string(s.port)}, "PUT \"a\" \"b\"\nGET \"a\"\n", {&s});
        show("closed after a reply", r);
        CHECK(r.exited && r.code == 1);
        CHECK(contains(r.out, "ERR_LINE_TOO_LONG\n"));
        CHECK(contains(r.err, "kv_client: connection to 127.0.0.1:"));
    }
    {
        // Nothing listening there at all.
        std::uint16_t dead_port;
        {
            FakeServer gone([](const std::string&) -> std::optional<std::string> { return std::nullopt; });
            dead_port = gone.port;
        }
        const auto r = run_client({"127.0.0.1", std::to_string(dead_port)}, "GET \"k\"\n", {});
        show("connection refused", r);
        CHECK(r.exited && r.code == 1);
        CHECK(contains(r.err, "kv_client: connection to 127.0.0.1:" + std::to_string(dead_port)));
    }
}

// More than [host] [port] is a usage error.
static void test_usage() {
    std::puts("test_usage");
    const auto r = run_client({"127.0.0.1", "1", "extra"}, "", {});
    CHECK(r.exited && r.code == 2 && contains(r.err, "usage:"));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <path to kv_client>\n", argv[0]);
        return 2;
    }
    g_client_binary = argv[1];
    std::signal(SIGPIPE, SIG_IGN);
    ::alarm(110);  // backstop below the CTest TIMEOUT

    test_host_and_port();
    test_follows_redirect();
    test_unfollowable_redirects();
    test_disconnect();
    test_usage();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all client tests passed");
    return 0;
}
