#ifndef MINIDB_COMMAND_PARSER_HPP
#define MINIDB_COMMAND_PARSER_HPP

#include <string_view>
#include <variant>

#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace minidb {

/// The commands the CLI understands.
enum class CommandType {
    Set,
    Get,
    Delete,
    Exists,
    Keys,
    Clear,
    Help,
    Exit,
};

/// One parsed command line.
///
/// `key` and `value` are empty for commands that do not take them. A plain
/// struct is enough here: the parser has already validated the combination,
/// so there is no invariant left for a class to protect.
struct Command {
    CommandType type;
    Key key;
    Value value;
};

/// The outcome of parsing: either a command or an explanation of why the
/// input was not one. std::variant makes the exclusivity a compile-time fact,
/// so a caller cannot read a command out of a failed parse. The failure side
/// is the same Result the database returns, so the CLI has one error model to
/// render rather than two.
using ParseOutcome = std::variant<Command, Result>;

/// Parses a single line of CLI input.
///
/// Rules:
///   - Command names are matched case-insensitively; keys and values are not.
///   - Arguments are separated by whitespace, so keys cannot contain spaces.
///   - SET takes the remainder of the line as its value, with surrounding
///     whitespace trimmed and interior whitespace preserved, so
///     `SET name Aarya Lalan` stores exactly "Aarya Lalan".
///   - Commands that take no arguments reject extra ones rather than
///     silently ignoring them; a typo should be reported, not swallowed.
///
/// A blank line is reported as an InvalidArgument failure. The CLI skips
/// blank input before calling this, so pressing Enter is not an error there.
[[nodiscard]] ParseOutcome parse_command(std::string_view line);

}  // namespace minidb

#endif  // MINIDB_COMMAND_PARSER_HPP
