#ifndef KV_STORE_STATUS_SERVER_H
#define KV_STORE_STATUS_SERVER_H

// A read-only HTTP/1.1 status listener for dashboards. It knows nothing about
// Raft: it serves whatever `render` returns, and `render` (supplied by the
// server) only reads state that is already in memory.
//
// Runs on the caller's io_context like everything else, so `render` is called
// between other handlers, never during one, and sees a consistent snapshot.
// No threads, no locks, no disk, no blocking calls.
//
// Each connection carries one request: read the request head (up to a blank
// line, at most kMaxStatusRequestBytes), answer, close. A connection that
// doesn't finish within `connection_timeout` is dropped, so a slow or stalled
// client can't hold a socket or stall the loop.
//
//   GET /status  -> 200 application/json, body from `render`, with
//                   Access-Control-Allow-Origin: * (demo dashboards on
//                   another origin; the data is read-only)
//   GET /healthz -> 200 text/plain "ok"
//   other method -> 405 (Allow: GET)
//   other path   -> 404
//   malformed    -> 400

#include <asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

constexpr std::size_t kMaxStatusRequestBytes = 8192;
constexpr std::chrono::milliseconds kDefaultStatusConnectionTimeout{2000};

class StatusServer
{
public:
    using render_fn = std::function<std::string()>;

    StatusServer(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, render_fn render,
                 std::chrono::milliseconds connection_timeout = kDefaultStatusConnectionTimeout);

    asio::ip::tcp::endpoint local_endpoint() const { return acceptor_.local_endpoint(); }

private:
    void accept();

    asio::ip::tcp::acceptor acceptor_;
    // Sessions hold this weakly: one that outlives the server renders nothing.
    std::shared_ptr<const render_fn> render_;
    std::chrono::milliseconds connection_timeout_;
};

#endif //KV_STORE_STATUS_SERVER_H
