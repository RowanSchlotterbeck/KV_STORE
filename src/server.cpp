//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include <server.h>
#include <optional>
#include "../include/wal.h"
#include "peer_transport.h"
#include "../include/raft_node.h"


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

static int usage(const char* prog) {
    std::cerr << "usage: " << prog
              << " [log_path] [--cluster <config_file> --node-id <id>] [--client-port <port>]\n";
    return 2;
}

int main(int argc, char** argv) {
    std::string log_path = PersistentLog::kDefaultPath;
    std::optional<std::string> cluster_path;
    std::optional<std::string> node_id_arg;
    std::uint16_t client_port = 5555;
    bool client_port_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cluster" && i + 1 < argc) {
            cluster_path = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id_arg = argv[++i];
        } else if (arg == "--client-port" && i + 1 < argc) {
            const std::string value = argv[++i];
            unsigned long port = 0;
            try { port = std::stoul(value); } catch (const std::exception&) {}
            if (port == 0 || port > 65535 || value.find_first_not_of("0123456789") != std::string::npos) {
                return usage(argv[0]);
            }
            client_port = static_cast<std::uint16_t>(port);
            client_port_given = true;
        } else if (i == 1 && arg.rfind("--", 0) != 0) {
            log_path = arg;
        } else {
            return usage(argv[0]);
        }
    }
    if (cluster_path.has_value() != node_id_arg.has_value()) { return usage(argv[0]); }

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
    // Destruction order matters: peer_server (holds raft's handlers) goes
    // first, then raft, then the client raft sends through, then its state file.
    std::optional<RaftStateFile> raft_state;
    PersistentRaftLog raft_log(engine_log());
    std::optional<ConnectPerRpcClient> peer_client;
    std::optional<RaftNode> raft;
    std::optional<PeerServer> peer_server;
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
        raft->start();
    }

    std::optional<ClusterFrontEnd> front;
    if (cluster) { front.emplace(ClusterFrontEnd{*raft, *cluster}); }
    Server server(io, client_port, front ? &*front : nullptr);
    std::cout << "KV server listening on port " << client_port << "\n";
    io.run();
}
