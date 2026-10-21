//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include <server.h>
#include <optional>
#include "../include/wal.h"
#include "peer_transport.h"
#include "../include/raft_node.h"
#include "status_server.h"


using asio::ip::tcp;

// A request waiting on Raft gets this long before ERR_TIMEOUT (outcome unknown).
constexpr std::chrono::seconds kClientRequestTimeout{5};

// Cluster mode: what a session needs to route requests through Raft.
struct ClusterFrontEnd
{
    RaftNode& raft;
    const ClusterConfig& config;

    // Where to send a client this node can't serve.
    std::string redirect(const std::string& op) const {
        const node_id leader = raft.leader_hint();
        std::string reply;
        if (leader == 0) {
            reply = "ERR_NO_LEADER";
        } else {
            reply = "REDIRECT " + std::to_string(leader);
            const auto addr = config.address_of(leader);
            if (addr && addr->client_port) { reply += " " + addr->host + ":" + std::to_string(*addr->client_port); }
        }
        std::cerr << "[client] " << op << " on non-leader node " << config.self_id() << ": " << reply << "\n";
        return reply;
    }
};

// The /status body (schema in README.md, "Status endpoint"). Reads only
// in-memory fields; called on the io_context thread, between Raft handlers,
// so the fields come from one consistent point in time. peers lists the
// leader's per-follower progress and is [] on a non-leader. seq counts
// responses from this process; timestamp_ms is wall-clock Unix time.
static std::string status_json(const RaftNode& raft, const RaftLog& log, std::uint64_t seq) {
    const raft_status st = raft.status();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    std::string out = "{";
    out += "\"node_id\":" + std::to_string(st.id);
    out += ",\"role\":\"" + std::string(to_string(st.role)) + "\"";
    out += ",\"current_term\":" + std::to_string(st.term);
    out += ",\"leader_id\":" + std::to_string(st.leader_id);
    out += ",\"log_last_index\":" + std::to_string(log.last().last_index);
    out += ",\"commit_index\":" + std::to_string(raft.commit_index());
    out += ",\"last_applied\":" + std::to_string(raft.last_applied());
    out += ",\"peers\":[";
    bool first = true;
    for (const auto& [id, p] : raft.progress()) {
        if (!first) { out += ","; }
        first = false;
        out += "{\"id\":" + std::to_string(id) + ",\"nextIndex\":" + std::to_string(p.next_index)
               + ",\"matchIndex\":" + std::to_string(p.match_index) + "}";
    }
    out += "]";
    out += ",\"seq\":" + std::to_string(seq);
    out += ",\"timestamp_ms\":" + std::to_string(now.count());
    out += "}";
    return out;
}

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const ClusterFrontEnd* cluster)
        : socket_(std::move(socket)), cluster_(cluster), timer_(socket_.get_executor()) {}

    void start() {
        read();
    }

private:
    void read() {
        auto self = shared_from_this();
        asio::async_read_until(socket_, buffer_, '\n',
            [this, self](std::error_code ec, std::size_t) {
                if (!ec) {
                    std::istream is(&buffer_);
                    std::string line;
                    std::getline(is, line);
                    handle(line);
                }
            });
    }

    void respond(std::string response) {
        response += "\n";
        write(response);
        std::cout << response;
    }

    void handle(const std::string& line) {
        if (!cluster_) {  // plain mode: answered on the spot, as always
            respond(process_line(line));
            return;
        }
        const client_request req = parse_request(line);
        if (req.type == client_request::kind::reply) {
            respond(req.reply);
            return;
        }

        // Reads and writes go to the leader and finish asynchronously: the
        // reply is sent when Raft completes them (or on timeout), and the
        // next line isn't read until then.
        auto self = shared_from_this();
        waiting_ = true;
        timer_.expires_after(kClientRequestTimeout);
        timer_.async_wait([this, self](std::error_code ec) {
            if (!ec) { finish("ERR_TIMEOUT"); }
        });
        bool accepted = false;
        if (req.type == client_request::kind::write) {
            accepted = cluster_->raft.propose(req.cmd, [this, self](const raft_outcome& o) {
                finish(o.type == raft_outcome::kind::applied ? o.result : "ERR_LEADERSHIP_LOST");
            });
        } else {
            accepted = cluster_->raft.read_barrier([this, self, cmd = req.cmd](const raft_outcome& o) {
                finish(o.type == raft_outcome::kind::applied ? execute_read(cmd) : "ERR_LEADERSHIP_LOST");
            });
        }
        if (!accepted) { finish(cluster_->redirect(req.cmd.operation)); }
    }

    // First completion wins: Raft's outcome or the timeout.
    void finish(const std::string& response) {
        if (!waiting_) { return; }
        waiting_ = false;
        timer_.cancel();
        respond(response);
    }

    void write(const std::string& msg) {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(msg),
            [this, self](std::error_code ec, std::size_t) {
                if (!ec) {
                    read();
                }
            });
    }

    tcp::socket socket_;
    asio::streambuf buffer_;
    const ClusterFrontEnd* cluster_;  // null in plain mode
    asio::steady_timer timer_;
    bool waiting_ = false;
};

class Server {
public:
    Server(asio::io_context& io, std::uint16_t port, const ClusterFrontEnd* cluster)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)), cluster_(cluster) {
        accept();
    }

private:
    void accept() {
        acceptor_.async_accept(
            [this](std::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), cluster_)->start();
                }
                accept();
            });
    }

    tcp::acceptor acceptor_;
    const ClusterFrontEnd* cluster_;
};

// A decimal TCP port, 1-65535.
static std::optional<std::uint16_t> parse_port(const std::string& value) {
    unsigned long port = 0;
    try { port = std::stoul(value); } catch (const std::exception&) {}
    if (port == 0 || port > 65535 || value.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(port);
}

static int usage(const char* prog) {
    std::cerr << "usage: " << prog
              << " [log_path] [--cluster <config_file> --node-id <id>] [--client-port <port>]"
                 " [--status-port <port>]\n";
    return 2;
}

int main(int argc, char** argv) {
    std::string log_path = PersistentLog::kDefaultPath;
    std::optional<std::string> cluster_path;
    std::optional<std::string> node_id_arg;
    std::uint16_t client_port = 5555;
    bool client_port_given = false;
    std::optional<std::uint16_t> status_port;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cluster" && i + 1 < argc) {
            cluster_path = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id_arg = argv[++i];
        } else if (arg == "--client-port" && i + 1 < argc) {
            const auto port = parse_port(argv[++i]);
            if (!port) { return usage(argv[0]); }
            client_port = *port;
            client_port_given = true;
        } else if (arg == "--status-port" && i + 1 < argc) {
            status_port = parse_port(argv[++i]);
            if (!status_port) { return usage(argv[0]); }
        } else if (i == 1 && arg.rfind("--", 0) != 0) {
            log_path = arg;
        } else {
            return usage(argv[0]);
        }
    }
    if (cluster_path.has_value() != node_id_arg.has_value()) { return usage(argv[0]); }
    // The status endpoint reports Raft state, so it exists only in cluster mode.
    if (status_port && !cluster_path) {
        std::cerr << "--status-port requires --cluster (the status endpoint reports Raft state)\n";
        return 2;
    }

    asio::io_context io;

    // Cluster mode is opt-in; without --cluster the server runs exactly as before.
    std::optional<ClusterConfig> cluster;
    peer_endpoints endpoints;
    if (cluster_path) {
        try {
            cluster = ClusterConfig::load(*cluster_path, parse_node_id(*node_id_arg));
            endpoints = resolve_cluster(io, *cluster);
        } catch (const config_error& e) {
            std::cerr << "Failed to load cluster config: " << e.what() << "\n";
            return 1;
        }
        // The config's client port (if any) is where others redirect clients
        // to us, so it must be where we actually listen.
        if (const auto configured = cluster->self_address().client_port) {
            if (client_port_given && client_port != *configured) {
                std::cerr << "--client-port " << client_port << " disagrees with the cluster config, which gives node "
                          << cluster->self_id() << " client port " << *configured << "\n";
                return 1;
            }
            client_port = *configured;
        }
        // The status port must not be any port this node or a node on the
        // same host already uses.
        if (status_port) {
            const std::string& host = cluster->self_address().host;
            std::string clash;
            if (*status_port == client_port) { clash = "this node's client port"; }
            for (const auto& [id, addr] : cluster->nodes()) {
                if (!clash.empty() || addr.host != host) { continue; }
                if (addr.port == *status_port) { clash = "node " + std::to_string(id) + "'s peer port"; }
                if (addr.client_port == *status_port) { clash = "node " + std::to_string(id) + "'s client port"; }
            }
            if (!clash.empty()) {
                std::cerr << "--status-port " << *status_port << " collides with " << clash << " on " << host << "\n";
                return 1;
            }
        }
    }

    try {
        // Rebuild state from the log before accepting any connections. In
        // cluster mode the log is only loaded here: Raft applies the
        // committed prefix itself when it starts.
        const auto recovered = init_engine(log_path, cluster ? 0 : UINT64_MAX);
        std::cout << "Replayed " << recovered.last_index << " log entries from " << log_path
                  << (cluster ? " (applying only committed entries, via Raft)" : "") << "\n";
        if (recovered.recovered_tail_bytes > 0) {
            std::cout << "Discarded " << recovered.recovered_tail_bytes
                      << " bytes of incomplete final entry (crash mid-write)\n";
        }
    } catch (const wal_error& e) {
        std::cerr << "Failed to load log: " << e.what() << "\n";
        return 1;
    }

    // Raft election runs on the same io_context as the client protocol.
    // Destruction order matters: status_server (reads raft) and peer_server
    // (holds raft's handlers) go first, then raft, then the client raft
    // sends through, then its state file.
    std::optional<RaftStateFile> raft_state;
    PersistentRaftLog raft_log(engine_log());
    std::optional<ConnectPerRpcClient> peer_client;
    std::optional<RaftNode> raft;
    std::optional<PeerServer> peer_server;
    std::optional<StatusServer> status_server;
    if (cluster) {
        const auto& self = endpoints.at(cluster->self_id());
        // Per-node by construction: each node already has its own log path.
        raft_state.emplace(RaftStateFile::path_for_log(log_path), cluster->self_id());
        raft_options options;
        options.state = &*raft_state;
        options.apply = [](const log_entry& e) { return apply_command(e.cmd); };
        // Replies slower than the shortest election timeout are stale anyway.
        peer_client.emplace(io, endpoints, kElectionTimeoutMin);
        try {
            raft.emplace(io, cluster->self_id(), cluster->peers(), *peer_client, raft_log, options);
        } catch (const raft_state_error& e) {
            std::cerr << "Failed to load Raft state: " << e.what() << "\n";
            return 1;
        }
        try {
            peer_server.emplace(io, self, raft->handlers());
        } catch (const std::system_error& e) {
            std::cerr << "Failed to listen for peers on " << self << ": " << e.what() << "\n";
            return 1;
        }
        std::cout << "Node " << cluster->self_id() << " peer RPC listening on " << self << " (peers:";
        for (const node_id peer : cluster->peers()) { std::cout << " " << peer; }
        std::cout << ")\n";
        // Read-only observability, opt-in. Its only access to Raft is the
        // const reads in status_json(); it never changes what the node does.
        if (status_port) {
            const tcp::endpoint where(asio::ip::address_v4::loopback(), *status_port);
            try {
                status_server.emplace(io, where, [&raft, &raft_log, seq = std::uint64_t{0}]() mutable {
                    return status_json(*raft, raft_log, ++seq);
                });
            } catch (const std::system_error& e) {
                std::cerr << "Failed to listen for status requests on " << where << ": " << e.what() << "\n";
                return 1;
            }
            std::cout << "status endpoint on http://127.0.0.1:" << *status_port << "/status\n";
        }
        raft->start();
    }

    std::optional<ClusterFrontEnd> front;
    if (cluster) { front.emplace(ClusterFrontEnd{*raft, *cluster}); }
    Server server(io, client_port, front ? &*front : nullptr);
    std::cout << "KV server listening on port " << client_port << "\n";
    io.run();
}
