#include "minidb/database.hpp"
#include "minidb/hash_table.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::nanoseconds;
using StringTable = minidb::HashTable<std::string, std::string>;

constexpr std::size_t kTrialCount = 5;
constexpr std::size_t kCacheWorkingSet = 128;
constexpr std::array<std::size_t, 3> kOperationCounts{1'000, 10'000, 100'000};

struct DataSet {
    std::vector<std::string> keys;
    std::vector<std::string> values;
};

struct Sample {
    Duration elapsed;
    std::size_t check;
};

struct Measurement {
    std::string_view workload;
    std::size_t operation_count;
    Duration median_elapsed;
    std::size_t check;
};

[[nodiscard]] DataSet make_data(std::size_t count) {
    DataSet data;
    data.keys.reserve(count);
    data.values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        data.keys.push_back("key-" + std::to_string(index));
        data.values.push_back("value-" + std::to_string(index));
    }
    return data;
}

void require_ok(const minidb::Result& result, std::string_view operation) {
    if (!result.is_ok()) {
        throw std::runtime_error(std::string(operation) + " failed: " + result.to_display_string());
    }
}

template<typename Function>
[[nodiscard]] Sample time_work(Function&& function) {
    const auto start = Clock::now();
    const std::size_t check = std::forward<Function>(function)();
    const auto finish = Clock::now();
    return {std::chrono::duration_cast<Duration>(finish - start), check};
}

template<typename Trial>
[[nodiscard]] Measurement measure(std::string_view name, std::size_t operation_count,
                                  Trial&& trial) {
    std::array<Duration, kTrialCount> elapsed{};
    std::size_t combined_check = 0;
    for (std::size_t index = 0; index < kTrialCount; ++index) {
        const Sample sample = trial();
        elapsed[index] = sample.elapsed;
        combined_check ^= sample.check + index;
    }
    std::sort(elapsed.begin(), elapsed.end());
    return {name, operation_count, elapsed[kTrialCount / 2], combined_check};
}

[[nodiscard]] Sample benchmark_set(const DataSet& data) {
    minidb::Database database(0);
    return time_work([&] {
        for (std::size_t index = 0; index < data.keys.size(); ++index) {
            require_ok(database.set(data.keys[index], data.values[index]), "SET");
        }
        return database.size();
    });
}

[[nodiscard]] Sample benchmark_get(const DataSet& data) {
    minidb::Database database(0);
    for (std::size_t index = 0; index < data.keys.size(); ++index) {
        require_ok(database.set(data.keys[index], data.values[index]), "GET setup SET");
    }
    return time_work([&] {
        std::size_t bytes_read = 0;
        for (const std::string& key : data.keys) {
            const minidb::Result result = database.get(key);
            require_ok(result, "GET");
            bytes_read += result.value().size();
        }
        return bytes_read;
    });
}

[[nodiscard]] Sample benchmark_delete(const DataSet& data) {
    minidb::Database database(0);
    for (std::size_t index = 0; index < data.keys.size(); ++index) {
        require_ok(database.set(data.keys[index], data.values[index]), "DELETE setup SET");
    }
    return time_work([&] {
        for (const std::string& key : data.keys) {
            require_ok(database.remove(key), "DELETE");
        }
        return data.keys.size() - database.size();
    });
}

[[nodiscard]] Sample benchmark_mixed(const DataSet& data) {
    minidb::Database database(0);
    return time_work([&] {
        std::size_t bytes_read = 0;
        for (std::size_t operation = 0; operation < data.keys.size(); ++operation) {
            const std::size_t key_index = operation / 2;
            if (operation % 2 == 0) {
                require_ok(database.set(data.keys[key_index], data.values[key_index]), "mixed SET");
            } else {
                const minidb::Result result = database.get(data.keys[key_index]);
                require_ok(result, "mixed GET");
                bytes_read += result.value().size();
            }
        }
        return bytes_read + database.size();
    });
}

[[nodiscard]] Sample benchmark_cache_hits(std::size_t operation_count, const DataSet& hot_data) {
    minidb::Database database(kCacheWorkingSet);
    for (std::size_t index = 0; index < hot_data.keys.size(); ++index) {
        require_ok(database.set(hot_data.keys[index], hot_data.values[index]), "cache setup SET");
    }
    for (const std::string& key : hot_data.keys) {
        require_ok(database.get(key), "cache warm GET");
    }

    return time_work([&] {
        std::size_t bytes_read = 0;
        for (std::size_t operation = 0; operation < operation_count; ++operation) {
            const minidb::Result result =
                database.get(hot_data.keys[operation % hot_data.keys.size()]);
            require_ok(result, "cache-hit GET");
            bytes_read += result.value().size();
        }
        return bytes_read + database.cache_size();
    });
}

[[nodiscard]] Sample benchmark_hash_table_insert(const DataSet& data) {
    StringTable table;
    return time_work([&] {
        std::size_t inserted = 0;
        for (std::size_t index = 0; index < data.keys.size(); ++index) {
            inserted += table.insert_or_assign(data.keys[index], data.values[index]) ? 1U : 0U;
        }
        return inserted + table.size();
    });
}

[[nodiscard]] Sample benchmark_unordered_map_insert(const DataSet& data) {
    std::unordered_map<std::string, std::string> table;
    return time_work([&] {
        std::size_t inserted = 0;
        for (std::size_t index = 0; index < data.keys.size(); ++index) {
            inserted +=
                table.insert_or_assign(data.keys[index], data.values[index]).second ? 1U : 0U;
        }
        return inserted + table.size();
    });
}

[[nodiscard]] Sample benchmark_hash_table_get(const DataSet& data) {
    StringTable table;
    for (std::size_t index = 0; index < data.keys.size(); ++index) {
        table.insert_or_assign(data.keys[index], data.values[index]);
    }
    return time_work([&] {
        std::size_t bytes_read = 0;
        for (const std::string& key : data.keys) {
            const std::string* value = table.find(key);
            if (value == nullptr) {
                throw std::runtime_error("HashTable lookup failed");
            }
            bytes_read += value->size();
        }
        return bytes_read;
    });
}

[[nodiscard]] Sample benchmark_unordered_map_get(const DataSet& data) {
    std::unordered_map<std::string, std::string> table;
    for (std::size_t index = 0; index < data.keys.size(); ++index) {
        table.insert_or_assign(data.keys[index], data.values[index]);
    }
    return time_work([&] {
        std::size_t bytes_read = 0;
        for (const std::string& key : data.keys) {
            const auto found = table.find(key);
            if (found == table.end()) {
                throw std::runtime_error("unordered_map lookup failed");
            }
            bytes_read += found->second.size();
        }
        return bytes_read;
    });
}

void print_header() {
    std::cout << "MiniDB benchmark\n"
              << "clock=std::chrono::steady_clock, trials=" << kTrialCount
              << ", reported=median, persistence=disabled\n\n"
              << std::left << std::setw(28) << "workload" << std::right << std::setw(12)
              << "operations" << std::setw(16) << "elapsed ms" << std::setw(18) << "ops/sec"
              << std::setw(16) << "avg ns/op" << '\n'
              << std::string(90, '-') << '\n';
}

void print_measurement(const Measurement& measurement) {
    const double nanoseconds = static_cast<double>(measurement.median_elapsed.count());
    const double operations = static_cast<double>(measurement.operation_count);
    const double elapsed_ms = nanoseconds / 1'000'000.0;
    const double operations_per_second =
        nanoseconds > 0.0 ? operations * 1'000'000'000.0 / nanoseconds : 0.0;
    const double average_nanoseconds = nanoseconds / operations;

    std::cout << std::left << std::setw(28) << measurement.workload << std::right << std::setw(12)
              << measurement.operation_count << std::fixed << std::setprecision(3) << std::setw(16)
              << elapsed_ms << std::setprecision(0) << std::setw(18) << operations_per_second
              << std::setprecision(1) << std::setw(16) << average_nanoseconds << '\n';

    // Keep each trial's observed data live through output without adding a
    // noisy checksum column to the result table.
    if (measurement.check == static_cast<std::size_t>(-1)) {
        std::cerr << "unreachable check value\n";
    }
}

}  // namespace

int main() {
    try {
        print_header();
        const DataSet hot_data = make_data(kCacheWorkingSet);
        for (const std::size_t operation_count : kOperationCounts) {
            const DataSet data = make_data(operation_count);
            print_measurement(
                measure("Database SET", operation_count, [&] { return benchmark_set(data); }));
            print_measurement(measure("Database GET (no cache)", operation_count,
                                      [&] { return benchmark_get(data); }));
            print_measurement(measure("Database DELETE", operation_count,
                                      [&] { return benchmark_delete(data); }));
            print_measurement(measure("Database mixed SET/GET", operation_count,
                                      [&] { return benchmark_mixed(data); }));
            print_measurement(measure("Database GET (cache hit)", operation_count, [&] {
                return benchmark_cache_hits(operation_count, hot_data);
            }));
            print_measurement(measure("HashTable insert", operation_count,
                                      [&] { return benchmark_hash_table_insert(data); }));
            print_measurement(measure("unordered_map insert", operation_count,
                                      [&] { return benchmark_unordered_map_insert(data); }));
            print_measurement(measure("HashTable lookup", operation_count,
                                      [&] { return benchmark_hash_table_get(data); }));
            print_measurement(measure("unordered_map lookup", operation_count,
                                      [&] { return benchmark_unordered_map_get(data); }));
            std::cout << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
