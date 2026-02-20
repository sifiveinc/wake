
#include <sqlite3.h>
#include <unistd.h>

#include <cerrno>
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

// Returns true only if checkpoint fully completed.
static bool checkpoint_wal(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, "PRAGMA wal_checkpoint(TRUNCATE);", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    std::cerr << "Failed to prepare checkpoint statement: " << sqlite3_errmsg(db) << std::endl;
    return false;
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    std::cerr << "Checkpoint did not return result row." << std::endl;
    sqlite3_finalize(stmt);
    return false;
  }

  // busy, log, checkpointed
  int busy = sqlite3_column_int(stmt, 0);
  int log = sqlite3_column_int(stmt, 1);
  int checkpointed = sqlite3_column_int(stmt, 2);
  sqlite3_finalize(stmt);

  if (busy != 0) {
    std::cerr << "Checkpoint blocked by concurrent access (busy=" << busy << ")." << std::endl;
    return false;
  }

  if (log != checkpointed) {
    std::cerr << "Checkpoint incomplete: " << checkpointed << " of " << log << " frames."
              << std::endl;
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

static const char* aux_suffixes[] = {"-wal", "-shm", "-journal"};

// Remove auxiliary files (-wal, -shm, -journal).
static void unlink_aux(const std::string& db_path) {
  for (const char* suffix : aux_suffixes) unlink((db_path + suffix).c_str());
}

// Remove db and auxiliary files.
static void remove_with_aux(const std::string& db_path) {
  unlink(db_path.c_str());
  unlink_aux(db_path);
}

// Move db and auxiliary files (-wal, -shm, -journal) to backup.
static bool move_to_backup(const std::string& db_path) {
  std::string backup_path = db_path + ".backup";

  // Move main database file (must succeed)
  std::cout << "Moving to backup: " << backup_path << std::endl;
  if (rename(db_path.c_str(), backup_path.c_str()) != 0) {
    std::cerr << "Failed to move " << db_path << ": " << strerror(errno) << std::endl;
    return false;
  }

  for (const char* suffix : aux_suffixes) {
    std::string aux_path = db_path + suffix;
    std::string aux_backup = backup_path + suffix;
    if (rename(aux_path.c_str(), aux_backup.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "Warning: failed to move " << aux_path << ": " << strerror(errno) << std::endl;
    }
  }

  return true;
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

      // Version 8 -> 9: Change runner_status from INTEGER to TEXT (nullable)
      // Additionally updates locking mode and adds busy timeout, both in metadata
      {8, 9,
       [](sqlite3* db) -> bool {
         // SQLite doesn't support ALTER COLUMN, so we need to recreate the table

         // Step 1: Create new jobs table with TEXT runner_status
         const char* create_new_table = R"(
           CREATE TABLE jobs_new(
             job_id      integer primary key autoincrement,
             run_id      integer not null references runs(run_id),
             use_id      integer not null references runs(run_id),
             label       text    not null,
             directory   text    not null,
             commandline blob    not null,
             environment blob    not null,
             stdin       text    not null,
             signature   integer not null,
             stack       blob    not null,
             stat_id     integer references stats(stat_id),
             starttime   integer not null default 0,
             endtime     integer not null default 0,
             keep        integer not null default 0,
             stale       integer not null default 0,
             is_atty     integer not null default 0,
             runner_status text
           );
         )";

         if (!exec_sql(db, create_new_table)) return false;

         // Step 2: Copy data, converting integer runner_status to text
         // 0 -> NULL (success), non-zero -> string representation (failure)
         const char* copy_data = R"(
           INSERT INTO jobs_new SELECT
             job_id, run_id, use_id, label, directory, commandline, environment,
             stdin, signature, stack, stat_id, starttime, endtime, keep, stale, is_atty,
             CASE WHEN runner_status = 0 THEN NULL ELSE 'Numeric return code ' || CAST(runner_status AS TEXT) END
           FROM jobs;
         )";

         if (!exec_sql(db, copy_data)) return false;

         // Step 3: Drop old table and rename new one
         if (!exec_sql(db, "DROP TABLE jobs;")) return false;
         if (!exec_sql(db, "ALTER TABLE jobs_new RENAME TO jobs;")) return false;

         // Step 4: Recreate indexes
         if (!exec_sql(db,
                       "CREATE INDEX job on jobs(directory, commandline, environment, stdin, "
                       "signature, keep, job_id, stat_id);"))
           return false;
         if (!exec_sql(db,
                       "CREATE INDEX runner_status_idx on jobs(runner_status) WHERE runner_status "
                       "IS NOT NULL;"))
           return false;
         if (!exec_sql(db, "CREATE INDEX jobstats on jobs(stat_id);")) return false;

         return true;
       },
       "Convert runner_status from INTEGER to TEXT"},

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
      std::cerr << "Failed to commit migration step." << std::endl;
      sqlite3_close(new_db);
      return false;
    }

    current_version = next_version;
  }

  // Apply WAKE_SCHEMA_SQL to ensure all current schema objects exist
  if (!run_wake_schema(new_db)) {
    std::cerr << "Failed to apply WAKE_SCHEMA_SQL after migration." << std::endl;
    sqlite3_close(new_db);
    return false;
  }

  // Validate the migrated database
  if (!run_integrity_check(new_db)) {
    std::cerr << "PRAGMA integrity_check failed on migrated database." << std::endl;
    sqlite3_close(new_db);
    return false;
  }

  // Checkpoint before close to flush WAL to main file
  if (!checkpoint_wal(new_db)) {
    std::cerr << "Checkpoint of migrated database failed." << std::endl;
    sqlite3_close(new_db);
    return false;
  }

  sqlite3_close(new_db);

  // Clean up migrated auxiliary files (safe after checkpoint)
  unlink_aux(temp_path);

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

  // Prevent concurrent access during migration.
  if (!exec_sql(db, "PRAGMA locking_mode=EXCLUSIVE;")) {
    std::cerr << "Failed to set exclusive locking mode." << std::endl;
    sqlite3_close(db);
    return 1;
  }
  // Checkpoint WAL before backup.
  if (!checkpoint_wal(db)) {
    sqlite3_close(db);
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

  std::cout << "Migrating database from version " << current_version << " to " << target_version
            << "..." << std::endl;
  std::cout << "Do not start wake until migration completes." << std::endl;

  // Create migrated copy (while original db still at its path)
  std::string migrated_path = db_path + ".migrated";
  if (!migrate_via_copy(db, db_path, current_version, target_version)) {
    std::cerr << "Migration failed." << std::endl;
    remove_with_aux(migrated_path);
    sqlite3_close(db);
    return 1;
  }

  sqlite3_close(db);

  // Move old database (and auxiliary files) to backup
  if (!move_to_backup(db_path)) {
    std::cerr << "Failed to move database to backup. Aborting migration." << std::endl;
    remove_with_aux(migrated_path);
    return 1;
  }

  // Move migrated database into place
  if (rename(migrated_path.c_str(), db_path.c_str()) != 0) {
    std::cerr << "Failed to move migrated database into place: " << strerror(errno) << std::endl;
    std::cerr << "Recovery: mv '" << db_path << ".backup' '" << db_path << "'" << std::endl;
    return 1;
  }

  std::cout << "Migration completed successfully." << std::endl;
  return 0;
}
