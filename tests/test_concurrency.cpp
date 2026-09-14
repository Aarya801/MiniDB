#include "minidb/database.hpp"
#include "minidb/transaction.hpp"

#include <atomic>
#include <cstddef>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "temp_directory.hpp"
#include "test_framework.hpp"

using minidb::Database;
using minidb::Transaction;
using minidb::testing::TempDirectory;

namespace {

void count_failure(std::atomic<int>& failures) noexcept {
    failures.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

TEST(concurrent_disjoint_writes_preserve_every_entry) {
    constexpr int kThreadCount = 6;
    constexpr int kEntriesPerThread = 80;
    Database database(32);
    std::latch ready(kThreadCount);
    std::latch start(1);
    std::atomic<int> failures{0};
    std::vector<std::jthread> workers;

    for (int worker = 0; worker < kThreadCount; ++worker) {
        workers.emplace_back([&, worker] {
            ready.count_down();
            start.wait();
            try {
                for (int entry = 0; entry < kEntriesPerThread; ++entry) {
                    const std::string key =
                        "worker-" + std::to_string(worker) + "-" + std::to_string(entry);
                    if (!database.set(key, std::to_string(entry)).is_ok()) {
                        count_failure(failures);
                    }
                }
            } catch (...) {
                count_failure(failures);
            }
        });
    }
    ready.wait();
    start.count_down();
    workers.clear();

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(database.size(), static_cast<std::size_t>(kThreadCount * kEntriesPerThread));
    for (int worker = 0; worker < kThreadCount; ++worker) {
        for (int entry = 0; entry < kEntriesPerThread; ++entry) {
            const std::string key =
                "worker-" + std::to_string(worker) + "-" + std::to_string(entry);
            EXPECT_EQ(database.get(key).value(), std::to_string(entry));
        }
    }
}

TEST(concurrent_reads_keep_lru_cache_and_values_consistent) {
    constexpr int kThreadCount = 8;
    constexpr int kKeyCount = 40;
    constexpr int kRounds = 30;
    Database database(7);
    for (int key = 0; key < kKeyCount; ++key) {
        ASSERT_TRUE(
            database.set("key-" + std::to_string(key), "value-" + std::to_string(key)).is_ok());
    }

    std::latch ready(kThreadCount);
    std::latch start(1);
    std::atomic<int> failures{0};
    std::vector<std::jthread> readers;
    for (int reader = 0; reader < kThreadCount; ++reader) {
        readers.emplace_back([&, reader] {
            ready.count_down();
            start.wait();
            try {
                for (int round = 0; round < kRounds; ++round) {
                    for (int offset = 0; offset < kKeyCount; ++offset) {
                        const int key = (offset + reader + round) % kKeyCount;
                        const auto result = database.get("key-" + std::to_string(key));
                        if (!result.is_ok() || result.value() != "value-" + std::to_string(key)) {
                            count_failure(failures);
                        }
                    }
                }
            } catch (...) {
                count_failure(failures);
            }
        });
    }
    ready.wait();
    start.count_down();
    readers.clear();

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(database.cache_size() <= database.cache_capacity());
    EXPECT_EQ(database.size(), static_cast<std::size_t>(kKeyCount));
}

TEST(readers_and_writers_share_one_database_without_torn_values) {
    constexpr int kWriterCount = 3;
    constexpr int kReaderCount = 5;
    constexpr int kIterations = 120;
    Database database(4);
    ASSERT_TRUE(database.set("hot", "writer-initial").is_ok());

    std::latch ready(kWriterCount + kReaderCount);
    std::latch start(1);
    std::atomic<int> failures{0};
    std::vector<std::jthread> workers;
    for (int writer = 0; writer < kWriterCount; ++writer) {
        workers.emplace_back([&, writer] {
            ready.count_down();
            start.wait();
            try {
                for (int iteration = 0; iteration < kIterations; ++iteration) {
                    const std::string value =
                        "writer-" + std::to_string(writer) + "-" + std::to_string(iteration);
                    if (!database.set("hot", value).is_ok()) {
                        count_failure(failures);
                    }
                }
            } catch (...) {
                count_failure(failures);
            }
        });
    }
    for (int reader = 0; reader < kReaderCount; ++reader) {
        workers.emplace_back([&] {
            ready.count_down();
            start.wait();
            try {
                for (int iteration = 0; iteration < kIterations * kWriterCount; ++iteration) {
                    const auto result = database.get("hot");
                    if (!result.is_ok() || !result.value().starts_with("writer-")) {
                        count_failure(failures);
                    }
                }
            } catch (...) {
                count_failure(failures);
            }
        });
    }
    ready.wait();
    start.count_down();
    workers.clear();

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(database.get("hot").value().starts_with("writer-"));
    EXPECT_TRUE(database.cache_size() <= database.cache_capacity());
}

TEST(one_transaction_object_serializes_concurrent_local_changes) {
    constexpr int kThreadCount = 6;
    constexpr int kEntriesPerThread = 30;
    Database database;
    Transaction transaction(database);
    ASSERT_TRUE(transaction.begin().is_ok());

    std::latch ready(kThreadCount);
    std::latch start(1);
    std::atomic<int> failures{0};
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < kThreadCount; ++worker) {
        workers.emplace_back([&, worker] {
            ready.count_down();
            start.wait();
            try {
                for (int entry = 0; entry < kEntriesPerThread; ++entry) {
                    const std::string key =
                        "tx-" + std::to_string(worker) + "-" + std::to_string(entry);
                    if (!transaction.set(key, "value").is_ok()) {
                        count_failure(failures);
                    }
                }
            } catch (...) {
                count_failure(failures);
            }
        });
    }
    ready.wait();
    start.count_down();
    workers.clear();

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(transaction.change_count(),
              static_cast<std::size_t>(kThreadCount * kEntriesPerThread));
    ASSERT_TRUE(transaction.commit().is_ok());
    EXPECT_EQ(database.size(), static_cast<std::size_t>(kThreadCount * kEntriesPerThread));
}

TEST(concurrent_commits_from_independent_transactions_do_not_lose_changes) {
    Database database;
    Transaction first(database);
    Transaction second(database);
    ASSERT_TRUE(first.begin().is_ok());
    ASSERT_TRUE(second.begin().is_ok());
    ASSERT_TRUE(first.set("first", "one").is_ok());
    ASSERT_TRUE(second.set("second", "two").is_ok());

    std::latch ready(2);
    std::latch start(1);
    std::atomic<int> failures{0};
    {
        std::jthread first_commit([&] {
            ready.count_down();
            start.wait();
            try {
                if (!first.commit().is_ok()) {
                    count_failure(failures);
                }
            } catch (...) {
                count_failure(failures);
            }
        });
        std::jthread second_commit([&] {
            ready.count_down();
            start.wait();
            try {
                if (!second.commit().is_ok()) {
                    count_failure(failures);
                }
            } catch (...) {
                count_failure(failures);
            }
        });
        ready.wait();
        start.count_down();
    }

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(database.get("first").value(), std::string("one"));
    EXPECT_EQ(database.get("second").value(), std::string("two"));
}

TEST(concurrent_save_and_writes_recover_the_complete_final_state) {
    constexpr int kWriterCount = 4;
    constexpr int kEntriesPerWriter = 25;
    const TempDirectory directory;
    const auto path = directory.file("database");
    Database database(path, 8);
    ASSERT_TRUE(database.open().is_ok());

    std::latch ready(kWriterCount + 1);
    std::latch start(1);
    std::atomic<int> failures{0};
    std::vector<std::jthread> workers;
    for (int writer = 0; writer < kWriterCount; ++writer) {
        workers.emplace_back([&, writer] {
            ready.count_down();
            start.wait();
            try {
                for (int entry = 0; entry < kEntriesPerWriter; ++entry) {
                    const std::string key =
                        "saved-" + std::to_string(writer) + "-" + std::to_string(entry);
                    if (!database.set(key, "value").is_ok()) {
                        count_failure(failures);
                    }
                }
            } catch (...) {
                count_failure(failures);
            }
        });
    }
    workers.emplace_back([&] {
        ready.count_down();
        start.wait();
        try {
            if (!database.save().is_ok()) {
                count_failure(failures);
            }
        } catch (...) {
            count_failure(failures);
        }
    });

    ready.wait();
    start.count_down();
    workers.clear();
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);

    Database recovered(path);
    ASSERT_TRUE(recovered.open().is_ok());
    EXPECT_EQ(recovered.size(), static_cast<std::size_t>(kWriterCount * kEntriesPerWriter));
    for (int writer = 0; writer < kWriterCount; ++writer) {
        for (int entry = 0; entry < kEntriesPerWriter; ++entry) {
            EXPECT_TRUE(
                recovered.exists("saved-" + std::to_string(writer) + "-" + std::to_string(entry)));
        }
    }
}

MINIDB_TEST_MAIN()
