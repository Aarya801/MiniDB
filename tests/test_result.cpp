// Tests for the Result / StatusCode error model.

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "minidb/result.hpp"
#include "test_framework.hpp"

using minidb::Result;
using minidb::StatusCode;

TEST(ok_result_reports_success_and_empty_payload) {
    const Result result = Result::ok();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::Ok);
    EXPECT_EQ(result.value(), std::string(""));
    EXPECT_EQ(result.message(), std::string(""));
}

TEST(ok_result_carries_its_payload) {
    const Result result = Result::ok("Aarya");
    // ASSERT rather than EXPECT: value() throws on a failed Result, so there is
    // no point continuing this case if the result is not ok.
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), std::string("Aarya"));
}

TEST(payload_may_contain_embedded_nul_bytes) {
    // Keys and values are byte strings, not C strings. A NUL in the middle
    // must survive, which is why std::string is used rather than const char*.
    const std::string binary("a\0b", 3);
    const Result result = Result::ok(binary);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().size(), static_cast<std::size_t>(3));
    EXPECT_EQ(result.value(), binary);
}

TEST(failure_result_reports_code_and_message) {
    const Result result = Result::failure(StatusCode::NotFound, "no such key");
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.code(), StatusCode::NotFound);
    EXPECT_EQ(result.message(), std::string("no such key"));
}

TEST(failure_message_is_optional) {
    const Result result = Result::failure(StatusCode::IoError);
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.message(), std::string(""));
}

TEST(reading_the_value_of_a_failure_is_a_programming_error) {
    const Result result = Result::failure(StatusCode::NotFound);
    // Cast to void: Result::value() is [[nodiscard]], and discarding it here is
    // the point of the test. Casting to void is how you say "deliberately
    // ignored" -- and -Wold-style-cast does not fire on casts to void.
    EXPECT_THROWS_AS((void)result.value(), std::logic_error);
}

TEST(a_failure_cannot_claim_success) {
    EXPECT_THROWS_AS((void)Result::failure(StatusCode::Ok), std::logic_error);
}

TEST(display_string_is_the_payload_on_success) {
    EXPECT_EQ(Result::ok("18").to_display_string(), std::string("18"));
}

TEST(display_string_combines_code_and_message_on_failure) {
    EXPECT_EQ(Result::failure(StatusCode::NotFound).to_display_string(), std::string("NOT_FOUND"));
    EXPECT_EQ(Result::failure(StatusCode::InvalidArgument, "empty key").to_display_string(),
              std::string("INVALID_ARGUMENT: empty key"));
}

TEST(every_status_code_has_a_distinct_name) {
    const StatusCode codes[] = {
        StatusCode::Ok,
        StatusCode::NotFound,
        StatusCode::InvalidArgument,
        StatusCode::KeyTooLarge,
        StatusCode::ValueTooLarge,
        StatusCode::IoError,
        StatusCode::CorruptData,
        StatusCode::UnsupportedVersion,
        StatusCode::InvalidTransactionState,
        StatusCode::Internal,
    };

    for (const StatusCode code : codes) {
        EXPECT_FALSE(minidb::to_string(code).empty());
        EXPECT_NE(minidb::to_string(code), std::string_view("UNKNOWN_STATUS"));
    }

    for (std::size_t i = 0; i < std::size(codes); ++i) {
        for (std::size_t j = i + 1; j < std::size(codes); ++j) {
            EXPECT_NE(minidb::to_string(codes[i]), minidb::to_string(codes[j]));
        }
    }
}

TEST(results_are_movable_without_losing_their_payload) {
    Result source = Result::ok("value");
    const Result moved = std::move(source);
    ASSERT_TRUE(moved.is_ok());
    EXPECT_EQ(moved.value(), std::string("value"));
}

MINIDB_TEST_MAIN()
