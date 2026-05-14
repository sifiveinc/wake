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

#include <filesystem>
#include <iostream>
#include <optional>

#include "cas/cas.h"
#include "cas/content_hash.h"
#include "cas/materialize.h"
#include "prim.h"
#include "types/data.h"
#include "types/primfn.h"
#include "value.h"
#include "wcl/file_ops.h"
#include "wcl/filepath.h"

namespace {

// Unlink staging_path only if it differs from dest (i.e. it's a temp copy, not the workspace file).
static void cleanup_staging_file(const std::string& staging_path, const std::string& dest_path) {
  if (staging_path != dest_path) {
    if (unlink(staging_path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "Warning: Failed to delete staging file " << staging_path << std::endl;
    }
  }
}

// Place a staged file/symlink/directory directly into the workspace.
// - type="file":      staging_path_or_target = staging path; reflink/copy + mtime + atomic rename
// - type="symlink":   staging_path_or_target = symlink target string
// - type="directory": staging_path_or_target unused
// Returns nullopt on success, error string on failure.
static std::optional<std::string> materialize_staged_item(const std::string& dest_path,
                                                          const std::string& type,
                                                          const std::string& staging_path_or_target,
                                                          mode_t mode, time_t mtime_sec,
                                                          long mtime_nsec) {
  if (auto msg = cas::ensure_parent_dirs(dest_path)) return msg;

  if (type == "file") {
    const std::string& staging_path = staging_path_or_target;
    std::string temp_path = cas::make_temp_path(dest_path);

    auto copy_result = wcl::reflink_or_copy_file(staging_path, temp_path, mode);
    if (!copy_result) {
      std::error_code ec;
      std::filesystem::remove(temp_path, ec);
      return "Failed to materialize staged file " + staging_path + " to " + dest_path + ": " +
             strerror(copy_result.error());
    }

    if (!cas::apply_mtime(temp_path, mtime_sec, mtime_nsec, 0)) {
      std::error_code ec;
      std::filesystem::remove(temp_path, ec);
      return "Failed to update timestamps for " + dest_path + ": " + strerror(errno);
    }

    if (auto msg = cas::atomic_replace(temp_path, dest_path, "staged file")) return msg;

    cleanup_staging_file(staging_path, dest_path);

  } else if (type == "symlink") {
    const std::string& target = staging_path_or_target;
    std::string temp_path = cas::make_temp_path(dest_path);

    if (symlink(target.c_str(), temp_path.c_str()) != 0)
      return "Failed to create symlink " + dest_path + " -> " + target + ": " + strerror(errno);

    (void)cas::apply_mtime(temp_path, mtime_sec, mtime_nsec, AT_SYMLINK_NOFOLLOW);

    if (auto msg = cas::atomic_replace(temp_path, dest_path, "symlink")) return msg;

  } else if (type == "directory") {
    mode_t dir_mode = mode & 07777;
    struct stat st;
    if (stat(dest_path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        chmod(dest_path.c_str(), dir_mode);
      } else {
        (void)unlink(dest_path.c_str());
        if (auto msg = cas::make_dir_or_chmod(dest_path, dir_mode)) return msg;
      }
    } else {
      if (auto msg = cas::make_dir_or_chmod(dest_path, dir_mode)) return msg;
    }

    if (!cas::apply_mtime(dest_path, mtime_sec, mtime_nsec, 0))
      return "Failed to update timestamps for directory " + dest_path + ": " + strerror(errno);

  } else {
    return "Unknown staging item type: " + type;
  }

  return std::nullopt;
}

// Ingest a staged item into CAS under its precomputed hash. Does not write to the workspace.
// - type="file":      staging_path_or_target = staging path
// - type="symlink":   staging_path_or_target = symlink target string
// - type="directory": no-op (directories have no CAS blob)
// Returns nullopt on success, error string on failure.
static std::optional<std::string> ingest_staged_item(cas::Cas& store, const std::string& dest_path,
                                                     const std::string& type,
                                                     const std::string& staging_path_or_target,
                                                     const std::string& hash_str) {
  if (type == "file") {
    const std::string& staging_path = staging_path_or_target;

    auto expected_hash = cas::ContentHash::from_hex(hash_str);
    if (!expected_hash) return "Invalid CAS hash: " + hash_str;

    if (access(staging_path.c_str(), F_OK) == 0) {
      auto store_result =
          store.store_blob_from_file_with_hash(staging_path.c_str(), *expected_hash);
      if (!store_result) return "Failed to store staging file in CAS: " + staging_path;
    } else if (!store.has_blob(*expected_hash)) {
      return "Missing staging file and CAS blob for " + dest_path;
    }

    cleanup_staging_file(staging_path, dest_path);

  } else if (type == "symlink") {
    const std::string& target = staging_path_or_target;
    auto expected_hash = cas::ContentHash::from_hex(hash_str);
    if (!expected_hash) return "Invalid CAS hash: " + hash_str;

    auto store_result = store.store_blob_with_hash(target, *expected_hash);
    if (!store_result) return "Failed to store symlink target in CAS for " + dest_path;

  } else if (type == "directory") {
    // Directories have no CAS blob; nothing to ingest.

  } else {
    return "Unknown staging item type: " + type;
  }

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

  cas::Cas* store = ctx->get_store();
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));
  time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
  long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

  if (auto msg = cas::materialize_item(*store, dest_path->c_str(), type_str->c_str(),
                                       hash_or_target->c_str(), mode, mtime_sec, mtime_nsec)) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
    auto err = String::claim(runtime.heap, *msg);
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

  mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));
  time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
  long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

  if (auto msg =
          materialize_staged_item(dest_path->c_str(), type_str->c_str(),
                                  staging_path_or_target->c_str(), mode, mtime_sec, mtime_nsec)) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
    auto err = String::claim(runtime.heap, *msg);
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

  cas::Cas* store = ctx->get_store();
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  if (auto msg = ingest_staged_item(*store, dest_path->c_str(), type_str->c_str(),
                                    staging_path_or_target->c_str(), hash_str->c_str())) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg->size()));
    auto err = String::claim(runtime.heap, *msg);
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
