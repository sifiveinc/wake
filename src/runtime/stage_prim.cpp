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

#include "stage_prim.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "prim.h"
#include "types/data.h"
#include "types/primfn.h"
#include "util/mkdir_parents.h"
#include "value.h"
#include "wcl/file_ops.h"

namespace fs = std::filesystem;

namespace {

enum class EntryType { File, Symlink, Directory };

struct StageEntry {
  std::string dest_path;
  EntryType type;
  std::string staging_path_or_target;
  mode_t mode = 0;
  struct timespec mtime = {0, 0};
};

struct CleanupRoot {
  fs::path root;
  bool keep = false;

  ~CleanupRoot() {
    if (keep || root.empty()) return;
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

bool is_valid_workspace_relative_path(const std::string& path) {
  if (path.empty()) return false;

  fs::path normalized(path);
  if (normalized.is_absolute()) return false;

  for (const auto& component : normalized) {
    const std::string part = component.string();
    if (part == ".." || part == ".") {
      return false;
    }
  }

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

std::vector<std::string> read_input_paths(std::istream& in) {
  std::vector<std::string> paths;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    paths.push_back(line);
  }
  return paths;
}

std::string create_stage_root(const std::string& staging_base) {
  int err = mkdir_with_parents(staging_base, 0755);
  if (err != 0) {
    throw std::runtime_error("failed to create staging base " + staging_base + ": " +
                             strerror(err));
  }

  std::string templ = staging_base + "/stage.XXXXXX";
  std::vector<char> buffer(templ.begin(), templ.end());
  buffer.push_back('\0');

  char* created = mkdtemp(buffer.data());
  if (!created) {
    throw std::runtime_error("mkdtemp(" + templ + "): " + strerror(errno));
  }

  return created;
}

std::vector<StageEntry> stage_outputs(const std::vector<std::string>& output_paths,
                                      const std::string& stage_root) {
  std::vector<StageEntry> entries;
  size_t next_stage_id = 0;

  for (const auto& dest_path : output_paths) {
    if (!is_valid_workspace_relative_path(dest_path)) {
      throw std::runtime_error("invalid workspace-relative output path: " + dest_path);
    }

    struct stat st;
    if (lstat(dest_path.c_str(), &st) != 0) {
      throw std::runtime_error("lstat(" + dest_path + "): " + strerror(errno));
    }

    if (S_ISREG(st.st_mode)) {
      std::string staging_path = stage_root + "/" + std::to_string(next_stage_id++);
      auto copy_result = wcl::reflink_or_copy_file(dest_path, staging_path,
                                                   static_cast<mode_t>(st.st_mode & 07777));
      if (!copy_result) {
        throw std::runtime_error("failed to stage file " + dest_path + ": " +
                                 strerror(copy_result.error()));
      }

      entries.push_back(StageEntry{
          dest_path,
          EntryType::File,
          staging_path,
          static_cast<mode_t>(st.st_mode & 07777),
          st.st_mtim,
      });
    } else if (S_ISLNK(st.st_mode)) {
      auto target_result = read_symlink_target(dest_path);
      if (!target_result) {
        throw std::runtime_error(target_result.error());
      }

      entries.push_back(StageEntry{
          dest_path,
          EntryType::Symlink,
          *target_result,
          0,
          st.st_mtim,
      });
    } else if (S_ISDIR(st.st_mode)) {
      entries.push_back(StageEntry{
          dest_path,
          EntryType::Directory,
          "",
          static_cast<mode_t>(st.st_mode & 07777),
          st.st_mtim,
      });
    } else {
      throw std::runtime_error("unsupported output type for " + dest_path);
    }
  }

  return entries;
}

const char* type_name(EntryType t) {
  switch (t) {
    case EntryType::File:
      return "file";
    case EntryType::Symlink:
      return "symlink";
    case EntryType::Directory:
      return "directory";
  }
  return "";
}

// Build a 6-tuple via nested Pairs (claim_tuple5 covers up to 5; nest one more Pair on top).
Value* claim_tuple6(Heap& h, Value* a, Value* b, Value* c, Value* d, Value* e, Value* f) {
  return claim_tuple2(h, a, claim_tuple5(h, b, c, d, e, f));
}

inline size_t reserve_tuple6() { return reserve_tuple2() + reserve_tuple5(); }

}  // namespace

// prim "stage_outputs" pathsLines stagingBase -> Result (Pair String (List Entry)) String
// where Entry = (destPath, type, stagingPathOrTarget, mode, mtimeSec, mtimeNsec)
// pathsLines: newline-separated workspace-relative output paths.
// stagingBase: directory under which a fresh `stage.XXXXXX/` is created (mkdtemp).
// On any failure the staging directory (if created) is removed via CleanupRoot's destructor.
static PRIMTYPE(type_stage_outputs) {
  // Entry tuple: 6 nested Pairs.
  TypeVar e1, e2, e3, e4, e5;
  Data::typePair.clone(e1);
  Data::typePair.clone(e2);
  Data::typePair.clone(e3);
  Data::typePair.clone(e4);
  Data::typePair.clone(e5);
  e1[0].unify(Data::typeString);  // destPath
  e1[1].unify(e2);
  e2[0].unify(Data::typeString);  // type
  e2[1].unify(e3);
  e3[0].unify(Data::typeString);  // stagingPathOrTarget
  e3[1].unify(e4);
  e4[0].unify(Data::typeInteger);  // mode
  e4[1].unify(e5);
  e5[0].unify(Data::typeInteger);  // mtimeSec
  e5[1].unify(Data::typeInteger);  // mtimeNsec

  TypeVar entryList;
  Data::typeList.clone(entryList);
  entryList[0].unify(e1);

  TypeVar outerPair;
  Data::typePair.clone(outerPair);
  outerPair[0].unify(Data::typeString);  // staging_root
  outerPair[1].unify(entryList);

  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(outerPair);
  result[1].unify(Data::typeString);

  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         out->unify(result);
}

static PRIMFN(prim_stage_outputs) {
  EXPECT(2);
  STRING(paths_arg, 0);
  STRING(staging_base_arg, 1);

  auto fail = [&](const std::string& msg) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  };

  try {
    std::stringstream paths_stream(std::string(paths_arg->c_str(), paths_arg->size()));
    std::vector<std::string> output_paths = read_input_paths(paths_stream);

    CleanupRoot cleanup;
    if (!output_paths.empty()) {
      cleanup.root = create_stage_root(staging_base_arg->c_str());
    }
    std::vector<StageEntry> entries = stage_outputs(output_paths, cleanup.root.string());

    // Build wake values from the staged entries.
    size_t need = reserve_result() + reserve_tuple2() +
                  String::reserve(cleanup.root.string().size()) + reserve_list(entries.size());
    for (const auto& e : entries) {
      need += reserve_tuple6() + String::reserve(e.dest_path.size()) +
              String::reserve(strlen(type_name(e.type))) +
              String::reserve(e.staging_path_or_target.size()) + Integer::reserve(e.mode) +
              Integer::reserve(e.mtime.tv_sec) + Integer::reserve(e.mtime.tv_nsec);
    }
    runtime.heap.reserve(need);

    std::vector<Value*> entry_values;
    entry_values.reserve(entries.size());
    for (const auto& e : entries) {
      Value* dest_v = String::claim(runtime.heap, e.dest_path);
      Value* type_v = String::claim(runtime.heap, type_name(e.type));
      Value* sp_v = String::claim(runtime.heap, e.staging_path_or_target);
      Value* mode_v = Integer::claim(runtime.heap, MPZ(static_cast<long>(e.mode)));
      Value* msec_v = Integer::claim(runtime.heap, MPZ(static_cast<long>(e.mtime.tv_sec)));
      Value* mnsec_v = Integer::claim(runtime.heap, MPZ(static_cast<long>(e.mtime.tv_nsec)));
      entry_values.push_back(
          claim_tuple6(runtime.heap, dest_v, type_v, sp_v, mode_v, msec_v, mnsec_v));
    }

    Value* root_v = String::claim(runtime.heap, cleanup.root.string());
    Value* list_v = claim_list(runtime.heap, entry_values.size(), entry_values.data());
    Value* outer = claim_tuple2(runtime.heap, root_v, list_v);

    cleanup.keep = true;
    RETURN(claim_result(runtime.heap, true, outer));
  } catch (const std::exception& ex) {
    fail(ex.what());
    return;
  }
}

void prim_register_stage(PrimMap& pmap) {
  prim_register(pmap, "stage_outputs", prim_stage_outputs, type_stage_outputs, PRIM_IMPURE);
}
