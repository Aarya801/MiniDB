// Tests for the CLI command parser.

#include "minidb/command_parser.hpp"

#include <string>
#include <variant>

#include "minidb/result.hpp"
#include "test_framework.hpp"

using minidb::Command;
using minidb::CommandType;
using minidb::parse_command;
using minidb::ParseOutcome;
using minidb::Result;
using minidb::StatusCode;

namespace {

/// True when the parse succeeded.
bool parsed(const ParseOutcome& outcome) {
    return std::holds_alternative<Command>(outcome);
}

/// The failure code of a rejected parse. Returns StatusCode::Ok if the parse
/// unexpectedly succeeded, which no test expects, so it shows up as a failure.
StatusCode failure_code(const ParseOutcome& outcome) {
    const Result* error = std::get_if<Result>(&outcome);
    return error == nullptr ? StatusCode::Ok : error->code();
}

}  // namespace

// -------------------------------------------------------------------------
// SET
// -------------------------------------------------------------------------

TEST(set_parses_a_key_and_value) {
    const ParseOutcome outcome = parse_command("SET name Aarya");
    ASSERT_TRUE(parsed(outcome));

    const Command& command = std::get<Command>(outcome);
    EXPECT_TRUE(command.type == CommandType::Set);
    EXPECT_EQ(command.key, std::string("name"));
    EXPECT_EQ(command.value, std::string("Aarya"));
}

TEST(set_keeps_spaces_inside_the_value) {
    const ParseOutcome outcome = parse_command("SET name Aarya Lalan");
    ASSERT_TRUE(parsed(outcome));
    EXPECT_EQ(std::get<Command>(outcome).value, std::string("Aarya Lalan"));
}

TEST(set_trims_around_the_value_but_not_inside_it) {
    const ParseOutcome outcome = parse_command("  SET   name   two   words   ");
    ASSERT_TRUE(parsed(outcome));

    const Command& command = std::get<Command>(outcome);
    EXPECT_EQ(command.key, std::string("name"));
    EXPECT_EQ(command.value, std::string("two   words"));
}

TEST(set_accepts_punctuation_in_the_value) {
    const ParseOutcome outcome = parse_command("SET greeting Hello, world! 42 = 42");
    ASSERT_TRUE(parsed(outcome));
    EXPECT_EQ(std::get<Command>(outcome).value, std::string("Hello, world! 42 = 42"));
}

TEST(set_without_a_key_or_value_is_rejected) {
    EXPECT_EQ(failure_code(parse_command("SET")), StatusCode::InvalidArgument);
}

TEST(set_without_a_value_is_rejected) {
    EXPECT_EQ(failure_code(parse_command("SET name")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("SET name    ")), StatusCode::InvalidArgument);
}

// -------------------------------------------------------------------------
// Single-key commands
// -------------------------------------------------------------------------

TEST(get_parses_one_key) {
    const ParseOutcome outcome = parse_command("GET name");
    ASSERT_TRUE(parsed(outcome));

    const Command& command = std::get<Command>(outcome);
    EXPECT_TRUE(command.type == CommandType::Get);
    EXPECT_EQ(command.key, std::string("name"));
    EXPECT_EQ(command.value, std::string(""));
}

TEST(delete_parses_one_key) {
    const ParseOutcome outcome = parse_command("DELETE age");
    ASSERT_TRUE(parsed(outcome));
    EXPECT_TRUE(std::get<Command>(outcome).type == CommandType::Delete);
    EXPECT_EQ(std::get<Command>(outcome).key, std::string("age"));
}

TEST(exists_parses_one_key) {
    const ParseOutcome outcome = parse_command("EXISTS age");
    ASSERT_TRUE(parsed(outcome));
    EXPECT_TRUE(std::get<Command>(outcome).type == CommandType::Exists);
    EXPECT_EQ(std::get<Command>(outcome).key, std::string("age"));
}

TEST(single_key_commands_require_a_key) {
    EXPECT_EQ(failure_code(parse_command("GET")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("DELETE")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("EXISTS")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("GET   ")), StatusCode::InvalidArgument);
}

TEST(single_key_commands_reject_a_second_argument) {
    // Silently ignoring the extra word would hide a typo from the user.
    EXPECT_EQ(failure_code(parse_command("GET name extra")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("DELETE name extra")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("EXISTS name extra")), StatusCode::InvalidArgument);
}

// -------------------------------------------------------------------------
// Commands without arguments
// -------------------------------------------------------------------------

TEST(argumentless_commands_parse) {
    const ParseOutcome keys = parse_command("KEYS");
    ASSERT_TRUE(parsed(keys));
    EXPECT_TRUE(std::get<Command>(keys).type == CommandType::Keys);

    const ParseOutcome clear = parse_command("CLEAR");
    ASSERT_TRUE(parsed(clear));
    EXPECT_TRUE(std::get<Command>(clear).type == CommandType::Clear);

    const ParseOutcome help = parse_command("HELP");
    ASSERT_TRUE(parsed(help));
    EXPECT_TRUE(std::get<Command>(help).type == CommandType::Help);

    const ParseOutcome exit = parse_command("EXIT");
    ASSERT_TRUE(parsed(exit));
    EXPECT_TRUE(std::get<Command>(exit).type == CommandType::Exit);
}

TEST(argumentless_commands_tolerate_surrounding_whitespace) {
    const ParseOutcome outcome = parse_command("   KEYS   ");
    ASSERT_TRUE(parsed(outcome));
    EXPECT_TRUE(std::get<Command>(outcome).type == CommandType::Keys);
}

TEST(argumentless_commands_reject_arguments) {
    EXPECT_EQ(failure_code(parse_command("KEYS extra")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("CLEAR everything")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("HELP me")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("EXIT now")), StatusCode::InvalidArgument);
}

// -------------------------------------------------------------------------
// Case handling
// -------------------------------------------------------------------------

TEST(command_names_are_case_insensitive) {
    EXPECT_TRUE(parsed(parse_command("set k v")));
    EXPECT_TRUE(parsed(parse_command("Set k v")));
    EXPECT_TRUE(parsed(parse_command("sEt k v")));
    EXPECT_TRUE(parsed(parse_command("get k")));
    EXPECT_TRUE(parsed(parse_command("keys")));
    EXPECT_TRUE(parsed(parse_command("ExIt")));
}

TEST(keys_and_values_keep_their_case) {
    const ParseOutcome outcome = parse_command("set MyKey MyValue");
    ASSERT_TRUE(parsed(outcome));

    const Command& command = std::get<Command>(outcome);
    EXPECT_EQ(command.key, std::string("MyKey"));
    EXPECT_EQ(command.value, std::string("MyValue"));
}

// -------------------------------------------------------------------------
// Bad input
// -------------------------------------------------------------------------

TEST(unknown_commands_are_rejected) {
    EXPECT_EQ(failure_code(parse_command("FROBNICATE x")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("SELECT * FROM users")), StatusCode::InvalidArgument);
}

TEST(an_unknown_command_is_named_in_the_error) {
    const ParseOutcome outcome = parse_command("FROBNICATE x");
    const Result* error = std::get_if<Result>(&outcome);
    ASSERT_TRUE(error != nullptr);
    EXPECT_TRUE(error->message().find("FROBNICATE") != std::string::npos);
}

TEST(a_near_miss_command_name_is_not_accepted) {
    // Prefix matching would turn a typo into a different command.
    EXPECT_EQ(failure_code(parse_command("SETX k v")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("GE k")), StatusCode::InvalidArgument);
}

TEST(a_blank_line_is_rejected) {
    // The CLI skips blank input before parsing; the parser still reports it
    // rather than inventing a command.
    EXPECT_EQ(failure_code(parse_command("")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("    ")), StatusCode::InvalidArgument);
    EXPECT_EQ(failure_code(parse_command("\t\r\n")), StatusCode::InvalidArgument);
}

TEST(every_rejection_explains_itself) {
    const char* const bad_inputs[] = {"", "SET", "SET k", "GET", "GET a b", "KEYS x", "NOPE"};
    for (const char* input : bad_inputs) {
        const ParseOutcome outcome = parse_command(input);
        const Result* error = std::get_if<Result>(&outcome);
        ASSERT_TRUE(error != nullptr);
        EXPECT_FALSE(error->message().empty());
    }
}

MINIDB_TEST_MAIN()
