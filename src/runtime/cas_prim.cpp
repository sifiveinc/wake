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
#include <filesystem>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

  cas::Cas* store = ctx->get_store(".");
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

  cas::Cas* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_bool());
    RETURN(claim_bool(runtime.heap, false));
  }

  auto hash_result = cas::ContentHash::from_hex(hash_str->c_str());
  if (!hash_result) {
    runtime.heap.reserve(reserve_bool());
    RETURN(claim_bool(runtime.heap, false));
  }
  bool exists = store->has_blob(*hash_result);

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

  cas::Cas* store = ctx->get_store(".");
  if (!store) {
    runtime.heap.reserve(reserve_result() + String::reserve(28));
    auto err = String::claim(runtime.heap, "CAS store not initialized");
    RETURN(claim_result(runtime.heap, false, err));
  }

  auto hash_result = cas::ContentHash::from_hex(hash_str->c_str());
  if (!hash_result) {
    runtime.heap.reserve(reserve_result() + String::reserve(25));
    auto err = String::claim(runtime.heap, "Invalid content hash hex");
    RETURN(claim_result(runtime.heap, false, err));
  }
  mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

  // Use 0 for mtime to let the system use current time
  auto result = store->materialize_blob(*hash_result, dest_path->c_str(), mode, 0, 0);
  if (!result) {
    runtime.heap.reserve(reserve_result() + String::reserve(35));
    auto err = String::claim(runtime.heap, "Failed to materialize file from CAS");
    RETURN(claim_result(runtime.heap, false, err));
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

// prim "cas_materialize_item" destPath type hashOrTarget mode mtimeSec mtimeNsec -> Result Unit Error
// Materialize an item from CAS to workspace (files already in CAS) or create symlink/directory
// - type="file": hashOrTarget = content hash, uses mode/mtime
// - type="symlink": hashOrTarget = symlink target
// - type="directory": hashOrTarget = "" (unused), uses mode
static PRIMTYPE(type_cas_materialize_item) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);  // Error message as String, converted to Error in Wake
  return args.size() == 6 && args[0]->unify(Data::typeString) &&   // destPath
         args[1]->unify(Data::typeString) &&                       // type
         args[2]->unify(Data::typeString) &&                       // hashOrTarget
         args[3]->unify(Data::typeInteger) &&                      // mode
         args[4]->unify(Data::typeInteger) &&                      // mtimeSec
         args[5]->unify(Data::typeInteger) &&                      // mtimeNsec
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
    auto hash_result = cas::ContentHash::from_hex(hash_str_val.c_str());
    if (!hash_result) {
      std::string msg = "Invalid content hash: " + hash_str_val;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));
    time_t mtime_sec = static_cast<time_t>(mpz_get_si(mtime_sec_mpz));
    long mtime_nsec = static_cast<long>(mpz_get_si(mtime_nsec_mpz));

    // Materialize from CAS to workspace with timestamps
    auto mat_result = store->materialize_blob(*hash_result, dest_str.c_str(), mode, mtime_sec, mtime_nsec);
    if (!mat_result) {
      std::string msg = "Failed to materialize blob " + hash_str_val + " to " + dest_str;
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "symlink") {
    std::string target = hash_or_target->c_str();

    // Create the symlink at a temporary path first, then atomically rename.
    std::string temp_path = dest_str + ".tmp." + std::to_string(getpid());
    (void)unlink(temp_path.c_str());

    if (symlink(target.c_str(), temp_path.c_str()) != 0) {
      std::string msg =
          "Failed to create symlink " + dest_str + " -> " + target + ": " + strerror(errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

    if (rename(temp_path.c_str(), dest_str.c_str()) != 0) {
      int saved_errno = errno;
      (void)unlink(temp_path.c_str());
      std::string msg =
          "Failed to create symlink " + dest_str + " -> " + target + ": " + strerror(saved_errno);
      runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
      auto err = String::claim(runtime.heap, msg);
      RETURN(claim_result(runtime.heap, false, err));
    }

  } else if (type == "directory") {
    mode_t mode = static_cast<mode_t>(mpz_get_ui(mode_mpz));

    struct stat st;
    if (stat(dest_str.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      // Directory already exists, just update mode
      chmod(dest_str.c_str(), mode);
    } else {
      // Create directory at a temporary path, then atomically rename into place.
      std::string temp_dir = dest_str + ".tmp." + std::to_string(getpid());
      (void)rmdir(temp_dir.c_str());

      if (mkdir(temp_dir.c_str(), mode) != 0) {
        std::string msg = "Failed to create directory " + temp_dir + ": " + strerror(errno);
        runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
        auto err = String::claim(runtime.heap, msg);
        RETURN(claim_result(runtime.heap, false, err));
      }

      if (rename(temp_dir.c_str(), dest_str.c_str()) != 0) {
        int saved_errno = errno;
        (void)rmdir(temp_dir.c_str());
        std::string msg = "Failed to create directory " + dest_str + ": " + strerror(saved_errno);
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


// ============================================================================
// Primitive Registration
// ============================================================================

void prim_register_cas(CASContext* ctx, PrimMap& pmap) {
  prim_register(pmap, "cas_store_file", prim_cas_store_file, type_cas_store_file, PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_has_blob", prim_cas_has_blob, type_cas_has_blob, PRIM_PURE, ctx);
  prim_register(pmap, "cas_materialize_file", prim_cas_materialize_file, type_cas_materialize_file,
                PRIM_IMPURE, ctx);
  prim_register(pmap, "cas_materialize_item", prim_cas_materialize_item, type_cas_materialize_item,
                PRIM_IMPURE, ctx);
}
