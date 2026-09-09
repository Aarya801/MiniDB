#include "minidb/result.hpp"

#include <ostream>
#include <stdexcept>
#include <utility>

namespace minidb {

std::string_view to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok:
            return "OK";
        case StatusCode::NotFound:
            return "NOT_FOUND";
        case StatusCode::InvalidArgument:
            return "INVALID_ARGUMENT";
        case StatusCode::KeyTooLarge:
            return "KEY_TOO_LARGE";
        case StatusCode::ValueTooLarge:
            return "VALUE_TOO_LARGE";
        case StatusCode::IoError:
            return "IO_ERROR";
        case StatusCode::CorruptData:
            return "CORRUPT_DATA";
        case StatusCode::UnsupportedVersion:
            return "UNSUPPORTED_VERSION";
        case StatusCode::InvalidTransactionState:
            return "INVALID_TRANSACTION_STATE";
        case StatusCode::Internal:
            return "INTERNAL_ERROR";
    }
    // Reached only if a StatusCode value was manufactured by a cast. No
    // default label above, so adding an enumerator makes the compiler warn
    // about the unhandled case instead of silently falling through to here.
    return "UNKNOWN_STATUS";
}

std::ostream& operator<<(std::ostream& out, StatusCode code) {
    return out << to_string(code);
}

Result::Result(StatusCode code, Value payload, std::string message)
    : code_(code), payload_(std::move(payload)), message_(std::move(message)) {}

Result Result::ok() {
    return Result(StatusCode::Ok, Value{}, std::string{});
}

Result Result::ok(Value value) {
    return Result(StatusCode::Ok, std::move(value), std::string{});
}

Result Result::failure(StatusCode code, std::string message) {
    if (code == StatusCode::Ok) {
        // Constructing a "failure" that reports success would let a real error
        // pass silently through every is_ok() check downstream.
        throw std::logic_error("Result::failure called with StatusCode::Ok");
    }
    return Result(code, Value{}, std::move(message));
}

const Value& Result::value() const {
    if (!is_ok()) {
        throw std::logic_error("Result::value called on a failed Result (" +
                               std::string(to_string(code_)) + ")");
    }
    return payload_;
}

std::string Result::to_display_string() const {
    if (is_ok()) {
        return payload_;
    }
    std::string text(to_string(code_));
    if (!message_.empty()) {
        text += ": ";
        text += message_;
    }
    return text;
}

}  // namespace minidb
