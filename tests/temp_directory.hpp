#ifndef MINIDB_TESTS_TEMP_DIRECTORY_HPP
#define MINIDB_TESTS_TEMP_DIRECTORY_HPP

// A scratch directory for tests that touch the filesystem.
//
// Shared by test_storage and test_database so neither has to duplicate it.
// Every path comes from std::filesystem::temp_directory_path(), so tests never
// write into the repository, never touch the user's real database, and never
// depend on a hard-coded path.

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <system_error>

namespace minidb::testing {

/// Creates a unique empty directory and deletes it, with everything in it,
/// when destroyed.
///
/// RAII rather than explicit cleanup: a test that fails an assertion leaves
/// its case early, and a destructor still runs where a cleanup call at the
/// end of the body would have been skipped.
class TempDirectory {
public:
    TempDirectory() {
        // Unique per instance per process. The counter covers several
        // directories in one test binary; the process id keeps two test
        // binaries running in parallel under CTest from colliding.
        static std::atomic<unsigned> counter{0};
        const unsigned ordinal = counter.fetch_add(1);

        path_ = std::filesystem::temp_directory_path() /
                ("minidb-test-" + std::to_string(process_tag()) + "-" + std::to_string(ordinal));

        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// A path inside this directory. The file need not exist.
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    /// Fixed within a process and different between processes, so two test
    /// binaries running at once under CTest cannot pick the same directory.
    /// std::random_device avoids reaching for a platform header to get a
    /// process id.
    static unsigned process_tag() {
        static const unsigned tag = std::random_device{}();
        return tag;
    }

    std::filesystem::path path_;
};

/// Reads a whole file as bytes. Returns an empty string if it cannot be read.
[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    std::string bytes;
    std::array<char, 4096> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    return bytes;
}

/// Writes bytes to a file, replacing it. Returns false on failure.
inline bool write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return static_cast<bool>(out);
}

}  // namespace minidb::testing

#endif  // MINIDB_TESTS_TEMP_DIRECTORY_HPP
