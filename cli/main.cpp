// MiniDB command-line front end.
//
// The CLI is deliberately thin: it owns argument handling and terminal I/O,
// and nothing else. All database behaviour lives in minidb_core so that the
// tests exercise the same code path the user does.

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kProgramName = "minidb";

void print_usage(std::ostream& out) {
    out << "Usage: " << kProgramName << " <data-directory>\n"
        << "\n"
        << "  <data-directory>  Directory holding the database files.\n"
        << "                    Created if it does not already exist.\n";
}

void print_banner(const std::filesystem::path& data_directory) {
    std::cout << "MiniDB v" << MINIDB_VERSION << "\n"
              << "Database: " << data_directory.string() << "\n";
}

/// Resolve and prepare the data directory.
/// Returns an empty path on failure, after reporting the reason.
std::filesystem::path prepare_data_directory(const std::string& argument) {
    std::error_code ec;
    std::filesystem::path directory = std::filesystem::absolute(argument, ec);
    if (ec) {
        std::cerr << kProgramName << ": cannot resolve '" << argument << "': " << ec.message()
                  << "\n";
        return {};
    }

    // create_directories succeeds silently if the directory already exists.
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        std::cerr << kProgramName << ": cannot create '" << directory.string()
                  << "': " << ec.message() << "\n";
        return {};
    }

    if (!std::filesystem::is_directory(directory)) {
        std::cerr << kProgramName << ": '" << directory.string() << "' is not a directory\n";
        return {};
    }

    return directory;
}

}  // namespace

int main(int argc, char** argv) {
    // Any escaped exception would otherwise terminate without a diagnostic.
    try {
        if (argc != 2) {
            print_usage(std::cerr);
            return 2;
        }

        const std::string argument = argv[1];
        if (argument == "-h" || argument == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (argument == "--version") {
            std::cout << kProgramName << " " << MINIDB_VERSION << "\n";
            return 0;
        }

        const std::filesystem::path data_directory = prepare_data_directory(argument);
        if (data_directory.empty()) {
            return 1;
        }

        print_banner(data_directory);
        std::cout << "The command interpreter is not implemented yet.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << kProgramName << ": fatal error: " << error.what() << "\n";
        return 1;
    }
}
