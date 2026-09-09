// MiniDB command-line front end.
//
// The CLI is deliberately thin: it owns terminal I/O and the shape of the
// replies, and nothing else. Parsing lives in the parser and all state lives
// in Database, so the tests exercise the same code paths the user drives.

#include <algorithm>
#include <exception>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "minidb/command_parser.hpp"
#include "minidb/database.hpp"
#include "minidb/result.hpp"
#include "minidb/types.hpp"

namespace {

constexpr std::string_view kProgramName = "minidb";
constexpr std::string_view kPrompt = "MiniDB> ";

void print_usage(std::ostream& out) {
    out << "Usage: " << kProgramName << "\n"
        << "\n"
        << "Starts an interactive MiniDB session. All data is held in memory\n"
        << "and is lost when the session ends.\n"
        << "\n"
        << "  -h, --help     Show this message\n"
        << "      --version  Show the version\n";
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
        << "  EXIT                End the session.\n"
        << "\n"
        << "Command names are case-insensitive; keys and values are not.\n";
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
            database.clear();
            out << "OK\n";
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
        if (argc > 1) {
            const std::string argument = argv[1];
            if (argument == "-h" || argument == "--help") {
                print_usage(std::cout);
                return 0;
            }
            if (argument == "--version") {
                std::cout << kProgramName << " " << MINIDB_VERSION << "\n";
                return 0;
            }
            std::cerr << kProgramName << ": unexpected argument '" << argument << "'\n";
            print_usage(std::cerr);
            return 2;
        }

        std::cout << "MiniDB v" << MINIDB_VERSION << "\n"
                  << "In-memory only: nothing is written to disk yet.\n"
                  << "Type HELP for the command list.\n\n";

        minidb::Database database;
        run_session(database);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << kProgramName << ": fatal error: " << error.what() << "\n";
        return 1;
    }
}
