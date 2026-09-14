#include "minidb/hash_table.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace minidb::detail {
namespace {

/// Bucket counts, each roughly twice the previous one and every one a prime.
///
/// Doubling keeps the amortised cost of growth constant: each rehash is O(n),
/// but it happens after the table has doubled, so the cost spread over the
/// inserts that caused it is O(1) each. Growing by a fixed amount instead
/// would make n inserts cost O(n^2) in total.
///
/// The last entry covers roughly eight million entries at the 0.75 load
/// factor, which is far past anything MiniDB is meant for; beyond it the
/// search below takes over.
constexpr std::array<std::size_t, 20> kBucketSizes = {
    17,    37,    79,    163,    331,    673,    1361,    2729,    5471,    10949,
    21911, 43853, 87719, 175447, 350899, 701819, 1403641, 2807303, 5614657, 11229331,
};

[[nodiscard]] bool is_prime(std::size_t value) noexcept {
    if (value < 2) {
        return false;
    }
    if (value % 2 == 0) {
        return value == 2;
    }
    // Only odd divisors up to the square root can divide an odd number.
    // The division form avoids both a floating-point sqrt and multiplication
    // overflow near the largest representable size.
    for (std::size_t divisor = 3; divisor <= value / divisor; divisor += 2) {
        if (value % divisor == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::size_t next_bucket_count(std::size_t minimum) {
    const auto entry = std::lower_bound(kBucketSizes.begin(), kBucketSizes.end(), minimum);
    if (entry != kBucketSizes.end()) {
        return *entry;
    }

    // Past the table: search upward for the next prime. Only reachable with
    // millions of entries, and the search is short because primes stay dense.
    std::size_t candidate = minimum | 1U;
    while (!is_prime(candidate)) {
        if (candidate > std::numeric_limits<std::size_t>::max() - 2) {
            throw std::length_error("HashTable bucket count is too large");
        }
        candidate += 2;
    }
    return candidate;
}

}  // namespace minidb::detail
