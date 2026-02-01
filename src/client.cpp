//
// Created by Rowan Schlotterbeck on 1/31/26.
//

#include "../network/client.h"

#include <asio.hpp>
#include <iostream>
#include <string>

using asio::ip::tcp;

int main() {
    asio::io_context io;
    tcp::socket socket(io);
    tcp::resolver resolver(io);

    asio::connect(socket, resolver.resolve("127.0.0.1", "5555"));

    std::string line;
    std::cout << "Connected to KV server\n";

    while (std::getline(std::cin, line)) {
        line += "\n";
        asio::write(socket, asio::buffer(line));

        asio::streambuf response;
        asio::read_until(socket, response, '\n');

        std::istream is(&response);
        std::string reply;
        std::getline(is, reply);

        std::cout << reply << std::endl;

        if (reply == "EXIT")
            break;
    }
}