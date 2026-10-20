//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include <server.h>
#include <optional>
#include "../include/wal.h"
#include "peer_transport.h"
#include "../include/raft_node.h"


using asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket)
        : socket_(std::move(socket)) {}

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

                    std::string response = process_line(line);
                    response += "\n";

                    write(response);
                    std::cout << response;
                }
            });
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
};

class Server {
public:
    Server(asio::io_context& io, std::uint16_t port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)) {
        accept();
    }

private:
    void accept() {
        acceptor_.async_accept(
            [this](std::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket))->start();
                }
                accept();
            });
    }

    tcp::acceptor acceptor_;
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
    }

    try {
        // Rebuild state from the log before accepting any connections.
        const auto recovered = init_engine(log_path);
        std::cout << "Replayed " << recovered.last_index << " log entries from " << log_path << "\n";
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
    // first, then raft, then the client raft sends through.
    std::optional<ConnectPerRpcClient> peer_client;
    std::optional<RaftNode> raft;
    std::optional<PeerServer> peer_server;
    if (cluster) {
        const auto& self = endpoints.at(cluster->self_id());
        // Replies slower than the shortest election timeout are stale anyway.
        peer_client.emplace(io, endpoints, kElectionTimeoutMin);
        raft.emplace(io, cluster->self_id(), cluster->peers(), *peer_client, [] {
            const engine_log_position pos = engine_last_log_position();
            return log_position{pos.last_index, pos.last_term};
        });
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

    Server server(io, client_port);
    std::cout << "KV server listening on port " << client_port << "\n";
    io.run();
}
