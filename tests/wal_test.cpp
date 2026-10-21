// Persistence and crash-recovery tests for PersistentLog.
// Every log lives in a fresh temp directory; nothing is written to the repo.
// Standalone (no framework): exits non-zero if any check fails.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include "../include/crc32c.h"
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

// Regression: a value that isn't valid UTF-8 used to be logged and acked,
// then make replay refuse to start ("undecodable payload").
static void test_non_utf8_survives_replay() {
    std::puts("test_non_utf8_survives_replay");
    std::string all_bytes;
    for (int b = 0; b < 256; ++b) { all_bytes.push_back(static_cast<char>(b)); }
    const fs::path path = temp_log("non_utf8");
    {
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        log.replay(sm);
        log.append(put("bin", "\xFF"));
        log.append(put("k\xFF\xFE", "\xC3"));
        log.append(put("all", all_bytes));
        log.append(put("doomed\x80", "x"));
        log.append(del("doomed\x80"));
    }
    PersistentLog log;
    log.open(path.string());
    StateMachine sm;
    CHECK(log.replay(sm) == 5);
    CHECK(matches(sm, {{"bin", "\xFF"}, {"k\xFF\xFE", "\xC3"}, {"all", all_bytes}, {"doomed\x80", std::nullopt}}));
}

// last_term() reports the final entry's term after replay (Raft's up-to-date
// check reads it), and 0 for an empty log.
static void test_last_term_after_replay() {
    std::puts("test_last_term_after_replay");
    const fs::path path = temp_log("last_term");
    {
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(log.replay(sm) == 0);
        CHECK(log.last_index() == 0 && log.last_term() == 0);
    }
    append_raw(path, PersistentLog::encode_frame(log_entry{1, 3, put("a", "1")})
                   + PersistentLog::encode_frame(log_entry{2, 5, put("b", "2")}));
    PersistentLog log;
    log.open(path.string());
    StateMachine sm;
    CHECK(log.replay(sm) == 2);
    CHECK(log.last_index() == 2 && log.last_term() == 5);
    CHECK(log.append(put("c", "3")) == 3);  // appends still write term 0 for now
    CHECK(log.last_term() == 0);
}

// term_at(i): the term of entry i, 0 for index 0, nullopt past the end.
static void test_term_at() {
    std::puts("test_term_at");
    const fs::path path = temp_log("term_at");
    {
        PersistentLog log;
        log.open(path.string());
        CHECK(log.term_at(0) == std::optional<std::uint64_t>(0));  // even before replay
        CHECK(!log.term_at(1).has_value());
        StateMachine sm;
        CHECK(log.replay(sm) == 0);
        CHECK(log.term_at(0) == std::optional<std::uint64_t>(0));  // empty log
        CHECK(!log.term_at(1).has_value());
    }
    const std::vector<std::uint64_t> terms = {1, 1, 2, 2, 2, 5};
    std::string frames;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        frames += PersistentLog::encode_frame(log_entry{i + 1, terms[i], put("k" + std::to_string(i), "v")});
    }
    append_raw(path, frames);

    PersistentLog log;
    log.open(path.string());
    StateMachine sm;
    CHECK(log.replay(sm) == terms.size());
    CHECK(log.term_at(0) == std::optional<std::uint64_t>(0));
    for (std::size_t i = 0; i < terms.size(); ++i) {
        CHECK(log.term_at(i + 1) == std::optional<std::uint64_t>(terms[i]));
    }
    CHECK(!log.term_at(terms.size() + 1).has_value());
    CHECK(!log.term_at(1000).has_value());
    CHECK(!log.term_at(UINT64_MAX).has_value());
    // Appends extend it (term 0 until Raft assigns terms).
    CHECK(log.append(put("x", "y")) == 7);
    CHECK(log.term_at(7) == std::optional<std::uint64_t>(0));
    CHECK(log.term_at(6) == std::optional<std::uint64_t>(5));
    CHECK(!log.term_at(8).has_value());

    // Reopening resets it until replay; a torn tail leaves no phantom entry.
    log.close();
    append_raw(path, PersistentLog::encode_frame(log_entry{8, 9, put("torn", "x")}).substr(0, 10));
    log.open(path.string());
    CHECK(!log.term_at(1).has_value());
    StateMachine sm2;
    CHECK(log.replay(sm2) == 7);
    CHECK(log.term_at(7) == std::optional<std::uint64_t>(0));
    CHECK(!log.term_at(8).has_value());
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

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void write_file(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Replays `path` into a fresh state machine; returns the wal_error message,
// or an empty string if replay succeeded.
static std::string replay_error(const fs::path& path) {
    PersistentLog log;
    log.open(path.string());
    StateMachine sm;
    try {
        log.replay(sm);
    } catch (const wal_error& e) {
        return e.what();
    }
    return "";
}

static const std::vector<command> kCmds = {put("a", "1"), put("b", ""), del("a"), put("c", "three")};
static const expected_t kExpected = {{"a", std::nullopt}, {"b", ""}, {"c", "three"}};

struct good_log
{
    fs::path path;
    std::uint64_t size;
    std::vector<std::uint64_t> frame_offsets;  // start of each entry's frame
};

// Writes kCmds through PersistentLog, recording where each frame starts.
static good_log write_good_log(const std::string& name) {
    good_log g{temp_log(name), 0, {}};
    fs::create_directories(g.path.parent_path());
    PersistentLog log;
    log.open(g.path.string());
    StateMachine sm;
    log.replay(sm);
    for (const auto& c : kCmds) {
        g.frame_offsets.push_back(size_on_disk(g.path));
        log.append(c);
    }
    g.size = size_on_disk(g.path);
    return g;
}

// Asserts that `path` (a copy of a good log plus `tail_bytes` of damaged tail)
// recovers every complete entry, is truncated back to `good_size`, accepts a
// new append, and that the new entry replays.
static void check_tail_recovers(const fs::path& path, std::uint64_t good_size, std::uint64_t tail_bytes) {
    const std::uint64_t n = kCmds.size();
    {
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(log.replay(sm) == n);                         // all complete entries recovered
        CHECK(log.recovered_tail_bytes() == tail_bytes);
        CHECK(size_on_disk(path) == good_size);             // truncated to last good offset
        CHECK(matches(sm, kExpected));
        CHECK(log.append(put("after", "crash")) == n + 1);  // resumes on a clean boundary
    }
    {
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(log.replay(sm) == n + 1);
        CHECK(log.recovered_tail_bytes() == 0);
        expected_t after = kExpected;
        after["after"] = "crash";
        after["torn"] = std::nullopt;
        CHECK(matches(sm, after));
    }
}

static void test_crc32c_known_answers() {
    std::puts("test_crc32c_known_answers");
    std::string zeros(32, '\0'), ones(32, '\xFF'), inc(32, '\0'), dec(32, '\0');
    for (int i = 0; i < 32; ++i) {
        inc[i] = static_cast<char>(i);
        dec[i] = static_cast<char>(31 - i);
    }
    // "123456789" check value, plus the RFC 3720 (iSCSI) appendix B.4 vectors.
    const std::vector<std::pair<std::string, std::uint32_t>> vectors = {
        {"", 0x00000000u},
        {"123456789", 0xE3069283u},
        {zeros, 0x8A9136AAu},
        {ones, 0x62A8AB43u},
        {inc, 0x46DD794Eu},
        {dec, 0x113FDB5Cu},
    };
    for (const auto& [data, want] : vectors) {
        CHECK(crc32c(data) == want);
        CHECK(crc32c_software(data.data(), data.size()) == want);
    }
    // Accelerated and table paths agree on every length/alignment up to 64.
    std::string bytes;
    for (int i = 0; i < 100; ++i) { bytes.push_back(static_cast<char>(i * 37 + 11)); }
    for (std::size_t start = 0; start < 8; ++start) {
        for (std::size_t len = 0; len + start <= 72; ++len) {
            CHECK(crc32c(bytes.data() + start, len) == crc32c_software(bytes.data() + start, len));
        }
    }
    std::printf("  crc32c path: %s\n", crc32c_is_hardware() ? "hardware" : "software");
}

static void test_frame_layout() {
    std::puts("test_frame_layout");
    const log_entry e{7, 0, put("k", "v")};
    const std::string frame = PersistentLog::encode_frame(e);
    std::string payload;
    CHECK(encode_entry_payload(e, payload));
    CHECK(frame.size() == PersistentLog::kHeaderSize + payload.size());
    const auto u32_at = [&](std::size_t i) {
        std::uint32_t v = 0;
        for (int b = 3; b >= 0; --b) { v = (v << 8) | static_cast<unsigned char>(frame[i + b]); }
        return v;
    };
    CHECK(u32_at(0) == payload.size());
    CHECK(u32_at(4) == crc32c(payload));
    CHECK(frame.substr(PersistentLog::kHeaderSize) == payload);
}

// Writes N complete entries, then the first `cut` bytes of entry N+1, as a
// crash mid-append would leave it. Covers every cut point, i.e. inside the
// 8-byte [length][crc] header and inside the body.
static void test_torn_tail_recovery() {
    std::puts("test_torn_tail_recovery");
    const good_log good = write_good_log("torn_base");
    const std::string torn_frame =
        PersistentLog::encode_frame(log_entry{kCmds.size() + 1, 0, put("torn", "never committed")});
    CHECK(torn_frame.size() > PersistentLog::kHeaderSize + 1);

    int cases = 0;
    for (std::size_t cut = 1; cut < torn_frame.size(); ++cut) {
        const fs::path path = temp_log("torn_" + std::to_string(cut));
        fs::create_directories(path.parent_path());
        fs::copy_file(good.path, path);
        append_raw(path, torn_frame.substr(0, cut));
        CHECK(size_on_disk(path) == good.size + cut);
        check_tail_recovers(path, good.size, cut);
        ++cases;
    }
    std::printf("  %d cut points checked (header: 1-%zu, body: %zu-%zu)\n", cases,
                PersistentLog::kHeaderSize - 1, PersistentLog::kHeaderSize, torn_frame.size() - 1);
}

// A crash on some filesystems leaves zeros instead of a short file. Previously
// this refused to start; it must now recover like a torn tail.
static void test_zeroed_tail_recovery() {
    std::puts("test_zeroed_tail_recovery");
    const good_log good = write_good_log("zero_base");
    for (std::size_t zeros : {1, 3, 7, 8, 9, 64, 4096}) {
        const fs::path path = temp_log("zero_" + std::to_string(zeros));
        fs::create_directories(path.parent_path());
        fs::copy_file(good.path, path);
        append_raw(path, std::string(zeros, '\0'));
        check_tail_recovers(path, good.size, zeros);
    }

    // Zeros followed by real data are not a crash tail: discarding them would
    // drop committed entries, so this must refuse to start.
    const fs::path path = temp_log("zero_then_data");
    fs::create_directories(path.parent_path());
    fs::copy_file(good.path, path);
    append_raw(path, std::string(16, '\0'));
    append_raw(path, PersistentLog::encode_frame(log_entry{kCmds.size() + 1, 0, put("x", "y")}));
    const std::string before = read_file(path);
    const std::string err = replay_error(path);
    CHECK(err.find("zero-filled header") != std::string::npos);
    CHECK(err.find("byte offset " + std::to_string(good.size) + " ") != std::string::npos);
    CHECK(read_file(path) == before);
}

// Flips one bit at `pos` in a copy of the good log; replay must refuse to
// start, name the damaged frame's offset, and leave the file untouched.
static void check_bit_flip_refused(const good_log& good, const std::string& name, std::uint64_t pos,
                                   std::uint64_t frame_offset, const std::string& expect_in_error) {
    const fs::path path = temp_log(name);
    fs::create_directories(path.parent_path());
    std::string bytes = read_file(good.path);
    bytes[pos] = static_cast<char>(bytes[pos] ^ 0x10);
    write_file(path, bytes);

    const std::string err = replay_error(path);
    CHECK(!err.empty());
    CHECK(err.find(expect_in_error) != std::string::npos);
    CHECK(err.find("byte offset " + std::to_string(frame_offset) + " ") != std::string::npos);
    CHECK(read_file(path) == bytes);  // nothing truncated or rewritten
    if (err.find(expect_in_error) == std::string::npos) { std::fprintf(stderr, "    error was: %s\n", err.c_str()); }
}

static void test_bit_flips_refuse_to_start() {
    std::puts("test_bit_flips_refuse_to_start");
    const good_log good = write_good_log("flip_base");
    const std::uint64_t mid = good.frame_offsets[1];   // second of four entries
    const std::uint64_t last = good.frame_offsets[3];  // final entry: still corrupt, not "torn"
    const std::uint64_t h = PersistentLog::kHeaderSize;

    check_bit_flip_refused(good, "flip_payload", mid + h + 2, mid, "checksum mismatch");
    check_bit_flip_refused(good, "flip_crc", mid + 5, mid, "checksum mismatch");
    check_bit_flip_refused(good, "flip_last_payload", good.size - 1, last, "checksum mismatch");
    check_bit_flip_refused(good, "flip_last_crc", last + 4, last, "checksum mismatch");
}

static void test_corruption_refuses_to_start() {
    std::puts("test_corruption_refuses_to_start");
    const good_log good = write_good_log("corrupt_base");

    // Oversized length in the middle of the file.
    {
        const fs::path path = temp_log("corrupt_length");
        fs::create_directories(path.parent_path());
        fs::copy_file(good.path, path);
        append_raw(path, std::string("\xFF\xFF\xFF\xFF\x00\x00\x00\x00", 8));
        append_raw(path, PersistentLog::encode_frame(log_entry{kCmds.size() + 1, 0, put("b", "2")}));
        const std::string before = read_file(path);
        PersistentLog log;
        log.open(path.string());
        StateMachine sm;
        CHECK(throws_wal_error([&] { log.replay(sm); }));
        CHECK(read_file(path) == before);  // nothing discarded
        CHECK(throws_wal_error([&] { log.append(put("x", "y")); }));
    }

    // Zero length with a non-zero CRC: never written by append.
    {
        const fs::path path = temp_log("corrupt_zero_len");
        fs::create_directories(path.parent_path());
        fs::copy_file(good.path, path);
        append_raw(path, std::string("\x00\x00\x00\x00\x01\x00\x00\x00", 8));
        CHECK(replay_error(path).find("zero length") != std::string::npos);
    }

    // Valid frames (correct CRCs) whose indices skip (1, 3).
    {
        const fs::path path = temp_log("corrupt_gap");
        fs::create_directories(path.parent_path());
        const std::string bytes = PersistentLog::encode_frame(log_entry{1, 0, put("a", "1")})
                                + PersistentLog::encode_frame(log_entry{3, 0, put("b", "2")});
        append_raw(path, bytes);
        CHECK(replay_error(path).find("expected index 2") != std::string::npos);
        CHECK(read_file(path) == bytes);
    }
}

int main() {
    std::string tmpl = (fs::temp_directory_path() / "kv_wal_test.XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) {
        std::perror("mkdtemp");
        return 2;
    }
    g_tmp_root = tmpl;
    for (const char* name : {"basic", "resume", "needs_replay", "non_utf8", "last_term", "term_at"}) {
        fs::create_directories(g_tmp_root / name);
    }

    test_crc32c_known_answers();
    test_frame_layout();
    test_append_reopen_replay();
    test_index_resumes_after_reopen();
    test_append_requires_replay();
    test_last_term_after_replay();
    test_term_at();
    test_non_utf8_survives_replay();
    test_torn_tail_recovery();
    test_zeroed_tail_recovery();
    test_bit_flips_refuse_to_start();
    test_corruption_refuses_to_start();

    fs::remove_all(g_tmp_root);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all wal tests passed");
    return 0;
}
