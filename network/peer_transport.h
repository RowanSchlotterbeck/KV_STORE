#ifndef KV_STORE_PEER_TRANSPORT_H
#define KV_STORE_PEER_TRANSPORT_H

// Node-to-node RPC transport: a listener that answers framed Raft RPCs and a
// client that sends them. Both run on the caller's io_context; neither starts
// threads. Everything is asynchronous because the same io_context also serves
// the client line protocol, so a blocking call would stall it.
//
// Plumbing only: no Raft logic lives here. Frame format: rpc_frame.h.

#include <asio.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include "../include/peer_config.h"
#include "../include/raft_messages.h"

using peer_endpoints = std::map<node_id, asio::ip::tcp::endpoint>;

// Resolves every node's host:port once, synchronously. Call at startup,
// before io_context::run(), so no lookup ever blocks the event loop (and
// asio's async resolver, which spawns a thread, is never used). Throws
// config_error if a host doesn't resolve or two nodes resolve to one endpoint.
peer_endpoints resolve_cluster(asio::io_context& io, const ClusterConfig& config);

// ---- client ----

enum class rpc_error
{
    none,
    unknown_peer,       // no address for that node id
    request_too_large,  // request frame would exceed kMaxRpcPayloadSize
    connect_failed,     // connection refused/unreachable
    timeout,            // deadline hit during connect, write, or read
    io_error,           // write/read failed, or the peer closed before replying
    bad_response,       // reply frame malformed, or not the expected response type
};

const char* to_string(rpc_error err);

template <typename Response>
struct rpc_result
{
    rpc_error error = rpc_error::none;
    Response response{};  // meaningful only when ok()

    bool ok() const { return error == rpc_error::none; }
};

// The send-RPC boundary Raft code will depend on. The completion callback
// runs exactly once, on the io_context thread, never inline from the send call.
class PeerClient
{
public:
    using request_vote_callback = std::function<void(rpc_result<request_vote_response>)>;
    using append_entries_callback = std::function<void(rpc_result<append_entries_response>)>;

    virtual ~PeerClient() = default;
    virtual void send_request_vote(node_id peer, const request_vote_request& req,
                                   request_vote_callback on_done) = 0;
    virtual void send_append_entries(node_id peer, const append_entries_request& req,
                                     append_entries_callback on_done) = 0;
};

constexpr std::chrono::milliseconds kDefaultRpcTimeout{1000};

// Opens a fresh TCP connection per RPC: connect, write one request frame,
// read one response frame, close. A single deadline covers the whole
// exchange. In-flight calls hold no reference to the client, so destroying
// it doesn't cancel them; they still complete or time out.
class ConnectPerRpcClient final : public PeerClient
{
public:
    ConnectPerRpcClient(asio::io_context& io, peer_endpoints peers,
                        std::chrono::milliseconds timeout = kDefaultRpcTimeout);

    void send_request_vote(node_id peer, const request_vote_request& req,
                           request_vote_callback on_done) override;
    void send_append_entries(node_id peer, const append_entries_request& req,
                             append_entries_callback on_done) override;

private:
    asio::io_context& io_;
    peer_endpoints peers_;
    std::chrono::milliseconds timeout_;
};

// ---- listener ----

// Request handlers, called on the io_context thread. Raft logic will plug in
// here later; for now main() installs stub_peer_handlers().
struct PeerHandlers
{
    std::function<request_vote_response(const request_vote_request&)> on_request_vote;
    std::function<append_entries_response(const append_entries_request&)> on_append_entries;
};

// Stubs: well-formed replies with no Raft logic. They echo the request's term
// and always refuse (vote_granted = false, success = false).
PeerHandlers stub_peer_handlers();

constexpr std::chrono::milliseconds kDefaultPeerConnectionTimeout{5000};

// Accepts connections on `endpoint`. Each connection carries exactly one RPC:
// read one request frame, dispatch by type tag, write the response frame,
// close. A malformed, truncated or response-typed frame closes the connection
// with no reply. A connection that doesn't finish within `connection_timeout`
// is closed, so a stalled peer can't pin a socket forever.
class PeerServer
{
public:
    PeerServer(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, PeerHandlers handlers,
               std::chrono::milliseconds connection_timeout = kDefaultPeerConnectionTimeout);

    asio::ip::tcp::endpoint local_endpoint() const { return acceptor_.local_endpoint(); }

private:
    void accept();

    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<const PeerHandlers> handlers_;
    std::chrono::milliseconds connection_timeout_;
};

#endif //KV_STORE_PEER_TRANSPORT_H
