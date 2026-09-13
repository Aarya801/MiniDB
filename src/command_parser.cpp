#include "minidb/command_parser.hpp"

#include <cctype>
#include <string>

namespace minidb {
namespace {

[[nodiscard]] bool is_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
           character == '\v' || character == '\f';
}

/// std::tolower takes an int that must be representable as unsigned char.
/// Passing a plain char is undefined behaviour for bytes above 127 on
/// platforms where char is signed, which is most of them.
[[nodiscard]] char to_lower(char character) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

[[nodiscard]] bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (to_lower(left[index]) != to_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Removes the first whitespace-delimited token from `rest` and returns it,
/// leaving `rest` starting at the character after that token. Returns an
/// empty view when `rest` holds nothing but whitespace.
[[nodiscard]] std::string_view take_token(std::string_view& rest) noexcept {
    std::size_t begin = 0;
    while (begin < rest.size() && is_space(rest[begin])) {
        ++begin;
    }
    rest.remove_prefix(begin);

    std::size_t length = 0;
    while (length < rest.size() && !is_space(rest[length])) {
        ++length;
    }
    const std::string_view token = rest.substr(0, length);
    rest.remove_prefix(length);
    return token;
}

/// SET is the only command whose last argument may contain spaces.
[[nodiscard]] ParseOutcome parse_set(std::string_view rest) {
    const std::string_view key = take_token(rest);
    if (key.empty()) {
        return Result::failure(StatusCode::InvalidArgument, "SET requires a key and a value");
    }

    const std::string_view value = trim(rest);
    if (value.empty()) {
        return Result::failure(StatusCode::InvalidArgument, "SET requires a value");
    }
    return Command{CommandType::Set, Key(key), Value(value)};
}

/// GET, DELETE and EXISTS: exactly one key, nothing after it.
[[nodiscard]] ParseOutcome parse_single_key(CommandType type, std::string_view name,
                                            std::string_view rest) {
    const std::string_view key = take_token(rest);
    if (key.empty()) {
        return Result::failure(StatusCode::InvalidArgument, std::string(name) + " requires a key");
    }
    if (!trim(rest).empty()) {
        return Result::failure(StatusCode::InvalidArgument,
                               std::string(name) + " takes exactly one argument");
    }
    return Command{type, Key(key), Value{}};
}

/// KEYS, CLEAR, transaction control, HELP and EXIT take nothing at all.
[[nodiscard]] ParseOutcome parse_without_arguments(CommandType type, std::string_view name,
                                                   std::string_view rest) {
    if (!trim(rest).empty()) {
        return Result::failure(StatusCode::InvalidArgument,
                               std::string(name) + " takes no arguments");
    }
    return Command{type, Key{}, Value{}};
}

}  // namespace

ParseOutcome parse_command(std::string_view line) {
    std::string_view rest = line;
    const std::string_view name = take_token(rest);

    if (name.empty()) {
        return Result::failure(StatusCode::InvalidArgument, "empty command");
    }
    if (equals_ignore_case(name, "SET")) {
        return parse_set(rest);
    }
    if (equals_ignore_case(name, "GET")) {
        return parse_single_key(CommandType::Get, "GET", rest);
    }
    if (equals_ignore_case(name, "DELETE")) {
        return parse_single_key(CommandType::Delete, "DELETE", rest);
    }
    if (equals_ignore_case(name, "EXISTS")) {
        return parse_single_key(CommandType::Exists, "EXISTS", rest);
    }
    if (equals_ignore_case(name, "KEYS")) {
        return parse_without_arguments(CommandType::Keys, "KEYS", rest);
    }
    if (equals_ignore_case(name, "CLEAR")) {
        return parse_without_arguments(CommandType::Clear, "CLEAR", rest);
    }
    if (equals_ignore_case(name, "BEGIN")) {
        return parse_without_arguments(CommandType::Begin, "BEGIN", rest);
    }
    if (equals_ignore_case(name, "COMMIT")) {
        return parse_without_arguments(CommandType::Commit, "COMMIT", rest);
    }
    if (equals_ignore_case(name, "ROLLBACK")) {
        return parse_without_arguments(CommandType::Rollback, "ROLLBACK", rest);
    }
    if (equals_ignore_case(name, "HELP")) {
        return parse_without_arguments(CommandType::Help, "HELP", rest);
    }
    if (equals_ignore_case(name, "EXIT")) {
        return parse_without_arguments(CommandType::Exit, "EXIT", rest);
    }

    return Result::failure(StatusCode::InvalidArgument,
                           "unknown command '" + std::string(name) + "'; type HELP for the list");
}

}  // namespace minidb
