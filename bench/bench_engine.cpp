//
// In-process benchmark for the KV engine.
//
// Measures the full command path (parse + validate + execute) via
// process_line(), and the raw map-operation path via execute(), so
// parser overhead can be seen separately from unordered_map overhead.
// process_line PUT includes a durable (fsynced) log append per write, using a
// throwaway log in a temp directory.
//
// Usage: kv_bench_engine [num_ops]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#include "engine.h"
#include "kv_store.h"

using clock_type = std::chrono::steady_clock;

struct Result {
    std::string name;
    long ops;
    double total_ms;
    double p50_us;
    double p99_us;
};

static Result summarize(const std::string& name, std::vector<double>& latencies_us) {
    std::sort(latencies_us.begin(), latencies_us.end());
    double total_us = 0;
    for (double l : latencies_us) total_us += l;

    Result r;
    r.name = name;
    r.ops = static_cast<long>(latencies_us.size());
    r.total_ms = total_us / 1000.0;
    r.p50_us = latencies_us[latencies_us.size() / 2];
    r.p99_us = latencies_us[static_cast<size_t>(latencies_us.size() * 0.99)];
    return r;
}

static void print_result(const Result& r) {
    double ops_per_sec = r.ops / (r.total_ms / 1000.0);
    std::printf("%-28s %10ld ops  %9.2f ms  %12.0f ops/sec  p50=%.2fus  p99=%.2fus\n",
                r.name.c_str(), r.ops, r.total_ms, ops_per_sec, r.p50_us, r.p99_us);
}

// Benchmarks the full text-protocol path used by the server: parsing,
// validation, and execution against a fresh store each time.
static Result bench_process_line(const std::string& name, long n,
                                  const std::vector<std::string>& lines) {
    std::vector<double> latencies;
    latencies.reserve(n);

    for (long i = 0; i < n; ++i) {
        const std::string& line = lines[i % lines.size()];
        auto start = clock_type::now();
        volatile auto reply = process_line(line);
        (void)reply;
        auto end = clock_type::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    return summarize(name, latencies);
}

// Benchmarks execute() directly with pre-parsed args, isolating
// unordered_map performance from string parsing/validation.
static Result bench_execute(const std::string& name, long n,
                             std::unordered_map<std::string, std::string>& map,
                             const std::vector<std::vector<std::string>>& arg_sets) {
    std::vector<double> latencies;
    latencies.reserve(n);

    for (long i = 0; i < n; ++i) {
        const auto& args = arg_sets[i % arg_sets.size()];
        auto start = clock_type::now();
        volatile auto reply = execute(map, args);
        (void)reply;
        auto end = clock_type::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    return summarize(name, latencies);
}

int main(int argc, char** argv) {
    long n = 200000;
    if (argc > 1) n = std::atol(argv[1]);

    std::printf("KV_STORE engine benchmark (%ld ops per case)\n\n", n);

    std::string tmp = (std::filesystem::temp_directory_path() / "kv_bench_engine.XXXXXX").string();
    if (!::mkdtemp(tmp.data())) {
        std::perror("mkdtemp");
        return 1;
    }
    init_engine((std::filesystem::path(tmp) / "kvstore.log").string());

    // --- Full protocol path (process_line: parse + validate + execute) ---
    std::vector<std::string> put_lines;
    std::vector<std::string> get_lines;
    for (long i = 0; i < 1000; ++i) {
        std::string key = "key" + std::to_string(i);
        put_lines.push_back("PUT \"" + key + "\" \"value" + std::to_string(i) + "\"");
        get_lines.push_back("GET \"" + key + "\"");
    }

    // Pre-populate so GET has hits to find.
    for (const auto& line : put_lines) process_line(line);

    print_result(bench_process_line("process_line PUT", n, put_lines));
    print_result(bench_process_line("process_line GET (hit)", n, get_lines));
    print_result(bench_process_line("process_line GET (miss)", n, {"GET \"nonexistent\""}));

    // --- Raw map path (execute: skips parsing/validation) ---
    std::unordered_map<std::string, std::string> map;
    std::vector<std::vector<std::string>> put_args;
    std::vector<std::vector<std::string>> get_args;
    for (long i = 0; i < 1000; ++i) {
        std::string key = "key" + std::to_string(i);
        put_args.push_back({"PUT", key, "value" + std::to_string(i)});
        get_args.push_back({"GET", key});
    }
    for (const auto& args : put_args) execute(map, args);

    std::printf("\n");
    print_result(bench_execute("execute PUT", n, map, put_args));
    print_result(bench_execute("execute GET (hit)", n, map, get_args));
    print_result(bench_execute("execute GET (miss)", n, map, {{"GET", "nonexistent"}}));
    print_result(bench_execute("execute DEL", n, map, put_args));

    std::filesystem::remove_all(tmp);
    return 0;
}
