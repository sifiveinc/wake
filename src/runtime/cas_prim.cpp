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

#include <iostream>

#include "cas/cas_store.h"
#include "cas/file_ops.h"
#include "prim.h"
#include "types/data.h"
#include "types/primfn.h"
#include "value.h"
#include "wcl/filepath.h"

// ============================================================================
// CASContext implementation
// ============================================================================

cas::CASStore* CASContext::get_store(const std::string& workspace) {
  if (store_ && workspace_ == workspace) {
    return store_.get();
  }

  std::string cas_root = workspace + "/.cas";
  auto store_result = cas::CASStore::open(cas_root);
  if (store_result) {
    store_ = std::make_unique<cas::CASStore>(std::move(*store_result));
    workspace_ = workspace;
    return store_.get();
  }

  return nullptr;
}

// ============================================================================
// CAS Primitives
// ============================================================================

// prim "cas_store_file" path: String -> Result String Error
// Stores a file in CAS and returns its content hash
static PRIMTYPE(type_cas_store_file) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_cas_store_file) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(1);
  STRING(path, 0);

  cas::CASStore* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  auto result = store->store_blob_from_file(path->c_str());
  if (!result) {
    runtime.heap.reserve(reserve_result() + String::reserve(30));
    auto err = String::claim(runtime.heap, "Failed to store file in CAS");
    RETURN(claim_result(runtime.heap, false, err));
  }

  std::string hash = result->to_hex();
  runtime.heap.reserve(reserve_result() + String::reserve(hash.size()));
  RETURN(claim_result(runtime.heap, true, String::claim(runtime.heap, hash)));
}

// prim "cas_has_blob" hash: String -> Boolean
// Checks if a blob exists in the CAS store
static PRIMTYPE(type_cas_has_blob) {
  return args.size() == 1 && args[0]->unify(Data::typeString) &&
         out->unify(Data::typeBoolean);
}

static PRIMFN(prim_cas_has_blob) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(1);
  STRING(hash_str, 0);

  cas::CASStore* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_bool());
    RETURN(claim_bool(runtime.heap, false));
  }

  cas::ContentHash hash = cas::ContentHash::from_hex(hash_str->c_str());
  bool exists = store->has_blob(hash);

  runtime.heap.reserve(reserve_bool());
  RETURN(claim_bool(runtime.heap, exists));
}

// prim "cas_materialize_file" hash: String -> destPath: String -> mode: Integer -> Result Unit Error
// Materializes a file from CAS to the filesystem
static PRIMTYPE(type_cas_materialize_file) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 3 && args[0]->unify(Data::typeString) &&
         args[1]->unify(Data::typeString) && args[2]->unify(Data::typeInteger) &&
         out->unify(result);
}

static PRIMFN(prim_cas_materialize_file) {
  CASContext* ctx = static_cast<CASContext*>(data);
  EXPECT(3);
  STRING(hash_str, 0);
  STRING(dest_path, 1);
  INTEGER_MPZ(mode_mpz, 2);

  cas::CASStore* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  cas::ContentHash hash = cas::ContentHash::from_hex(hash_str->c_str());
  mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

  auto result = store->materialize_blob(hash, dest_path->c_str(), mode);
  if (!result) {
    runtime.heap.reserve(reserve_result() + String::reserve(35));
    auto err = String::claim(runtime.heap, "Failed to materialize file from CAS");
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

// prim "cas_ingest_staging_file" destPath type stagingPathOrTarget hash mode mtimeSec mtimeNsec -> Result Unit Error
// Unified atomic operation for all staged item types (file, symlink, directory)
// - type="file": stagingPathOrTarget = staging path, uses hash/mode/mtime
// - type="symlink": stagingPathOrTarget = symlink target
// - type="directory": stagingPathOrTarget = "" (unused), uses mode
static PRIMTYPE(type_cas_ingest_staging_file) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 7 && args[0]->unify(Data::typeString) &&   // destPath
         args[1]->unify(Data::typeString) &&                       // type
         args[2]->unify(Data::typeString) &&                       // stagingPathOrTarget
         args[3]->unify(Data::typeString) &&                       // hash
         args[4]->unify(Data::typeInteger) &&                      // mode
         args[5]->unify(Data::typeInteger) &&                      // mtimeSec
         args[6]->unify(Data::typeInteger) &&                      // mtimeNsec
         out->unify(result);
}

static PRIMFN(prim_cas_ingest_staging_file) {
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
  auto parent = wcl::parent_and_base(dest_str);
  if ((bool)parent && !parent->first.empty()) {
    auto mkdir_result = cas::mkdir_parents(parent->first);
    if (!mkdir_result) {
      std::string msg = "Failed to create parent directories for " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }
  }

  if (type == "file") {
    // Handle regular file: store in CAS, materialize, apply timestamps, cleanup
    cas::CASStore* store = ctx->get_store(".");
    if (!store) {
      runtime.heap.reserve(reserve_result() + String::reserve(28));
      auto err = String::claim(runtime.heap, "CAS store not initialized");
      RETURN(claim_result(runtime.heap, false, err));
    }

    std::string staging_path = staging_path_or_target->c_str();

    // 1. Store staging file in CAS
    auto store_result = store->store_blob_from_file(staging_path.c_str());
    if (!store_result) {
      std::string msg = "Failed to store staging file in CAS: " + staging_path;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    // Verify the stored hash matches the expected hash
    cas::ContentHash expected_hash = cas::ContentHash::from_hex(hash_str->c_str());
    cas::ContentHash stored_hash = *store_result;

    if (expected_hash.to_hex() != stored_hash.to_hex()) {
      std::string msg = "Hash mismatch: expected " + std::string(hash_str->c_str()) +
                        " but got " + stored_hash.to_hex();
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    // 2. Materialize to workspace
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

    auto mat_result = store->materialize_blob(stored_hash, dest_str.c_str(), mode);
    if (!mat_result) {
      std::string msg = "Failed to materialize blob " + stored_hash.to_hex() + " to " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    // 3. Apply timestamps
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;  // Don't change atime
    times[1].tv_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
    times[1].tv_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

    if (utimensat(AT_FDCWD, dest_str.c_str(), times, 0) != 0) {
      std::cerr << "Warning: Failed to set timestamps on " << dest_str << std::endl;
    }

    // 4. Delete staging file
    if (unlink(staging_path.c_str()) != 0) {
      std::cerr << "Warning: Failed to delete staging file " << staging_path << std::endl;
    }

  } else if (type == "symlink") {
    // Handle symlink: create symlink with target
    std::string target = staging_path_or_target->c_str();

    // Remove existing file/symlink if present
    (void)unlink(dest_str.c_str());

    // Create the symlink
    if (symlink(target.c_str(), dest_str.c_str()) != 0) {
      std::string msg = "Failed to create symlink " + dest_str + " -> " + target + ": " + strerror(errno);
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
  prim_register(pmap, "cas_store_file", prim_cas_store_file, type_cas_store_file, PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_has_blob", prim_cas_has_blob, type_cas_has_blob, PRIM_PURE, ctx);
  prim_register(pmap, "cas_materialize_file", prim_cas_materialize_file, type_cas_materialize_file,
                PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_ingest_staging_file", prim_cas_ingest_staging_file,
                type_cas_ingest_staging_file, PRIM_IMPURE, ctx);
}
