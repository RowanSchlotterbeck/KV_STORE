// Round-trip tests for the log entry serialization layer.
// Standalone (no framework): exits non-zero if any check fails.

#include <cstdio>
#include <string>
#include <vector>
#include "../include/serialization.h"
#include "../include/proto_convert.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static log_entry make_entry(std::uint64_t index, std::uint64_t term, command cmd) {
    log_entry e;
    e.index = index;
    e.term = term;
    e.cmd = std::move(cmd);
    return e;
}

static bool round_trip(const log_entry& in, log_entry& out) {
    std::string buf;
    if (!serialize_entry(in, buf)) { return false; }
    std::size_t offset = 0;
    if (deserialize_entry(buf, offset, out) != decode_status::ok) { return false; }
    return offset == buf.size();
}

static void test_put_with_value() {
    std::puts("test_put_with_value");
    log_entry out;
    CHECK(round_trip(make_entry(1, 1, {"PUT", "user:1", "alice"}), out));
    CHECK(out.index == 1);
    CHECK(out.term == 1);
    CHECK(out.cmd.operation == "PUT");
    CHECK(out.cmd.key == "user:1");
    CHECK(out.cmd.value.has_value());
    CHECK(out.cmd.value == "alice");
}

static void test_del_value_absent() {
    std::puts("test_del_value_absent");
    log_entry out;
    CHECK(round_trip(make_entry(2, 1, {"DEL", "user:1", std::nullopt}), out));
    CHECK(out.cmd.operation == "DEL");
    CHECK(out.cmd.key == "user:1");
    CHECK(!out.cmd.value.has_value());

    // Presence is also preserved at the proto boundary itself.
    CHECK(!to_proto(command{"DEL", "k", std::nullopt}).has_value());
}

static void test_put_empty_value_present() {
    std::puts("test_put_empty_value_present");
    log_entry out;
    CHECK(round_trip(make_entry(3, 2, {"PUT", "k", std::string{}}), out));
    CHECK(out.cmd.operation == "PUT");
    CHECK(out.cmd.value.has_value());
    CHECK(out.cmd.value.has_value() && out.cmd.value->empty());

    CHECK(to_proto(command{"PUT", "k", std::string{}}).has_value());
}

static void test_multiple_entries_in_order() {
    std::puts("test_multiple_entries_in_order");
    const std::vector<log_entry> entries = {
        make_entry(1, 1, {"PUT", "a", "1"}),
        make_entry(2, 1, {"PUT", "b", std::string{}}),
        make_entry(3, 2, {"DEL", "a", std::nullopt}),
        make_entry(4, 3, {"PUT", "c", std::string(10000, 'x')}),
        make_entry(5, 3, {"DEL", "b", std::nullopt}),
    };

    std::string buf;
    for (const auto& e : entries) { CHECK(serialize_entry(e, buf)); }

    std::size_t offset = 0;
    for (const auto& expected : entries) {
        log_entry got;
        CHECK(deserialize_entry(buf, offset, got) == decode_status::ok);
        CHECK(got.index == expected.index);
        CHECK(got.term == expected.term);
        CHECK(got.cmd.operation == expected.cmd.operation);
        CHECK(got.cmd.key == expected.cmd.key);
        CHECK(got.cmd.value == expected.cmd.value);
    }
    log_entry extra;
    CHECK(deserialize_entry(buf, offset, extra) == decode_status::end_of_input);
    CHECK(offset == buf.size());
}

static void test_truncated_final_entry() {
    std::puts("test_truncated_final_entry");
    std::string buf;
    CHECK(serialize_entry(make_entry(1, 1, {"PUT", "a", "1"}), buf));
    const std::size_t first_frame_end = buf.size();
    CHECK(serialize_entry(make_entry(2, 1, {"PUT", "b", "some longer value"}), buf));

    // Every possible cut point inside the second frame (header or payload)
    // must yield the first entry, then `truncated` with offset unchanged.
    for (std::size_t cut = first_frame_end + 1; cut < buf.size(); ++cut) {
        // Copy into an exactly-sized buffer so sanitizers would catch any overread.
        const std::string truncated_buf = buf.substr(0, cut);
        std::size_t offset = 0;
        log_entry e;
        CHECK(deserialize_entry(truncated_buf, offset, e) == decode_status::ok);
        CHECK(e.index == 1);
        const std::size_t before = offset;
        CHECK(deserialize_entry(truncated_buf, offset, e) == decode_status::truncated);
        CHECK(offset == before);
        CHECK(e.index == 1); // output left untouched on failure
    }
}

static void test_oversized_length_is_corrupt() {
    std::puts("test_oversized_length_is_corrupt");
    const std::string buf("\xFF\xFF\xFF\xFF", 4);
    std::size_t offset = 0;
    log_entry e;
    CHECK(deserialize_entry(buf, offset, e) == decode_status::corrupt);
    CHECK(offset == 0);
}

int main() {
    test_put_with_value();
    test_del_value_absent();
    test_put_empty_value_present();
    test_multiple_entries_in_order();
    test_truncated_final_entry();
    test_oversized_length_is_corrupt();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all serialization tests passed");
    return 0;
}
