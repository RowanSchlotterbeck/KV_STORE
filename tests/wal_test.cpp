// Persistence and crash-recovery tests for PersistentLog.
// Every log lives in a fresh temp directory; nothing is written to the repo.
// Standalone (no framework): exits non-zero if any check fails.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include "../include/serialization.h"
#include "../include/state_machine.h"
#include "../include/wal.h"

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static fs::path g_tmp_root;

static fs::path temp_log(const std::string& name) {
    return g_tmp_root / name / "kvstore.log";
}

static std::uint64_t size_on_disk(const fs::path& p) {
    return static_cast<std::uint64_t>(fs::file_size(p));
}

static void append_raw(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::app);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static command put(const std::string& k, const std::string& v) { return {"PUT", k, v}; }
static command del(const std::string& k) { return {"DEL", k, std::nullopt}; }

// Expected contents: key -> value, or nullopt if the key must be absent.
using expected_t = std::map<std::string, std::optional<std::string>>;

static bool matches(const StateMachine& sm, const expected_t& expected) {
    bool ok = true;
    for (const auto& [key, value] : expected) {
        const std::string got = sm.get(command{"GET", key, std::nullopt});
        const std::string want = value.has_value() ? *value : "NOT_FOUND";
        if (got != want) {
            std::fprintf(stderr, "    key '%s': got '%s', want '%s'\n", key.c_str(), got.c_str(), want.c_str());
            ok = false;
        }
    }
    return ok;
}

static bool throws_wal_error(auto&& fn) {
    try { fn(); } catch (const wal_error&) { return true; }
    return false;
}

static void test_append_reopen_replay() {
    std::puts("test_append_reopen_replay");
    const fs::path path = temp_log("basic");
    {
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(log.replay(sm) == 0);
        CHECK(log.append(put("a", "1")) == 1);
        CHECK(log.append(put("b", "2")) == 2);
        CHECK(log.append(put("empty", "")) == 3);
        CHECK(log.append(put("gone", "x")) == 4);
        CHECK(log.append(del("gone")) == 5);
        CHECK(log.append(put("a", "overwritten")) == 6);
        CHECK(log.last_index() == 6);
    }

    PersistentLog log;
    log.open(path.string());
    StateMachine fresh;
    CHECK(log.replay(fresh) == 6);
    CHECK(log.last_index() == 6);
    CHECK(log.recovered_tail_bytes() == 0);
    CHECK(matches(fresh, {{"a", "overwritten"}, {"b", "2"}, {"empty", ""}, {"gone", std::nullopt}}));
    // Empty value is present, not absent.
    CHECK(fresh.get(command{"GET", "empty", std::nullopt}) == "");
    CHECK(fresh.list().find("empty => \n") != std::string::npos);
}

static void test_index_resumes_after_reopen() {
    std::puts("test_index_resumes_after_reopen");
    const fs::path path = temp_log("resume");
    for (std::uint64_t round = 0; round < 3; ++round) {
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(log.replay(sm) == round * 2);
        CHECK(log.append(put("k" + std::to_string(round), "a")) == round * 2 + 1);
        CHECK(log.append(put("k" + std::to_string(round), "b")) == round * 2 + 2);
    }
    // Replay validates that indices are exactly 1..N with no gap or reuse.
    PersistentLog log;
    log.open(path.string());
    StateMachine sm;
    CHECK(log.replay(sm) == 6);
    CHECK(matches(sm, {{"k0", "b"}, {"k1", "b"}, {"k2", "b"}}));
}

static void test_append_requires_replay() {
    std::puts("test_append_requires_replay");
    PersistentLog log;
    CHECK(throws_wal_error([&] { log.append(put("a", "1")); }));  // not open
    log.open(temp_log("needs_replay").string());
    CHECK(throws_wal_error([&] { log.append(put("a", "1")); }));  // not replayed
    StateMachine sm;
    log.replay(sm);
    CHECK(log.append(put("a", "1")) == 1);
    CHECK(throws_wal_error([&] { log.replay(sm); }));             // only once
}

// Writes N complete entries, then the first `cut` bytes of entry N+1, as a
// crash mid-append would leave it. Covers every cut point, i.e. both inside
// the 4-byte length prefix and inside the body.
static void test_torn_tail_recovery() {
    std::puts("test_torn_tail_recovery");
    const std::vector<command> cmds = {put("a", "1"), put("b", ""), del("a"), put("c", "three")};
    const std::uint64_t n = cmds.size();
    const expected_t expected = {{"a", std::nullopt}, {"b", ""}, {"c", "three"}};

    const fs::path good = temp_log("torn_base");
    {
        PersistentLog log;
        log.open(good.string());
        StateMachine sm;
        log.replay(sm);
        for (const auto& c : cmds) { log.append(c); }
    }
    const std::uint64_t good_size = size_on_disk(good);

    std::string torn_frame;
    CHECK(serialize_entry(log_entry{n + 1, 0, put("torn", "never committed")}, torn_frame));
    CHECK(torn_frame.size() > kFrameHeaderSize + 1);

    int cases = 0;
    for (std::size_t cut = 1; cut < torn_frame.size(); ++cut) {
        const fs::path path = temp_log("torn_" + std::to_string(cut));
        fs::create_directories(path.parent_path());
        fs::copy_file(good, path);
        append_raw(path, torn_frame.substr(0, cut));
        CHECK(size_on_disk(path) == good_size + cut);

        {
            PersistentLog log;
            log.open(path.string());
            StateMachine sm;
            CHECK(log.replay(sm) == n);                       // all complete entries recovered
            CHECK(log.recovered_tail_bytes() == cut);
            CHECK(size_on_disk(path) == good_size);              // truncated to last good offset
            CHECK(matches(sm, expected));
            CHECK(sm.get(command{"GET", "torn", std::nullopt}) == "NOT_FOUND");

            CHECK(log.append(put("after", "crash")) == n + 1); // resumes on a clean boundary
        }
        {
            PersistentLog log;
            log.open(path.string());
            StateMachine sm;
            CHECK(log.replay(sm) == n + 1);
            CHECK(log.recovered_tail_bytes() == 0);
            expected_t after = expected;
            after["after"] = "crash";
            after["torn"] = std::nullopt;
            CHECK(matches(sm, after));
        }
        ++cases;
    }
    std::printf("  %d cut points checked (prefix: 1-%zu, body: %zu-%zu)\n",
                cases, kFrameHeaderSize - 1, kFrameHeaderSize, torn_frame.size() - 1);
}

static void test_corruption_refuses_to_start() {
    std::puts("test_corruption_refuses_to_start");

    // Oversized length prefix in the middle of the file.
    {
        const fs::path path = temp_log("corrupt_length");
        {
            PersistentLog log;
            log.open(path.string());
            StateMachine sm;
            log.replay(sm);
            log.append(put("a", "1"));
        }
        const std::uint64_t first_end = size_on_disk(path);
        append_raw(path, std::string("\xFF\xFF\xFF\xFF", 4));
        {
            std::string good_frame;
            serialize_entry(log_entry{2, 0, put("b", "2")}, good_frame);
            append_raw(path, good_frame);
        }
        const std::uint64_t size_before = size_on_disk(path);
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(throws_wal_error([&] { log.replay(sm); }));
        CHECK(size_on_disk(path) == size_before);  // nothing discarded
        CHECK(throws_wal_error([&] { log.append(put("x", "y")); }));
        CHECK(first_end < size_before);
    }

    // Well-formed frames whose indices skip (1, 3).
    {
        const fs::path path = temp_log("corrupt_gap");
        fs::create_directories(path.parent_path());
        std::string bytes;
        serialize_entry(log_entry{1, 0, put("a", "1")}, bytes);
        serialize_entry(log_entry{3, 0, put("b", "2")}, bytes);
        append_raw(path, bytes);
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(throws_wal_error([&] { log.replay(sm); }));
        CHECK(size_on_disk(path) == bytes.size());
    }
}

int main() {
    std::string tmpl = (fs::temp_directory_path() / "kv_wal_test.XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) {
        std::perror("mkdtemp");
        return 2;
    }
    g_tmp_root = tmpl;
    for (const char* name : {"basic", "resume", "needs_replay", "torn_base", "corrupt_length"}) {
        fs::create_directories(g_tmp_root / name);
    }

    test_append_reopen_replay();
    test_index_resumes_after_reopen();
    test_append_requires_replay();
    test_torn_tail_recovery();
    test_corruption_refuses_to_start();

    fs::remove_all(g_tmp_root);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all wal tests passed");
    return 0;
}
