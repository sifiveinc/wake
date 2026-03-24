/*
 * Copyright 2024 SiFive, Inc.
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

#include "cas/cas.h"
#include "cas/content_hash.h"
#include "prim.h"
#include "types/data.h"
#include "types/primfn.h"
#include "util/mkdir_parents.h"
#include "value.h"
#include "wcl/filepath.h"

// ============================================================================
// CASContext implementation
// ============================================================================

cas::Cas* CASContext::get_store(const std::string& workspace) {
  if (store_ && workspace_ == workspace) {
    return store_.get();
  }

  std::string cas_root = workspace + "/.cas";
  auto store_result = cas::Cas::open(cas_root);
  if (store_result) {
    store_ = std::make_unique<cas::Cas>(std::move(*store_result));
    workspace_ = workspace;
    return store_.get();
  }

  return nullptr;
}

// ============================================================================
// CAS Primitives
// ============================================================================

namespace {

bool parse_hash_string(const std::string& id, cas::ContentHash& out, std::string& error) {
  if (id.empty() || cas::is_directory_hash_sentinel(id)) {
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

wcl::result<std::string, std::string> read_symlink_target(const std::string& path) {
  std::string buffer(8192, '\0');

  while (true) {
    ssize_t bytes_read = readlink(path.c_str(), buffer.data(), buffer.size());
    if (bytes_read < 0) {
      return wcl::make_error<std::string, std::string>("readlink(" + path +
                                                       "): " + strerror(errno));
    }
    if (static_cast<size_t>(bytes_read) < buffer.size()) {
      buffer.resize(bytes_read);
      return wcl::make_result<std::string, std::string>(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
}

}  // namespace

// prim "cas_ingest_workspace_path" path: String -> type: String -> hash: String -> Result Unit
// Error Ingests an already-hashed workspace path into CAS without recomputing its digest. The path
// already exists in the workspace, so this does not materialize anything back to it.
static PRIMTYPE(type_cas_ingest_workspace_path) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);
  return args.size() == 3 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_cas_ingest_workspace_path) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(3);
  STRING(path, 0);
  STRING(type_str, 1);
  STRING(hash_str, 2);

  std::string type = type_str->c_str();
  if (type == "directory") {
    runtime.heap.reserve(reserve_result() + reserve_unit());
    RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
  }

  cas::Cas* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  cas::ContentHash expected_hash;
  std::string parse_error;
  if (!parse_hash_string(hash_str->c_str(), expected_hash, parse_error)) {
    runtime.heap.reserve(reserve_result() + String::reserve(parse_error.size()));
    auto err = String::claim(runtime.heap, parse_error);
    RETURN(claim_result(runtime.heap, false, err));
  }

  wcl::result<bool, cas::CASError> store_result = wcl::make_result<bool, cas::CASError>(true);
  if (type == "file") {
    store_result = store->store_blob_from_file_with_hash(path->c_str(), expected_hash);
  } else if (type == "symlink") {
    auto target_result = read_symlink_target(path->c_str());
    if (!target_result) {
      std::string msg = "Failed to read symlink target for " + std::string(path->c_str());
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }
    store_result = store->store_blob_with_hash(*target_result, expected_hash);
  } else {
    std::string msg = "Unsupported path type for CAS ensure: " + type;
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  if (!store_result) {
    std::string msg = "Failed to store " + std::string(path->c_str()) + " in CAS";
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

// prim "cas_has_blob" hash: String -> Boolean
// Checks if a CAS blob exists in the store.
static PRIMTYPE(type_cas_has_blob) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeBoolean);
}

static PRIMFN(prim_cas_has_blob) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(1);
  STRING(hash_str, 0);

  cas::Cas* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_bool());
    RETURN(claim_bool(runtime.heap, false));
  }

  cas::ContentHash hash;
  std::string parse_error;
  if (!parse_hash_string(hash_str->c_str(), hash, parse_error)) {
    runtime.heap.reserve(reserve_bool());
    RETURN(claim_bool(runtime.heap, false));
  }
  bool exists = store->has_blob(hash);

  runtime.heap.reserve(reserve_bool());
  RETURN(claim_bool(runtime.heap, exists));
}

// prim "cas_materialize_file" hash: String -> destPath: String -> mode: Integer -> Result Unit
// Error Materializes a file blob from CAS to the filesystem.
static PRIMTYPE(type_cas_materialize_file) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 3 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeInteger) && out->unify(result);
}

static PRIMFN(prim_cas_materialize_file) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(3);
  STRING(hash_str, 0);
  STRING(dest_path, 1);
  INTEGER_MPZ(mode_mpz, 2);

  cas::Cas* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  cas::ContentHash hash;
  std::string parse_error;
  if (!parse_hash_string(hash_str->c_str(), hash, parse_error)) {
    runtime.heap.reserve(reserve_result() + String::reserve(parse_error.size()));
    auto err = String::claim(runtime.heap, parse_error);
    RETURN(claim_result(runtime.heap, false, err));
  }
  mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

  // Use 0 for mtime to let the system use current time
  auto result = store->materialize_blob(hash, dest_path->c_str(), mode, 0, 0);
  if (!result) {
    runtime.heap.reserve(reserve_result() + String::reserve(35));
    auto err = String::claim(runtime.heap, "Failed to materialize file from CAS");
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
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
  std::filesystem::path parent_dir = std::filesystem::path(dest_str).parent_path();
  if (!parent_dir.empty()) {
    if (mkdir_with_parents(parent_dir.string(), 0755) != 0) {
      std::string msg = "Failed to create parent directories for " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }
  }

  if (type == "file") {
    // Handle regular file: materialize from CAS (already stored by wakebox), apply timestamps
    cas::Cas* store = ctx->get_store(".");
    if (!store) {
      runtime.heap.reserve(reserve_result() + String::reserve(28));
      auto err = String::claim(runtime.heap, "CAS store not initialized");
      RETURN(claim_result(runtime.heap, false, err));
    }

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
    cas::Cas* store = ctx->get_store(".");
    if (!store) {
      runtime.heap.reserve(reserve_result() + String::reserve(28));
      auto err = String::claim(runtime.heap, "CAS store not initialized");
      RETURN(claim_result(runtime.heap, false, err));
    }

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

    (void)unlink(dest_str.c_str());
    if (symlink(target_result->c_str(), dest_str.c_str()) != 0) {
      std::string msg = "Failed to create symlink " + dest_str + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "directory") {
    // Handle directory: create directory with mode
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

    // Remove existing file if present (but not directory)
    struct stat st;
    if (stat(dest_str.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        // Directory already exists, just update mode
        chmod(dest_str.c_str(), mode);
      } else {
        // It's a file, remove it and create directory
        (void)unlink(dest_str.c_str());
        if (mkdir(dest_str.c_str(), mode) != 0) {
          std::string msg = "Failed to create directory " + dest_str + ": " + strerror(errno);
          runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
          auto err = String::claim(runtime.heap, msg);
          RETURN(claim_result(runtime.heap, false, err));
        }
      }
    } else {
      // Directory doesn't exist, create it
      if (mkdir(dest_str.c_str(), mode) != 0) {
        std::string msg = "Failed to create directory " + dest_str + ": " + strerror(errno);
        runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
        auto err = String::claim(runtime.heap, msg);
        RETURN(claim_result(runtime.heap, false, err));
      }
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

// prim "cas_ingest_staged_item" destPath type stagingPathOrTarget hash mode mtimeSec mtimeNsec ->
// Result Unit Error Unified output ingestion step after hashing has already happened in Wake. This
// step also materializes or creates the final workspace item.
// - type="file": stagingPathOrTarget = staging path, uses precomputed hash/mode/mtime
// - type="symlink": stagingPathOrTarget = symlink target, uses precomputed hash
// - type="directory": stagingPathOrTarget = "" (unused), uses mode
static PRIMTYPE(type_cas_ingest_staged_item) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 7 && args[0]->unify(Data::typeString) &&  // destPath
         args[1]->unify(Data::typeString) &&                      // type
         args[2]->unify(Data::typeString) &&                      // stagingPathOrTarget
         args[3]->unify(Data::typeString) &&                      // hash
         args[4]->unify(Data::typeInteger) &&                     // mode
         args[5]->unify(Data::typeInteger) &&                     // mtimeSec
         args[6]->unify(Data::typeInteger) &&                     // mtimeNsec
         out->unify(result);
}

static PRIMFN(prim_cas_ingest_staged_item) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(7);
  STRING(dest_path, 0);
  STRING(type_str, 1);
  STRING(staging_path_or_target, 2);
  STRING(hash_str, 3);
  INTEGER_MPZ(mode_mpz, 4);
  INTEGER_MPZ(mtime_sec_mpz, 5);
  INTEGER_MPZ(mtime_nsec_mpz, 6);

  std::string type = type_str->c_str();
  std::string dest_str = dest_path->c_str();

  // Create parent directories for all types
  std::filesystem::path parent_dir = std::filesystem::path(dest_str).parent_path();
  if (!parent_dir.empty()) {
    if (mkdir_with_parents(parent_dir.string(), 0755) != 0) {
      std::string msg = "Failed to create parent directories for " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }
  }

  if (type == "file") {
    // Handle regular file: insert into CAS under the precomputed hash, then materialize it.
    cas::Cas* store = ctx->get_store(".");
    if (!store) {
      runtime.heap.reserve(reserve_result() + String::reserve(28));
      auto err = String::claim(runtime.heap, "CAS store not initialized");
      RETURN(claim_result(runtime.heap, false, err));
    }

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

    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));
    time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
    long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

    auto mat_result =
        store->materialize_blob(expected_hash, dest_str.c_str(), mode, mtime_sec, mtime_nsec);
    if (!mat_result) {
      std::string msg = "Failed to materialize blob " + expected_hash.to_hex() + " to " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    if (unlink(staging_path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "Warning: Failed to delete staging file " << staging_path << std::endl;
    }

  } else if (type == "symlink") {
    // Handle symlink: insert the target bytes into CAS under the precomputed hash, then
    // materialize.
    cas::Cas* store = ctx->get_store(".");
    if (!store) {
      runtime.heap.reserve(reserve_result() + String::reserve(28));
      auto err = String::claim(runtime.heap, "CAS store not initialized");
      RETURN(claim_result(runtime.heap, false, err));
    }

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

    (void)unlink(dest_str.c_str());
    if (symlink(target.c_str(), dest_str.c_str()) != 0) {
      std::string msg =
          "Failed to create symlink " + dest_str + " -> " + target + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "directory") {
    // Handle directory: create directory with mode
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

    // Remove existing file if present (but not directory)
    struct stat st;
    if (stat(dest_str.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        // Directory already exists, just update mode
        chmod(dest_str.c_str(), mode & 07777);
      } else {
        // Not a directory, remove and create
        (void)unlink(dest_str.c_str());
        if (mkdir(dest_str.c_str(), mode & 07777) != 0) {
          std::string msg = "Failed to create directory " + dest_str + ": " + strerror(errno);
          runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
          auto err = String::claim(runtime.heap, msg);
          RETURN(claim_result(runtime.heap, false, err));
        }
      }
    } else {
      // Doesn't exist, create it
      if (mkdir(dest_str.c_str(), mode & 07777) != 0) {
        std::string msg = "Failed to create directory " + dest_str + ": " + strerror(errno);
        runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
        auto err = String::claim(runtime.heap, msg);
        RETURN(claim_result(runtime.heap, false, err));
      }
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

// ============================================================================
// Primitive Registration
// ============================================================================

void prim_register_cas(CASContext* ctx, PrimMap& pmap) {
  prim_register(pmap, "cas_ingest_workspace_path", prim_cas_ingest_workspace_path,
                type_cas_ingest_workspace_path, PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_has_blob", prim_cas_has_blob, type_cas_has_blob, PRIM_PURE, ctx);
  prim_register(pmap, "cas_materialize_file", prim_cas_materialize_file, type_cas_materialize_file,
                PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_materialize_item", prim_cas_materialize_item, type_cas_materialize_item,
                PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_ingest_staged_item", prim_cas_ingest_staged_item,
                type_cas_ingest_staged_item, PRIM_IMPURE, ctx);
}
