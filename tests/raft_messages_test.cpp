// Round-trip tests for Raft RPC message conversions.
// Each message goes domain -> proto -> domain, and also through the wire
// (domain -> proto -> bytes -> proto -> domain).
// Standalone (no framework): exits non-zero if any check fails.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include "../include/raft_convert.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

static bool same(const command& a, const command& b) {
    return a.operation == b.operation && a.key == b.key && a.value == b.value;
}

static bool same(const log_entry& a, const log_entry& b) {
    return a.index == b.index && a.term == b.term && same(a.cmd, b.cmd);
}

static bool same(const request_vote_request& a, const request_vote_request& b) {
    return a.term == b.term && a.candidate_id == b.candidate_id
        && a.last_log_index == b.last_log_index && a.last_log_term == b.last_log_term;
}

static bool same(const request_vote_response& a, const request_vote_response& b) {
    return a.term == b.term && a.vote_granted == b.vote_granted;
}

static bool same(const append_entries_request& a, const append_entries_request& b) {
    if (a.term != b.term || a.leader_id != b.leader_id || a.prev_log_index != b.prev_log_index
        || a.prev_log_term != b.prev_log_term || a.leader_commit != b.leader_commit
        || a.entries.size() != b.entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        if (!same(a.entries[i], b.entries[i])) { return false; }
    }
    return true;
}

static bool same(const append_entries_response& a, const append_entries_response& b) {
    return a.term == b.term && a.success == b.success;
}

// domain -> proto -> domain
template <typename T>
static T via_proto(const T& msg) {
    return from_proto(to_proto(msg));
}

// domain -> proto -> bytes -> proto -> domain
template <typename T>
static bool via_wire(const T& msg, T& out) {
    std::string bytes;
    if (!to_proto(msg).SerializeToString(&bytes)) { return false; }
    decltype(to_proto(msg)) parsed;
    if (!parsed.ParseFromString(bytes)) { return false; }
    out = from_proto(parsed);
    return true;
}

template <typename T>
static void check_round_trip(const T& msg) {
    CHECK(same(via_proto(msg), msg));
    T wire;
    CHECK(via_wire(msg, wire));
    CHECK(same(wire, msg));
}

static void test_request_vote_request() {
    std::puts("test_request_vote_request");
    check_round_trip(request_vote_request{5, 2, 41, 4});
    check_round_trip(request_vote_request{});  // all defaults (zero)
    check_round_trip(request_vote_request{kMax, kMax, kMax - 1, kMax});

    const auto pb = to_proto(request_vote_request{5, 2, 41, 4});
    CHECK(pb.term() == 5);
    CHECK(pb.candidate_id() == 2);
    CHECK(pb.last_log_index() == 41);
    CHECK(pb.last_log_term() == 4);
}

static void test_request_vote_response() {
    std::puts("test_request_vote_response");
    check_round_trip(request_vote_response{7, true});
    check_round_trip(request_vote_response{7, false});
    check_round_trip(request_vote_response{kMax, true});
    CHECK(via_proto(request_vote_response{1, true}).vote_granted);
    CHECK(!via_proto(request_vote_response{1, false}).vote_granted);
}

static void test_append_entries_heartbeat() {
    std::puts("test_append_entries_heartbeat");
    const append_entries_request hb{3, 1, 10, 2, {}, 9};
    check_round_trip(hb);
    const auto pb = to_proto(hb);
    CHECK(pb.entries_size() == 0);
    CHECK(via_proto(hb).entries.empty());
}

static void test_append_entries_with_entries() {
    std::puts("test_append_entries_with_entries");
    append_entries_request req;
    req.term = 4;
    req.leader_id = 3;
    req.prev_log_index = 10;
    req.prev_log_term = 3;
    req.leader_commit = 10;
    req.entries = {
        log_entry{11, 4, command{"PUT", "user", std::string("alice")}},
        log_entry{12, 4, command{"PUT", "empty", std::string{}}},
        log_entry{13, 4, command{"DEL", "user", std::nullopt}},
        // Non-ASCII UTF-8 and an embedded NUL. (Command.key/value are proto3
        // `string`, so non-UTF-8 bytes are rejected on parse; not exercised here.)
        log_entry{14, 4, command{"PUT", "caf\xC3\xA9", std::string("\xC3\xA9\x00t", 4)}},
    };
    check_round_trip(req);

    const auto pb = to_proto(req);
    CHECK(pb.entries_size() == 4);
    const append_entries_request back = via_proto(req);
    CHECK(back.entries.size() == 4);
    if (back.entries.size() == 4) {
        CHECK(back.entries[0].cmd.value == "alice");
        CHECK(back.entries[1].cmd.value.has_value());   // empty value: present...
        CHECK(back.entries[1].cmd.value == "");         // ...and empty
        CHECK(back.entries[2].cmd.operation == "DEL");
        CHECK(!back.entries[2].cmd.value.has_value());  // DEL: absent
        CHECK(back.entries[3].cmd.value == std::string("\xC3\xA9\x00t", 4));
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK(back.entries[i].index == 11 + i);
            CHECK(back.entries[i].term == 4);
        }
    }
    // Presence is also preserved on the embedded proto LogEntry.
    CHECK(pb.entries(1).command().has_value());
    CHECK(!pb.entries(2).command().has_value());
}

static void test_append_entries_large_values() {
    std::puts("test_append_entries_large_values");
    append_entries_request req{kMax, kMax, kMax - 1, kMax - 2, {}, kMax};
    req.entries.push_back(log_entry{kMax, kMax, command{"PUT", "k", std::string("v")}});
    check_round_trip(req);
}

static void test_append_entries_response() {
    std::puts("test_append_entries_response");
    check_round_trip(append_entries_response{9, true});
    check_round_trip(append_entries_response{9, false});
    check_round_trip(append_entries_response{kMax, false});
    CHECK(via_proto(append_entries_response{1, true}).success);
    CHECK(!via_proto(append_entries_response{1, false}).success);
}

int main() {
    test_request_vote_request();
    test_request_vote_response();
    test_append_entries_heartbeat();
    test_append_entries_with_entries();
    test_append_entries_large_values();
    test_append_entries_response();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all raft message tests passed");
    return 0;
}
