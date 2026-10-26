//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../network/client.h"

#include <asio.hpp>
#include <iostream>
#include <optional>
#include <string>

using asio::ip::tcp;

// A cluster follower answers REDIRECT <id> <host>:<port>; follow at most
// this many in a row, then show the reply as is.
constexpr int kMaxRedirects = 3;

struct address
{
    std::string host;
    std::string port;
};

// The <host>:<port> of a REDIRECT <id> <host>:<port> reply; nullopt for any
// other reply, including a REDIRECT that names no address.
static std::optional<address> redirect_target(const std::string& reply) {
    if (reply.rfind("REDIRECT ", 0) != 0) { return std::nullopt; }
    const std::size_t sp = reply.find(' ', 9);
    if (sp == std::string::npos) { return std::nullopt; }
    const std::string where = reply.substr(sp + 1);
    const std::size_t colon = where.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == where.size()) { return std::nullopt; }
    return address{where.substr(0, colon), where.substr(colon + 1)};
}

static void connect_to(tcp::socket& socket, const address& to) {
    std::error_code ignored;
    socket.close(ignored);
    tcp::resolver resolver(socket.get_executor());
    asio::connect(socket, resolver.resolve(to.host, to.port));
}

// Sends one request line (newline included) and returns the reply line.
static std::string request(tcp::socket& socket, const std::string& line) {
    asio::write(socket, asio::buffer(line));

    asio::streambuf response;
    asio::read_until(socket, response, '\n');

    std::istream is(&response);
    std::string reply;
    std::getline(is, reply);
    return reply;
}

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr << "usage: " << argv[0] << " [host] [port]   (default 127.0.0.1 5555)\n";
        return 2;
    }
    address server{argc > 1 ? argv[1] : "127.0.0.1", argc > 2 ? argv[2] : "5555"};

    asio::io_context io;
    tcp::socket socket(io);

    try {
        connect_to(socket, server);

        std::string line;
        std::cout << "Connected to KV server\n";

        while (std::getline(std::cin, line)) {
            line += "\n";
            std::string reply = request(socket, line);

            // Not the leader: reconnect to the one it names and ask again.
            for (int hops = 0; hops < kMaxRedirects; ++hops) {
                const std::optional<address> leader = redirect_target(reply);
                if (!leader) { break; }
                std::cout << "(" << reply << ": reconnecting)\n";
                server = *leader;
                connect_to(socket, server);
                reply = request(socket, line);
            }

            std::cout << reply << std::endl;

            if (reply == "EXIT")
                break;
        }
    } catch (const std::system_error& e) {
        // Can't connect, or the server closed the connection (e.g. after
        // ERR_LINE_TOO_LONG, or it went down).
        std::cerr << "kv_client: connection to " << server.host << ":" << server.port << " failed: "
                  << e.what() << "\n";
        return 1;
    }
}
