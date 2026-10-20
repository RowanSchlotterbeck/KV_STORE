#include "../network/peer_transport.h"

#include <array>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include "../include/rpc_frame.h"

using asio::ip::tcp;

peer_endpoints resolve_cluster(asio::io_context& io, const ClusterConfig& config) {
    peer_endpoints out;
    std::set<tcp::endpoint> seen;
    tcp::resolver resolver(io);
    for (const auto& [id, addr] : config.nodes()) {
        const std::string who = "node " + std::to_string(id) + " (" + addr.host + ":"
                              + std::to_string(addr.port) + ")";
        tcp::endpoint endpoint;
        std::error_code ec;
        const asio::ip::address literal = asio::ip::make_address(addr.host, ec);
        if (!ec) {
            endpoint = tcp::endpoint(literal, addr.port);
        } else {
            const auto results = resolver.resolve(addr.host, std::to_string(addr.port),
                                                  tcp::resolver::numeric_service, ec);
            if (ec || results.empty()) {
                throw config_error("cannot resolve " + who + ": "
                                   + (ec ? ec.message() : std::string("no addresses")));
            }
            endpoint = results.begin()->endpoint();
        }
        if (!seen.insert(endpoint).second) {
            throw config_error(who + " resolves to an endpoint another node already uses");
        }
        out.emplace(id, endpoint);
    }
    return out;
}

const char* to_string(rpc_error err) {
    switch (err) {
        case rpc_error::none:              return "ok";
        case rpc_error::unknown_peer:      return "unknown peer";
        case rpc_error::request_too_large: return "request too large";
        case rpc_error::connect_failed:    return "connect failed";
        case rpc_error::timeout:           return "timed out";
        case rpc_error::io_error:          return "i/o error";
        case rpc_error::bad_response:      return "bad response";
    }
    return "unknown error";
}

// ---- client ----

namespace {

using call_done = std::function<void(rpc_error, rpc_message)>;

// One connect-per-RPC exchange. Owns itself through the shared_ptr captured
// by its pending handlers; it's freed once the socket and timer go idle.
class RpcCall : public std::enable_shared_from_this<RpcCall>
{
public:
    RpcCall(asio::io_context& io, const tcp::endpoint& endpoint, std::string request_frame,
            rpc_type expected, std::chrono::milliseconds timeout, call_done on_done)
        : socket_(io), timer_(io), endpoint_(endpoint), request_(std::move(request_frame)),
          expected_(expected), timeout_(timeout), on_done_(std::move(on_done)) {}

    void start() {
        auto self = shared_from_this();
        timer_.expires_after(timeout_);
        timer_.async_wait([self](std::error_code ec) {
            if (!ec) { self->on_deadline(); }
        });
        socket_.async_connect(endpoint_, [self](std::error_code ec) {
            if (ec) { return self->fail(ec, rpc_error::connect_failed); }
            self->write_request();
        });
    }

private:
    void on_deadline() {
        if (done_) { return; }
        timed_out_ = true;
        std::error_code ignored;
        socket_.close(ignored);  // aborts whichever operation is pending
    }

    void write_request() {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(request_), [self](std::error_code ec, std::size_t) {
            if (ec) { return self->fail(ec, rpc_error::io_error); }
            self->read_header();
        });
    }

    void read_header() {
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(header_), [self](std::error_code ec, std::size_t) {
            if (ec) { return self->fail(ec, rpc_error::io_error); }
            rpc_frame_header header;
            const std::string_view raw(self->header_.data(), self->header_.size());
            if (decode_rpc_header(raw, header) != rpc_decode_status::ok || header.type != self->expected_) {
                return self->finish(rpc_error::bad_response, {});
            }
            self->read_payload(header.payload_size);
        });
    }

    void read_payload(std::uint32_t size) {
        payload_.resize(size);
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(payload_), [self](std::error_code ec, std::size_t) {
            if (ec) { return self->fail(ec, rpc_error::io_error); }
            rpc_message response;
            if (!decode_rpc_payload(self->expected_, self->payload_, response)) {
                return self->finish(rpc_error::bad_response, {});
            }
            self->finish(rpc_error::none, std::move(response));
        });
    }

    // An operation failed; once the deadline has fired, every failure is a timeout.
    void fail(std::error_code, rpc_error err) {
        finish(timed_out_ ? rpc_error::timeout : err, {});
    }

    void finish(rpc_error err, rpc_message response) {
        if (done_) { return; }
        done_ = true;
        timer_.cancel();
        std::error_code ignored;
        socket_.close(ignored);
        on_done_(err, std::move(response));
    }

    tcp::socket socket_;
    asio::steady_timer timer_;
    tcp::endpoint endpoint_;
    std::string request_;
    rpc_type expected_;
    std::chrono::milliseconds timeout_;
    call_done on_done_;
    std::array<char, kRpcHeaderSize> header_{};
    std::string payload_;
    bool timed_out_ = false;
    bool done_ = false;
};

// Turns the untyped completion into the typed rpc_result for one RPC.
template <typename Response, typename Callback>
call_done typed(Callback on_done) {
    return [on_done = std::move(on_done)](rpc_error err, rpc_message msg) {
        rpc_result<Response> result;
        result.error = err;
        if (err == rpc_error::none) { result.response = std::get<Response>(std::move(msg)); }
        on_done(std::move(result));
    };
}

void start_call(asio::io_context& io, const peer_endpoints& peers, node_id peer,
                const rpc_message& request, rpc_type expected,
                std::chrono::milliseconds timeout, call_done on_done) {
    const auto it = peers.find(peer);
    std::string frame;
    rpc_error early = rpc_error::none;
    if (it == peers.end()) {
        early = rpc_error::unknown_peer;
    } else if (!encode_rpc_frame(request, frame)) {
        early = rpc_error::request_too_large;
    }
    if (early != rpc_error::none) {
        // Never complete inline: callers can rely on the callback running later.
        asio::post(io, [on_done = std::move(on_done), early] { on_done(early, {}); });
        return;
    }
    std::make_shared<RpcCall>(io, it->second, std::move(frame), expected, timeout,
                              std::move(on_done))->start();
}

}  // namespace

ConnectPerRpcClient::ConnectPerRpcClient(asio::io_context& io, peer_endpoints peers,
                                         std::chrono::milliseconds timeout)
    : io_(io), peers_(std::move(peers)), timeout_(timeout) {}

void ConnectPerRpcClient::send_request_vote(node_id peer, const request_vote_request& req,
                                            request_vote_callback on_done) {
    start_call(io_, peers_, peer, req, rpc_type::request_vote_response, timeout_,
               typed<request_vote_response>(std::move(on_done)));
}

void ConnectPerRpcClient::send_append_entries(node_id peer, const append_entries_request& req,
                                              append_entries_callback on_done) {
    start_call(io_, peers_, peer, req, rpc_type::append_entries_response, timeout_,
               typed<append_entries_response>(std::move(on_done)));
}

// ---- listener ----

PeerHandlers stub_peer_handlers() {
    PeerHandlers h;
    h.on_request_vote = [](const request_vote_request& req) {
        return request_vote_response{req.term, false};
    };
    h.on_append_entries = [](const append_entries_request& req) {
        return append_entries_response{req.term, false};
    };
    return h;
}

namespace {

// Serves exactly one RPC on an accepted connection, then closes it.
class PeerSession : public std::enable_shared_from_this<PeerSession>
{
public:
    PeerSession(tcp::socket socket, std::shared_ptr<const PeerHandlers> handlers,
                std::chrono::milliseconds timeout)
        : socket_(std::move(socket)), timer_(socket_.get_executor()),
          handlers_(std::move(handlers)), timeout_(timeout) {}

    void start() {
        auto self = shared_from_this();
        timer_.expires_after(timeout_);
        timer_.async_wait([self](std::error_code ec) {
            if (!ec) { self->close(); }
        });
        asio::async_read(socket_, asio::buffer(header_), [self](std::error_code ec, std::size_t) {
            if (ec) { return self->close(); }
            rpc_frame_header header;
            const std::string_view raw(self->header_.data(), self->header_.size());
            if (decode_rpc_header(raw, header) != rpc_decode_status::ok || !is_request(header.type)) {
                return self->reject("malformed or non-request frame header");
            }
            self->read_payload(header);
        });
    }

private:
    void read_payload(rpc_frame_header header) {
        payload_.resize(header.payload_size);
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(payload_),
            [self, type = header.type](std::error_code ec, std::size_t) {
                if (ec) { return self->close(); }
                rpc_message request;
                if (!decode_rpc_payload(type, self->payload_, request)) {
                    return self->reject("unparseable request payload");
                }
                self->respond(request);
            });
    }

    void respond(const rpc_message& request) {
        rpc_message response;
        try {
            if (const auto* rv = std::get_if<request_vote_request>(&request)) {
                if (!handlers_->on_request_vote) { return reject("no RequestVote handler"); }
                response = handlers_->on_request_vote(*rv);
            } else if (const auto* ae = std::get_if<append_entries_request>(&request)) {
                if (!handlers_->on_append_entries) { return reject("no AppendEntries handler"); }
                response = handlers_->on_append_entries(*ae);
            } else {
                return reject("non-request message");
            }
        } catch (const std::exception& e) {
            return reject(std::string("handler threw: ") + e.what());
        }

        if (!encode_rpc_frame(response, response_)) { return reject("response too large"); }
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(response_), [self](std::error_code, std::size_t) {
            self->close();
        });
    }

    void reject(const std::string& why) {
        std::error_code ec;
        const auto remote = socket_.remote_endpoint(ec);
        std::cerr << "peer rpc: dropping connection from "
                  << (ec ? std::string("?") : remote.address().to_string() + ":" + std::to_string(remote.port()))
                  << ": " << why << "\n";
        close();
    }

    void close() {
        timer_.cancel();
        std::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    tcp::socket socket_;
    asio::steady_timer timer_;
    std::shared_ptr<const PeerHandlers> handlers_;
    std::chrono::milliseconds timeout_;
    std::array<char, kRpcHeaderSize> header_{};
    std::string payload_;
    std::string response_;
};

}  // namespace

PeerServer::PeerServer(asio::io_context& io, const tcp::endpoint& endpoint, PeerHandlers handlers,
                       std::chrono::milliseconds connection_timeout)
    : acceptor_(io, endpoint),
      handlers_(std::make_shared<const PeerHandlers>(std::move(handlers))),
      connection_timeout_(connection_timeout) {
    accept();
}

void PeerServer::accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        // Aborted means the acceptor was closed (PeerServer destroyed): touch nothing.
        if (ec == asio::error::operation_aborted) { return; }
        if (!ec) {
            std::make_shared<PeerSession>(std::move(socket), handlers_, connection_timeout_)->start();
        }
        accept();
    });
}
