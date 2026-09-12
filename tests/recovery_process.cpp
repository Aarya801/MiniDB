// Subprocess fixture: _Exit intentionally skips destructors and normal CLI save.
#include "minidb/database.hpp"
#include "minidb/storage.hpp"

#include <cstdlib>
#include <filesystem>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    const std::string_view mode(argv[1]);
    minidb::Database db{std::filesystem::path(argv[2])};
    if (!db.open().is_ok()) {
        return 3;
    }
    if (mode == "verify-dirty" || mode == "verify-saved") {
        if (db.size() != 2 || db.exists("a") || !db.get("b").is_ok() || !db.get("c").is_ok() ||
            db.get("b").value() != "2" || db.get("c").value() != "3") {
            return 4;
        }
        const std::size_t expected = mode == "verify-dirty" ? 2 : 0;
        return db.replayed_operation_count() == expected ? 0 : 5;
    }
    if (!db.set("a", "1").is_ok() || !db.set("b", "2").is_ok() || !db.save().is_ok() ||
        !db.remove("a").is_ok() || !db.set("c", "3").is_ok()) {
        return 6;
    }
    if (mode == "installed") {
        if (!minidb::StorageManager(db.snapshot_path()).save({{"b", "2"}, {"c", "3"}}, 4).is_ok()) {
            return 7;
        }
    } else if (mode == "reset") {
        if (!db.save().is_ok()) {
            return 8;
        }
    } else if (mode != "dirty") {
        return 9;
    }
    std::_Exit(0);
}
