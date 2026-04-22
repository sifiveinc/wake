/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "cas_prim.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <optional>

#include "cas/cas.h"
#include "cas/content_hash.h"
#include "prim.h"
#include "types/data.h"
#include "types/primfn.h"
#include "util/mkdir_parents.h"
#include "value.h"
#include "wcl/file_ops.h"
#include "wcl/filepath.h"

// Counter for unique temp file names during staged materialization.
// PID + counter ensures no collisions across concurrent wake processes.
static std::atomic<uint64_t> g_staged_materialize_counter{0};

namespace {

bool parse_hash_string(const std::string& id, cas::ContentHash& out, std::string& error) {
  if (id.empty()) {
    error = "Invalid CAS hash: " + id;
    return false;
  }

  auto hash_result = cas::ContentHash::from_hex(id);
  if (!hash_result) {
    error = "Invalid CAS hash: " + id;
    return false;
  }
  out = *hash_result;
  return true;
}

// Unique temp path for atomic rename. PID + counter avoids races across concurrent processes.
static std::string make_temp_path(const std::string& dest) {
  return dest + "." + std::to_string(getpid()) + "." +
         std::to_string(g_staged_materialize_counter.fetch_add(1));
}

// Place temp at dest via a single rename().
// Returns nullopt on success, error string on failure. Always cleans up temp on failure.
static std::optional<std::string> atomic_replace(const std::string& temp, const std::string& dest,
                                                 const std::string& label) {
  std::error_code ec;
  std::filesystem::rename(temp, dest, ec);
  if (!ec) return std::nullopt;

  std::filesystem::remove(temp, ec);
  return "Failed to place " + label + " " + dest + ": " + ec.message();
}

// Set mtime on path. Returns false on failure; symlink callers may ignore the return value.
static bool apply_mtime(const std::string& path, time_t sec, long nsec, int flags) {
  if (sec == 0 && nsec == 0) return true;
  struct timespec times[2];
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = sec;
  times[1].tv_nsec = nsec;
  return utimensat(AT_FDCWD, path.c_str(), times, flags) == 0;
}

// mkdir(path, mode). On EEXIST (concurrent process won), fall back to chmod.
// Returns nullopt on success, error string on failure.
static std::optional<std::string> make_dir_or_chmod(const std::string& path, mode_t mode) {
  if (mkdir(path.c_str(), mode) != 0) {
    if (errno == EEXIST) {
      chmod(path.c_str(), mode);
    } else {
      return "Failed to create directory " + path + ": " + strerror(errno);
    }
  }
  return std::nullopt;
}

// Unlink staging_path only if it differs from dest (i.e. it's a temp copy, not the workspace file).
static void cleanup_staging_file(const std::string& staging_path, const std::string& dest_path) {
  if (staging_path != dest_path) {
    if (unlink(staging_path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "Warning: Failed to delete staging file " << staging_path << std::endl;
    }
  }
}

// Ensure parent directories of dest exist at mode 0755.
// Returns nullopt on success, error string on failure.
static std::optional<std::string> ensure_parent_dirs(const std::string& dest) {
  std::filesystem::path parent = std::filesystem::path(dest).parent_path();
  if (!parent.empty() && mkdir_with_parents(parent.string(), 0755) != 0)
    return "Failed to create parent directories for " + dest;
  return std::nullopt;
}

}  // namespace

// prim "cas_dir" -> String
// Returns the CAS root directory as configured in tools/wake/main.cpp.
// Blobs live at {cas_dir}/blobs and staging at {cas_dir}/staging.
static PRIMTYPE(type_cas_dir) { return args.size() == 0 && out->unify(Data::typeString); }

static PRIMFN(prim_cas_dir) {
  EXPECT(0);
  auto* ctx = static_cast<CASContext*>(data);
  RETURN(String::alloc(runtime.heap, ctx->root()));
}

// prim "cas_materialize_item" destPath type hashOrTarget mode mtimeSec mtimeNsec -> Result Unit
// Error Materialize an item from CAS to workspace (files already in CAS) or create
// symlink/directory.
// - type="file": hashOrTarget = file hash, uses mode/mtime
// - type="symlink": hashOrTarget = hash of the symlink target path blob
// - type="directory": hashOrTarget = "" (unused), uses mode
static PRIMTYPE(type_cas_materialize_item) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 6 && args[0]->unify(Data::typeString) &&  // destPath
         args[1]->unify(Data::typeString) &&                      // type
         args[2]->unify(Data::typeString) &&                      // hashOrTarget
         args[3]->unify(Data::typeInteger) &&                     // mode
         args[4]->unify(Data::typeInteger) &&                     // mtimeSec
         args[5]->unify(Data::typeInteger) &&                     // mtimeNsec
         out->unify(result);
}

static PRIMFN(prim_cas_materialize_item) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(6);
  STRING(dest_path, 0);
  STRING(type_str, 1);
  STRING(hash_or_target, 2);
  INTEGER_MPZ(mode_mpz, 3);
  INTEGER_MPZ(mtime_sec_mpz, 4);
  INTEGER_MPZ(mtime_nsec_mpz, 5);

  std::string type = type_str->c_str();
  std::string dest_str = dest_path->c_str();

  // Create parent directories for all types
  if (auto msg = ensure_parent_dirs(dest_str)) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
    auto err = String::claim(runtime.heap, *msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  cas::Cas* store = ctx->get_store();
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  if (type == "file") {
    // Handle regular file: materialize from CAS (already stored by wakebox), apply timestamps
    std::string hash_str_val = hash_or_target->c_str();
    cas::ContentHash hash;
    std::string parse_error;
    if (!parse_hash_string(hash_str_val, hash, parse_error)) {
      std::string msg = parse_error;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));
    time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
    long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

    // Materialize from CAS to workspace with timestamps
    auto mat_result = store->materialize_blob(hash, dest_str.c_str(), mode, mtime_sec, mtime_nsec);
    if (!mat_result) {
      std::string msg = "Failed to materialize blob " + hash_str_val + " to " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "symlink") {
    cas::ContentHash hash;
    std::string parse_error;
    if (!parse_hash_string(hash_or_target->c_str(), hash, parse_error)) {
      runtime.heap.reserve(reserve_result() + String::reserve(parse_error.size()));
      auto err = String::claim(runtime.heap, parse_error);
      RETURN(claim_result(runtime.heap, false, err));
    }

    auto target_result = store->read_blob(hash);
    if (!target_result) {
      std::string msg = "Failed to materialize symlink " + std::string(hash_or_target->c_str()) +
                        " to " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    // Atomically create symlink via temp+rename
    std::string temp_path = make_temp_path(dest_str);
    time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
    long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

    if (symlink(target_result->c_str(), temp_path.c_str()) != 0) {
      std::string msg = "Failed to create symlink " + dest_str + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    (void)apply_mtime(temp_path, mtime_sec, mtime_nsec, AT_SYMLINK_NOFOLLOW);

    if (auto msg = atomic_replace(temp_path, dest_str, "symlink")) {
      runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
      auto err = String::claim(runtime.heap, *msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "directory") {
    // Handle directory: create directory with mode
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

    struct stat st;
    if (stat(dest_str.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        // Directory already exists, just update mode
        chmod(dest_str.c_str(), mode);
      } else {
        // Non-directory at dest — remove it then create directory
        (void)unlink(dest_str.c_str());
        if (auto msg = make_dir_or_chmod(dest_str, mode)) {
          runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
          auto err = String::claim(runtime.heap, *msg);
          RETURN(claim_result(runtime.heap, false, err));
        }
      }
    } else {
      if (auto msg = make_dir_or_chmod(dest_str, mode)) {
        runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
        auto err = String::claim(runtime.heap, *msg);
        RETURN(claim_result(runtime.heap, false, err));
      }
    }

    time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
    long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));
    if (!apply_mtime(dest_str, mtime_sec, mtime_nsec, 0)) {
      std::string msg =
          "Failed to update timestamps for directory " + dest_str + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else {
    std::string msg = "Unknown item type: " + type;
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

// prim "materialize_staged_workspace_item" destPath type stagingPathOrTarget mode mtimeSec
// mtimeNsec -> Result Unit Error Materialize a staged item straight into the workspace without
// hashing it or storing it in CAS.
// - type="file": stagingPathOrTarget = staging path, uses mode/mtime
// - type="symlink": stagingPathOrTarget = symlink target
// - type="directory": stagingPathOrTarget = "" (unused), uses mode
static PRIMTYPE(type_materialize_staged_workspace_item) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 6 && args[0]->unify(Data::typeString) &&  // destPath
         args[1]->unify(Data::typeString) &&                      // type
         args[2]->unify(Data::typeString) &&                      // stagingPathOrTarget
         args[3]->unify(Data::typeInteger) &&                     // mode
         args[4]->unify(Data::typeInteger) &&                     // mtimeSec
         args[5]->unify(Data::typeInteger) &&                     // mtimeNsec
         out->unify(result);
}

static PRIMFN(prim_materialize_staged_workspace_item) {
  EXPECT(6);
  STRING(dest_path, 0);
  STRING(type_str, 1);
  STRING(staging_path_or_target, 2);
  INTEGER_MPZ(mode_mpz, 3);
  INTEGER_MPZ(mtime_sec_mpz, 4);
  INTEGER_MPZ(mtime_nsec_mpz, 5);

  std::string type = type_str->c_str();
  std::string dest_str = dest_path->c_str();
  time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
  long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

  if (auto msg = ensure_parent_dirs(dest_str)) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
    auto err = String::claim(runtime.heap, *msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  if (type == "file") {
    std::string staging_path = staging_path_or_target->c_str();
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));
    std::string temp_path = make_temp_path(dest_str);

    auto copy_result = wcl::reflink_or_copy_file(staging_path, temp_path, mode);
    if (!copy_result) {
      std::error_code ec;
      std::filesystem::remove(temp_path, ec);
      std::string msg = "Failed to materialize staged file " + staging_path + " to " + dest_str +
                        ": " + strerror(copy_result.error());
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    if (!apply_mtime(temp_path, mtime_sec, mtime_nsec, 0)) {
      std::error_code ec;
      std::filesystem::remove(temp_path, ec);
      std::string msg = "Failed to update timestamps for " + dest_str + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    if (auto msg = atomic_replace(temp_path, dest_str, "staged file")) {
      runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
      auto err = String::claim(runtime.heap, *msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    cleanup_staging_file(staging_path, dest_str);

  } else if (type == "symlink") {
    std::string target = staging_path_or_target->c_str();
    std::string temp_path = make_temp_path(dest_str);

    if (symlink(target.c_str(), temp_path.c_str()) != 0) {
      std::string msg =
          "Failed to create symlink " + dest_str + " -> " + target + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    (void)apply_mtime(temp_path, mtime_sec, mtime_nsec, AT_SYMLINK_NOFOLLOW);

    if (auto msg = atomic_replace(temp_path, dest_str, "symlink")) {
      runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
      auto err = String::claim(runtime.heap, *msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "directory") {
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

    struct stat st;
    if (stat(dest_str.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        chmod(dest_str.c_str(), mode & 07777);
      } else {
        (void)unlink(dest_str.c_str());
        if (auto msg = make_dir_or_chmod(dest_str, mode & 07777)) {
          runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
          auto err = String::claim(runtime.heap, *msg);
          RETURN(claim_result(runtime.heap, false, err));
        }
      }
    } else {
      if (auto msg = make_dir_or_chmod(dest_str, mode & 07777)) {
        runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
        auto err = String::claim(runtime.heap, *msg);
        RETURN(claim_result(runtime.heap, false, err));
      }
    }

    if (!apply_mtime(dest_str, mtime_sec, mtime_nsec, 0)) {
      std::string msg =
          "Failed to update timestamps for directory " + dest_str + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else {
    std::string msg = "Unknown staging item type: " + type;
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

// prim "cas_ingest_staged_item" destPath type stagingPathOrTarget hash -> Result Unit Error
// Stores staged content in CAS under its precomputed hash. Does not write to the workspace.
// - type="file": stagingPathOrTarget = staging path
// - type="symlink": stagingPathOrTarget = symlink target
// - type="directory": no-op (directories have no CAS blob)
static PRIMTYPE(type_cas_ingest_staged_item) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 4 && args[0]->unify(Data::typeString) &&  // destPath
         args[1]->unify(Data::typeString) &&                      // type
         args[2]->unify(Data::typeString) &&                      // stagingPathOrTarget
         args[3]->unify(Data::typeString) &&                      // hash
         out->unify(result);
}

static PRIMFN(prim_cas_ingest_staged_item) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(4);
  STRING(dest_path, 0);
  STRING(type_str, 1);
  STRING(staging_path_or_target, 2);
  STRING(hash_str, 3);

  std::string type = type_str->c_str();
  std::string dest_str = dest_path->c_str();

  cas::Cas* store = ctx->get_store();
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  if (type == "file") {
    std::string staging_path = staging_path_or_target->c_str();

    cas::ContentHash expected_hash;
    std::string parse_error;
    if (!parse_hash_string(hash_str->c_str(), expected_hash, parse_error)) {
      std::string msg = parse_error;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    if (access(staging_path.c_str(), F_OK) == 0) {
      auto store_result =
          store->store_blob_from_file_with_hash(staging_path.c_str(), expected_hash);
      if (!store_result) {
        std::string msg = "Failed to store staging file in CAS: " + staging_path;
        runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
        auto err = String::claim(runtime.heap, msg);
        RETURN(claim_result(runtime.heap, false, err));
      }
    } else if (!store->has_blob(expected_hash)) {
      std::string msg = "Missing staging file and CAS blob for " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    cleanup_staging_file(staging_path, dest_str);

  } else if (type == "symlink") {
    // Handle symlink: insert the target bytes into CAS under the precomputed hash.
    std::string target = staging_path_or_target->c_str();
    cas::ContentHash expected_hash;
    std::string parse_error;
    if (!parse_hash_string(hash_str->c_str(), expected_hash, parse_error)) {
      runtime.heap.reserve(reserve_result() + String::reserve(parse_error.size()));
      auto err = String::claim(runtime.heap, parse_error);
      RETURN(claim_result(runtime.heap, false, err));
    }

    auto store_result = store->store_blob_with_hash(target, expected_hash);
    if (!store_result) {
      std::string msg = "Failed to store symlink target in CAS for " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "directory") {
    // Directories do not store a regular blob in CAS, so there is nothing to ingest here.

  } else {
    std::string msg = "Unknown staging item type: " + type;
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

// ============================================================================
// Primitive Registration
// ============================================================================

void prim_register_cas(CASContext* ctx, PrimMap& pmap) {
  prim_register(pmap, "cas_dir", prim_cas_dir, type_cas_dir, PRIM_PURE, ctx);
  prim_register(pmap, "cas_materialize_item", prim_cas_materialize_item, type_cas_materialize_item,
                PRIM_IMPURE, ctx);
  prim_register(pmap, "materialize_staged_workspace_item", prim_materialize_staged_workspace_item,
                type_materialize_staged_workspace_item, PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_ingest_staged_item", prim_cas_ingest_staged_item,
                type_cas_ingest_staged_item, PRIM_IMPURE, ctx);
}
