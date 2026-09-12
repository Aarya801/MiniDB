// MiniDB command-line front end.
//
// The CLI is deliberately thin: it owns terminal I/O, the shape of the
// replies, and the decision of when to load and save. Parsing lives in the
// parser, state lives in Database, and bytes on disk are StorageManager's
// business, so the tests exercise the same code paths the user drives.

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "minidb/command_parser.hpp"
#include "minidb/database.hpp"
#include "minidb/result.hpp"
#include "minidb/storage.hpp"
#include "minidb/types.hpp"

namespace {

constexpr std::string_view kProgramName = "minidb";
constexpr std::string_view kPrompt = "MiniDB> ";

void print_usage(std::ostream& out) {
    out << "Usage: " << kProgramName << " [database-file]\n"
        << "\n"
        << "Starts an interactive MiniDB session.\n"
        << "\n"
        << "The database is read at startup and written back when the session\n"
        << "ends normally, through EXIT or end of input.\n"
        << "\n"
        << "  [database-file]  Snapshot to use. Defaults to a file in your\n"
        << "                   user data directory.\n"
        << "  -h, --help       Show this message\n"
        << "      --version    Show the version\n";
}

void print_help(std::ostream& out) {
    out << "Commands:\n"
        << "  SET <key> <value>   Store a value. The value may contain spaces.\n"
        << "  GET <key>           Print a value, or NOT_FOUND.\n"
        << "  DELETE <key>        Remove a key.\n"
        << "  EXISTS <key>        Print true or false.\n"
        << "  KEYS                List every key.\n"
        << "  CLEAR               Remove every key.\n"
        << "  HELP                Show this message.\n"
        << "  EXIT                Save and end the session.\n"
        << "\n"
        << "Command names are case-insensitive; keys and values are not.\n"
        << "Changes are logged and flushed to the OS before being applied.\n";
}

bool is_blank(std::string_view line) noexcept {
    return line.find_first_not_of(" \t\r\n\v\f") == std::string_view::npos;
}

/// Runs one command against the database and writes the reply.
/// Returns false when the session should end.
bool execute(const minidb::Command& command, minidb::Database& database, std::ostream& out) {
    switch (command.type) {
        case minidb::CommandType::Set: {
            const minidb::Result result = database.set(command.key, command.value);
            // A successful set carries no payload, so "OK" is the CLI's word
            // for it rather than something the engine returns.
            out << (result.is_ok() ? "OK" : result.to_display_string()) << "\n";
            break;
        }
        case minidb::CommandType::Get: {
            // On success this prints the value; on failure, NOT_FOUND.
            out << database.get(command.key).to_display_string() << "\n";
            break;
        }
        case minidb::CommandType::Delete: {
            const minidb::Result result = database.remove(command.key);
            out << (result.is_ok() ? "OK" : result.to_display_string()) << "\n";
            break;
        }
        case minidb::CommandType::Exists: {
            out << (database.exists(command.key) ? "true" : "false") << "\n";
            break;
        }
        case minidb::CommandType::Keys: {
            std::vector<minidb::Key> keys = database.keys();
            if (keys.empty()) {
                out << "(empty)\n";
                break;
            }
            // Database::keys() is unordered. Sorting is a presentation choice,
            // so it happens here and costs the engine nothing.
            std::sort(keys.begin(), keys.end());
            for (const minidb::Key& key : keys) {
                out << key << "\n";
            }
            break;
        }
        case minidb::CommandType::Clear: {
            // Clearing is logged before it is applied, so it can fail.
            const minidb::Result result = database.clear();
            out << (result.is_ok() ? "OK" : result.to_display_string()) << "\n";
            break;
        }
        case minidb::CommandType::Help: {
            print_help(out);
            break;
        }
        case minidb::CommandType::Exit: {
            out << "Goodbye!\n";
            return false;
        }
    }
    return true;
}

void run_session(minidb::Database& database) {
    std::string line;
    while (true) {
        std::cout << kPrompt << std::flush;

        // getline fails at end of input -- Ctrl+D, Ctrl+Z, or a piped script
        // running out. That is a normal way to finish, not an error.
        if (!std::getline(std::cin, line)) {
            std::cout << "\nGoodbye!\n";
            return;
        }
        if (is_blank(line)) {
            continue;
        }

        const minidb::ParseOutcome outcome = minidb::parse_command(line);
        if (const minidb::Result* error = std::get_if<minidb::Result>(&outcome)) {
            std::cout << error->to_display_string() << "\n";
            continue;
        }
        if (!execute(std::get<minidb::Command>(outcome), database, std::cout)) {
            return;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Any escaped exception would otherwise terminate without a diagnostic.
    try {
        std::filesystem::path database_file = minidb::default_snapshot_path();

        if (argc > 2) {
            std::cerr << kProgramName << ": too many arguments\n";
            print_usage(std::cerr);
            return 2;
        }
        if (argc == 2) {
            const std::string argument = argv[1];
            if (argument == "-h" || argument == "--help") {
                print_usage(std::cout);
                return 0;
            }
            if (argument == "--version") {
                std::cout << kProgramName << " " << MINIDB_VERSION << "\n";
                return 0;
            }
            database_file = argument;
        }

        minidb::Database database(database_file);

        std::cout << "MiniDB v" << MINIDB_VERSION << "\n"
                  << "Database: " << database.snapshot_path().string() << "\n";

        const bool had_snapshot = database.snapshot_exists();
        const minidb::Result loaded = database.load();
        if (!loaded.is_ok()) {
            // A damaged database is never treated as an empty one. Stopping
            // here leaves the file untouched so it can be inspected or moved
            // aside, instead of being overwritten by an empty snapshot when
            // this session ends.
            std::cerr << kProgramName << ": cannot open database: " << loaded.to_display_string()
                      << "\n"
                      << kProgramName
                      << ": the file has been left unchanged; move it aside to start fresh\n";
            return 1;
        }

        if (database.replayed_operation_count() > 0) {
            std::cout << "Recovered " << database.replayed_operation_count()
                      << " WAL operations.\n";
        }
        if (had_snapshot || database.replayed_operation_count() > 0) {
            std::cout << "Loaded " << database.size() << " entr"
                      << (database.size() == 1 ? "y" : "ies") << ".\n";
        } else {
            std::cout << "New database: nothing saved here yet.\n";
        }
        std::cout << "Type HELP for the command list.\n\n";

        run_session(database);

        const minidb::Result saved = database.save();
        if (!saved.is_ok()) {
            std::cerr << kProgramName << ": failed to save: " << saved.to_display_string() << "\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << kProgramName << ": fatal error: " << error.what() << "\n";
        return 1;
    }
}
