//
// Network benchmark client for kv_server.
//
// Opens a single TCP connection and issues a synchronous
// request/response loop, mirroring how kv_client talks to the server.
// This measures real round-trip cost (syscalls + asio + engine),
// as opposed to bench_engine which measures the engine in-process.
//
// Usage: kv_bench_network [host] [port] [num_ops]

#include <asio.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <istream>
#include <string>
#include <vector>

using asio::ip::tcp;
using clock_type = std::chrono::steady_clock;

static double percentile(std::vector<double>& sorted_us, double p) {
    return sorted_us[static_cast<size_t>(sorted_us.size() * p)];
}

static double run_case(tcp::socket& socket, const std::string& name, long n,
                        const std::vector<std::string>& lines) {
    std::vector<double> latencies;
    latencies.reserve(n);

    auto case_start = clock_type::now();
    for (long i = 0; i < n; ++i) {
        std::string line = lines[i % lines.size()] + "\n";

        auto start = clock_type::now();
        asio::write(socket, asio::buffer(line));

        asio::streambuf response;
        asio::read_until(socket, response, '\n');
        std::istream is(&response);
        std::string reply;
        std::getline(is, reply);
        auto end = clock_type::now();

        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    auto case_end = clock_type::now();

    std::sort(latencies.begin(), latencies.end());
    double total_ms = std::chrono::duration<double, std::milli>(case_end - case_start).count();
    double ops_per_sec = n / (total_ms / 1000.0);
    std::printf("%-24s %8ld ops  %9.2f ms  %10.0f ops/sec  p50=%.1fus  p99=%.1fus\n",
                name.c_str(), n, total_ms, ops_per_sec,
                percentile(latencies, 0.50), percentile(latencies, 0.99));
    return ops_per_sec;
}

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    std::string port = argc > 2 ? argv[2] : "5555";
    long n = argc > 3 ? std::atol(argv[3]) : 20000;

    asio::io_context io;
    tcp::socket socket(io);
    tcp::resolver resolver(io);

    try {
        asio::connect(socket, resolver.resolve(host, port));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to connect to %s:%s (%s)\n"
                              "Start kv_server first: ./kv_server\n",
                      host.c_str(), port.c_str(), e.what());
        return 1;
    }

    std::printf("KV_STORE network benchmark against %s:%s (%ld ops per case)\n\n",
                host.c_str(), port.c_str(), n);

    std::vector<std::string> put_lines;
    std::vector<std::string> get_lines;
    for (int i = 0; i < 1000; ++i) {
        std::string key = "netkey" + std::to_string(i);
        put_lines.push_back("PUT \"" + key + "\" \"value" + std::to_string(i) + "\"");
        get_lines.push_back("GET \"" + key + "\"");
    }

    run_case(socket, "PUT", n, put_lines);
    run_case(socket, "GET (hit)", n, get_lines);
    run_case(socket, "GET (miss)", n, {"GET \"nonexistent\""});

    return 0;
}
