
#include <sqlite3.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/runtime/schema.h"

static bool exec_sql(sqlite3* db, const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL error: " << (err ? err : "(null)") << std::endl;
    if (err) sqlite3_free(err);
    return false;
  }
  return true;
}

static bool run_wake_schema(sqlite3* db) {
  char* err = nullptr;
  if (sqlite3_exec(db, WAKE_SCHEMA_SQL, nullptr, nullptr, &err) != SQLITE_OK) {
    std::cerr << "Failed to apply WAKE_SCHEMA_SQL: " << (err ? err : "(null)") << std::endl;
    if (err) sqlite3_free(err);
    return false;
  }
  return true;
}

static int get_version(sqlite3* db) {
  int version = 0;
  sqlite3_stmt* stmt = nullptr;

  // Try PRAGMA user_version first (preferred method)
  if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (version > 0) return version;
  }

  // Fallback to schema table for legacy databases
  if (sqlite3_prepare_v2(db, "SELECT max(version) FROM schema;", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  return version;
}

static bool set_version(sqlite3* db, int version) {
  char pragma_buf[64];
  snprintf(pragma_buf, sizeof(pragma_buf), "PRAGMA user_version=%d;", version);

  // Set PRAGMA user_version
  if (sqlite3_exec(db, pragma_buf, nullptr, nullptr, nullptr) != SQLITE_OK) {
    return false;
  }

  // Insert/update schema table entry
  std::string schema_sql =
      "INSERT OR IGNORE INTO schema(version) VALUES(" + std::to_string(version) + ");";
  return sqlite3_exec(db, schema_sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

static bool backup_database(const std::string& db_path) {
  std::string backup_path = db_path + ".backup";
  std::string cmd = "cp '" + db_path + "' '" + backup_path + "'";

  std::cout << "Creating backup: " << backup_path << std::endl;
  return system(cmd.c_str()) == 0;
}

static bool has_column(sqlite3* db, const char* table, const char* column) {
  sqlite3_stmt* stmt = nullptr;
  std::string query = std::string("PRAGMA table_info(") + table + ");";
  bool found = false;

  if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char* column_name = sqlite3_column_text(stmt, 1);
      if (column_name && strcmp(reinterpret_cast<const char*>(column_name), column) == 0) {
        found = true;
        break;
      }
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

struct Migration {
  int from_version;
  int to_version;
  std::function<bool(sqlite3*)> migrate_func;
  std::string description;
};

// Migration registry - define all available migrations
static std::vector<Migration> get_migrations() {
  return {
      // Version 6 -> 7: Add runner_status column to jobs table
      {6, 7,
       [](sqlite3* db) -> bool {
         if (!has_column(db, "jobs", "runner_status")) {
           const char* sql =
               "ALTER TABLE jobs ADD COLUMN runner_status INTEGER NOT NULL DEFAULT 0;";
           return exec_sql(db, sql);
         }
         return true;
       },
       "Add jobs.runner_status column"},

      // Version 7 -> 8: Add partial index on runner_status for non-zero values
      {7, 8,
       [](sqlite3* db) -> bool {
         const char* sql =
             "CREATE INDEX IF NOT EXISTS runner_status_idx "
             "ON jobs(runner_status) WHERE runner_status <> 0;";
         return exec_sql(db, sql);
       },
       "Add runner_status partial index"},

      // Version 8 -> 9: Update locking mode and add busy timeout
      {8, 9,
       [](sqlite3* db) -> bool {
         // PRAGMA changes are applied by WAKE_SCHEMA_SQL after migration
         // No schema modifications needed, just version bump
         return true;
       },
       "Update locking mode to normal and add busy timeout"},

  };
}

// Apply a single migration step from from_version to to_version
static bool apply_migrations(sqlite3* db, int from_version, int to_version) {
  if (to_version != from_version + 1) {
    std::cerr << "apply_migrations expects single-step migration, got " << from_version << " -> "
              << to_version << std::endl;
    return false;
  }

  std::vector<Migration> migrations = get_migrations();

  // Find the specific migration for this version step
  for (const auto& migration : migrations) {
    if (migration.from_version == from_version && migration.to_version == to_version) {
      std::cout << "Applying migration: " << migration.description << std::endl;
      return migration.migrate_func(db);
    }
  }

  std::cerr << "No migration found for " << from_version << " -> " << to_version << std::endl;
  return false;
}

static bool run_integrity_check(sqlite3* db) {
  // Enable foreign keys for complete integrity validation
  if (!exec_sql(db, "PRAGMA foreign_keys=ON;")) return false;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &st, nullptr) != SQLITE_OK)
    return false;
  bool ok = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    ok = (txt && std::string(reinterpret_cast<const char*>(txt)) == "ok");
  }
  sqlite3_finalize(st);
  return ok;
}

static bool migrate_via_copy(sqlite3* old_db, const std::string& db_path, int from_version,
                             int to_version) {
  std::string temp_path = db_path + ".migrated";
  sqlite3* new_db = nullptr;

  // Create temporary database for migration
  if (sqlite3_open(temp_path.c_str(), &new_db) != SQLITE_OK) {
    std::cerr << "Failed to create temp database: " << (new_db ? sqlite3_errmsg(new_db) : "(null)")
              << std::endl;
    if (new_db) sqlite3_close(new_db);
    return false;
  }

  // Clone old database to new using SQLite backup API
  bool clone_success = false;
  sqlite3_backup* backup = sqlite3_backup_init(new_db, "main", old_db, "main");
  if (backup) {
    int result = sqlite3_backup_step(backup, -1);
    clone_success = (result == SQLITE_DONE);
    sqlite3_backup_finish(backup);
  }

  if (!clone_success) {
    std::cerr << "Failed to clone database via backup API." << std::endl;
    sqlite3_close(new_db);
    return false;
  }

  // Apply stepwise migrations on the cloned database
  int current_version = from_version;
  while (current_version < to_version) {
    int next_version = current_version + 1;

    // Acquire write lock to begin transaction for this migration step
    if (!exec_sql(new_db, "BEGIN IMMEDIATE;")) {
      sqlite3_close(new_db);
      return false;
    }

    std::cout << "Migrating " << current_version << " -> " << next_version << "..." << std::endl;

    // Apply the migration
    if (!apply_migrations(new_db, current_version, next_version)) {
      std::cerr << "Migration step failed: " << current_version << " -> " << next_version
                << std::endl;
      exec_sql(new_db, "ROLLBACK;");
      sqlite3_close(new_db);
      return false;
    }

    // Update version stamps
    if (!set_version(new_db, next_version)) {
      std::cerr << "Failed to set version to " << next_version << std::endl;
      exec_sql(new_db, "ROLLBACK;");
      sqlite3_close(new_db);
      return false;
    }

    // Commit this migration step
    if (!exec_sql(new_db, "COMMIT;")) {
      std::cerr << "Failed to commit migration step" << std::endl;
      sqlite3_close(new_db);
      return false;
    }

    current_version = next_version;
  }

  // Apply WAKE_SCHEMA_SQL to ensure all current schema objects exist
  if (!run_wake_schema(new_db)) {
    std::cerr << "Failed to apply WAKE_SCHEMA_SQL after migration" << std::endl;
    sqlite3_close(new_db);
    return false;
  }

  // Validate the migrated database
  if (!run_integrity_check(new_db)) {
    std::cerr << "PRAGMA integrity_check failed on migrated database" << std::endl;
    sqlite3_close(new_db);
    return false;
  }

  sqlite3_close(new_db);

  // Swap the migrated database into place
  std::string move_new_cmd = "mv '" + temp_path + "' '" + db_path + "'";

  if (system(move_new_cmd.c_str()) != 0) {
    std::cerr << "Failed to swap migrated database into place." << std::endl;
    return false;
  }

  return true;
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <wake.db>" << std::endl;
    return 1;
  }

  std::string db_path = argv[1];
  sqlite3* db = nullptr;

  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
    std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
    return 1;
  }

  // Check current and target versions
  int current_version = get_version(db);
  int target_version = std::stoi(SCHEMA_VERSION);

  std::cout << "Database version: " << current_version << std::endl;
  std::cout << "Target version: " << target_version << std::endl;

  if (current_version == target_version) {
    std::cout << "Database is already up to date." << std::endl;
    sqlite3_close(db);
    return 0;
  }

  // Currently cannot migrate wake.db to an older version
  if (current_version > target_version) {
    std::cerr << "Database version (" << current_version
              << ") is newer than this wake version supports (" << target_version << ")."
              << std::endl;
    std::cerr << "Please update wake or use a newer version." << std::endl;
    sqlite3_close(db);
    return 1;
  }

  if (current_version < 6) {
    std::cerr << "Unsupported source version (" << current_version
              << "). This tool only supports migration from version 6 and above." << std::endl;
    sqlite3_close(db);
    return 1;
  }

  // Create backup before migration
  if (!backup_database(db_path)) {
    std::cerr << "Failed to create backup. Aborting migration." << std::endl;
    sqlite3_close(db);
    return 1;
  }

  std::cout << "Migrating database from version " << current_version << " to " << target_version
            << "..." << std::endl;

  // Perform the migration
  if (!migrate_via_copy(db, db_path, current_version, target_version)) {
    std::cerr << "Migration failed." << std::endl;
    sqlite3_close(db);
    return 1;
  }

  std::cout << "Migration completed successfully." << std::endl;
  sqlite3_close(db);
  return 0;
}
