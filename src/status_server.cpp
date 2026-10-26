#include "../network/status_server.h"

#include <string_view>
#include <utility>

using asio::ip::tcp;

namespace {

// Matches the blank line that ends an HTTP request head ("\n\n" or "\n\r\n").
struct head_end
{
    template <typename Iterator>
    std::pair<Iterator, bool> operator()(Iterator begin, Iterator end) const {
        for (Iterator i = begin; i != end; ++i) {
            if (*i != '\n') { continue; }
            Iterator j = std::next(i);
            if (j == end) { return {i, false}; }
            if (*j == '\n') { return {std::next(j), true}; }
            if (*j == '\r') {
                Iterator k = std::next(j);
                if (k == end) { return {i, false}; }
                if (*k == '\n') { return {std::next(k), true}; }
            }
        }
        return {end, false};
    }
};

}  // namespace

template <> struct asio::is_match_condition<head_end> : std::true_type {};

namespace {

struct http_response
{
    int code;
    const char* reason;
    const char* content_type;
    std::string body;
    std::string extra_headers;  // each ending in \r\n
};

std::string encode(const http_response& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.code) + " " + r.reason + "\r\n";
    out += "Content-Type: " + std::string(r.content_type) + "\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "Connection: close\r\n";
    out += r.extra_headers;
    out += "\r\n";
    out += r.body;
    return out;
}

http_response plain(int code, const char* reason, std::string body, std::string extra = {}) {
    return {code, reason, "text/plain", std::move(body) + "\n", std::move(extra)};
}

// Answers one request head. Only the request line matters; headers are ignored.
http_response answer(std::string_view head, const StatusServer::render_fn& render) {
    std::string_view line = head.substr(0, head.find('\n'));
    if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }

    // METHOD SP request-target SP HTTP-version, single spaces.
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos) {
        return plain(400, "Bad Request", "bad request");
    }
    const std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (method.empty() || target.empty() || target.front() != '/' || version.rfind("HTTP/1.", 0) != 0) {
        return plain(400, "Bad Request", "bad request");
    }
    target = target.substr(0, target.find('?'));

    if (method != "GET") { return plain(405, "Method Not Allowed", "method not allowed", "Allow: GET\r\n"); }
    if (target == "/healthz") { return plain(200, "OK", "ok"); }
    if (target == "/status") {
        return {200, "OK", "application/json", render() + "\n", "Access-Control-Allow-Origin: *\r\n"};
    }
    return plain(404, "Not Found", "not found");
}

class StatusSession : public std::enable_shared_from_this<StatusSession>
{
public:
    StatusSession(tcp::socket socket, std::weak_ptr<const StatusServer::render_fn> render,
                  std::chrono::milliseconds timeout)
        : socket_(std::move(socket)), timer_(socket_.get_executor()), render_(std::move(render)),
          timeout_(timeout) {}

    void start() {
        auto self = shared_from_this();
        timer_.expires_after(timeout_);
        timer_.async_wait([self](std::error_code ec) {
            if (!ec) { self->close(); }
        });
        asio::async_read_until(socket_, asio::dynamic_buffer(request_, kMaxStatusRequestBytes), head_end{},
            [self](std::error_code ec, std::size_t n) {
                if (ec == asio::error::not_found) {  // head larger than the cap
                    return self->send(plain(400, "Bad Request", "request too large"));
                }
                if (ec) { return self->close(); }
                const auto render = self->render_.lock();
                if (!render) { return self->close(); }  // server gone
                self->send(answer(std::string_view(self->request_).substr(0, n), *render));
            });
    }

private:
    void send(const http_response& response) {
        response_ = encode(response);
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(response_), [self](std::error_code, std::size_t) {
            self->close();
        });
    }

    void close() {
        timer_.cancel();
        std::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    tcp::socket socket_;
    asio::steady_timer timer_;
    std::weak_ptr<const StatusServer::render_fn> render_;
    std::chrono::milliseconds timeout_;
    std::string request_;
    std::string response_;
};

}  // namespace

StatusServer::StatusServer(asio::io_context& io, const tcp::endpoint& endpoint, render_fn render,
                           std::chrono::milliseconds connection_timeout)
    : acceptor_(io, endpoint),
      render_(std::make_shared<const render_fn>(std::move(render))),
      connection_timeout_(connection_timeout) {
    accept();
}

void StatusServer::accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        // Aborted means the acceptor was closed (StatusServer destroyed): touch nothing.
        if (ec == asio::error::operation_aborted) { return; }
        if (!ec) {
            std::make_shared<StatusSession>(std::move(socket), render_, connection_timeout_)->start();
        }
        accept();
    });
}
