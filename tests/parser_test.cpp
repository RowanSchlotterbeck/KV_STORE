// Parser and line-protocol tests. Covers the empty-value PUT fix and the
// former out-of-bounds read on short PUTs, plus unchanged behavior elsewhere.
// Standalone (no framework): exits non-zero if any check fails.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>
#include "../include/parser.h"
#include "../include/engine.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using args_t = std::vector<std::string>;
static const args_t kParseError = {"Error"};

// process_line shares one StateMachine across the process, so tests use
// distinct keys and run in a fixed order.

static void test_put_empty_value() {
    std::puts("test_put_empty_value");
    CHECK(input_parser(R"(PUT "a" "")") == (args_t{"PUT", "a", ""}));
    CHECK(process_line(R"(PUT "a" "")") == "OK");
    CHECK(process_line(R"(GET "a")") == "");          // present but empty, not NOT_FOUND
    CHECK(process_line(R"(DEL "a")") == "OK");
    CHECK(process_line(R"(GET "a")") == "NOT_FOUND");
}

static void test_put_too_few_args() {
    std::puts("test_put_too_few_args");
    // Unbalanced quote count: rejected before any indexing (unchanged).
    CHECK(input_parser(R"(PUT "a")") == kParseError);
    CHECK(process_line(R"(PUT "a")") == "INPUT_ERROR");
    CHECK(input_parser("PUT") == kParseError);
    CHECK(process_line("PUT") == "INPUT_ERROR");

    // Four quotes but fewer than three tokens: formerly read past the end
    // of args; now reaches process_line's arity check.
    CHECK(process_line(R"(PUT "" "v")") == "ERR_USAGE PUT <key> <value>");
    CHECK(process_line(R"(PUT "" "")") == "ERR_USAGE PUT <key> <value>");
    CHECK(process_line(R"(PUT "a""b")") == "ERR_USAGE PUT <key> <value>");
    CHECK(process_line(R"(PUT "a""")") == "ERR_USAGE PUT <key> <value>");
}

static void test_put_with_value_unchanged() {
    std::puts("test_put_with_value_unchanged");
    CHECK(input_parser(R"(PUT "b" "hello")") == (args_t{"PUT", "b", "hello"}));
    CHECK(process_line(R"(PUT "b" "hello")") == "OK");
    CHECK(process_line(R"(GET "b")") == "hello");
    CHECK(input_parser(R"(PUT "k" "v w")") == (args_t{"PUT", "k", "v w"}));
}

static void test_other_commands_unchanged() {
    std::puts("test_other_commands_unchanged");
    CHECK(input_parser(R"(GET "b")") == (args_t{"GET", "b"}));
    CHECK(input_parser(R"(DEL "b")") == (args_t{"DEL", "b"}));
    CHECK(input_parser("LIST") == (args_t{"LIST"}));
    CHECK(process_line(R"(DEL "b")") == "OK");
    CHECK(process_line(R"(DEL "b")") == "NOT_FOUND");
    CHECK(process_line(R"(GET "missing")") == "NOT_FOUND");
    CHECK(process_line("LIST") == "(empty)");
    CHECK(process_line("EXIT") == "EXIT");

    // Garbage / malformed input.
    CHECK(process_line("") == "INPUT_ERROR");
    CHECK(process_line("   ") == "INPUT_ERROR");
    CHECK(process_line("garbage") == "ERR_UNKNOWN_CMD");
    CHECK(process_line("GET a") == "INPUT_ERROR");
    CHECK(process_line(R"(GET "a" "b")") == "INPUT_ERROR");

    // Empty quoted strings outside the PUT value slot are still dropped.
    CHECK(process_line(R"("")") == "INPUT_ERROR");
    CHECK(process_line(R"(GET "")") == "ERR_USAGE GET <key>");
    CHECK(process_line(R"(DEL "")") == "ERR_USAGE DEL <key>");
    CHECK(process_line(R"(PUT "a" "b" "")") == "INPUT_ERROR");
}

static void test_non_ascii_bytes() {
    std::puts("test_non_ascii_bytes");
    // UTF-8 "é" (0xC3 0xA9) in key and value.
    CHECK(input_parser("PUT \"caf\xC3\xA9\" \"\xC3\xA9t\xC3\xA9\"")
          == (args_t{"PUT", "caf\xC3\xA9", "\xC3\xA9t\xC3\xA9"}));
    CHECK(process_line("PUT \"caf\xC3\xA9\" \"\xC3\xA9t\xC3\xA9\"") == "OK");
    CHECK(process_line("GET \"caf\xC3\xA9\"") == "\xC3\xA9t\xC3\xA9");
    CHECK(process_line("DEL \"caf\xC3\xA9\"") == "OK");

    // Every byte 0x80-0xFF, alone as a value, survives intact and is never
    // treated as whitespace (0xA0 and 0x85 are spaces in some locales).
    for (int b = 0x80; b <= 0xFF; ++b) {
        const std::string byte(1, static_cast<char>(b));
        CHECK(input_parser("PUT \"hi\" \"" + byte + "\"") == (args_t{"PUT", "hi", byte}));
        CHECK(process_line("PUT \"hi\" \"" + byte + "\"") == "OK");
        CHECK(process_line("GET \"hi\"") == byte);
        // Unquoted, a high byte still doesn't split tokens.
        CHECK(input_parser("x" + byte + "y") == (args_t{"X" + byte + "Y"}));
    }
    CHECK(process_line("DEL \"hi\"") == "OK");

    // High bytes outside quotes: unchanged error handling.
    CHECK(process_line("\xC3\xA9") == "ERR_UNKNOWN_CMD");
    CHECK(process_line("GET \x80") == "INPUT_ERROR"); // unquoted key: quote count != 2
}

int main() {
    // Writes go through the persistent log; keep it in a temp dir.
    std::string tmp = (std::filesystem::temp_directory_path() / "kv_parser_test.XXXXXX").string();
    if (!::mkdtemp(tmp.data())) {
        std::perror("mkdtemp");
        return 2;
    }
    init_engine((std::filesystem::path(tmp) / "kvstore.log").string());

    test_put_empty_value();
    test_put_too_few_args();
    test_put_with_value_unchanged();
    test_other_commands_unchanged();
    test_non_ascii_bytes();

    std::filesystem::remove_all(tmp);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all parser tests passed");
    return 0;
}
