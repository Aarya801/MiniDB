#ifndef MINIDB_RESULT_HPP
#define MINIDB_RESULT_HPP

#include <iosfwd>
#include <string>
#include <string_view>

#include "minidb/types.hpp"

namespace minidb {

/// Every way a MiniDB operation can fail.
///
/// Errors that a caller is expected to handle -- a missing key, a malformed
/// command -- are values, not exceptions. Exceptions are reserved for
/// programming mistakes and genuinely exceptional conditions, so that normal
/// control flow stays visible in the code.
enum class StatusCode {
    Ok,
    NotFound,
    InvalidArgument,
    KeyTooLarge,
    ValueTooLarge,
    IoError,
    CorruptData,
    UnsupportedVersion,
    InvalidTransactionState,
    Internal,
};

/// Stable, human-readable name for a status code. Used in error messages,
/// test failures and the CLI.
[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// Streams the status name. Without this, a failed test comparison could only
/// report the enumerator as an opaque value.
std::ostream& operator<<(std::ostream& out, StatusCode code);

/// The outcome of a database operation: either success (optionally carrying a
/// value, as GET does) or a failure code with an explanatory message.
///
/// Result is a plain value type. It is copyable and movable, owns its strings,
/// and is marked [[nodiscard]] so a caller cannot silently ignore a failure.
class [[nodiscard]] Result {
public:
    /// Success with no payload -- SET, DELETE, CLEAR.
    static Result ok();

    /// Success carrying a value -- GET.
    static Result ok(Value value);

    /// Failure. `code` must not be StatusCode::Ok.
    static Result failure(StatusCode code, std::string message = {});

    [[nodiscard]] bool is_ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] StatusCode code() const noexcept { return code_; }

    /// The payload of a successful operation.
    /// Precondition: is_ok(). Calling this on a failed Result is a programming
    /// error and throws std::logic_error rather than returning junk.
    [[nodiscard]] const Value& value() const;

    /// Explanation of a failure. Empty for successful results.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// Rendering for humans: the payload on success, "CODE: message" on failure.
    [[nodiscard]] std::string to_display_string() const;

private:
    Result(StatusCode code, Value payload, std::string message);

    StatusCode code_;
    Value payload_;
    std::string message_;
};

}  // namespace minidb

#endif  // MINIDB_RESULT_HPP
