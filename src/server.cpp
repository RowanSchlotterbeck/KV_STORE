//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include <server.h>
#include <optional>
#include "../include/wal.h"
#include "peer_transport.h"


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
    Server(asio::io_context& io, short port)
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
    std::cerr << "usage: " << prog << " [log_path] [--cluster <config_file> --node-id <id>]\n";
    return 2;
}

int main(int argc, char** argv) {
    std::string log_path = PersistentLog::kDefaultPath;
    std::optional<std::string> cluster_path;
    std::optional<std::string> node_id_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cluster" && i + 1 < argc) {
            cluster_path = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id_arg = argv[++i];
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

    // Peer RPC listener and client share the one io_context. Handlers are
    // stubs until Raft lands; the client is ready for it but not yet used.
    std::optional<PeerServer> peer_server;
    std::optional<ConnectPerRpcClient> peer_client;
    if (cluster) {
        const auto& self = endpoints.at(cluster->self_id());
        try {
            peer_server.emplace(io, self, stub_peer_handlers());
        } catch (const std::system_error& e) {
            std::cerr << "Failed to listen for peers on " << self << ": " << e.what() << "\n";
            return 1;
        }
        peer_client.emplace(io, endpoints);
        std::cout << "Node " << cluster->self_id() << " peer RPC listening on " << self << " (peers:";
        for (const node_id peer : cluster->peers()) { std::cout << " " << peer; }
        std::cout << ")\n";
    }

    Server server(io, 5555);
    std::cout << "KV server listening on port 5555\n";
    io.run();
}
