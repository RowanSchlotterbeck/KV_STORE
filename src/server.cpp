//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include <server.h>
#include "../include/wal.h"


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

int main(int argc, char** argv) {
    const std::string log_path = argc > 1 ? argv[1] : PersistentLog::kDefaultPath;
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

    asio::io_context io;
    Server server(io, 5555);
    std::cout << "KV server listening on port 5555\n";
    io.run();
}