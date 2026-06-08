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
#include <istream>
#include <sstream>
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

wcl::result<std::string, std::string> create_stage_root(const std::string& staging_base) {
  int err = mkdir_with_parents(staging_base, 0755);
  if (err != 0) {
    return wcl::result_error<std::string>("failed to create staging base " + staging_base + ": " +
                                          strerror(err));
  }

  std::string templ = staging_base + "/stage.XXXXXX";
  std::vector<char> buffer(templ.begin(), templ.end());
  buffer.push_back('\0');

  char* created = mkdtemp(buffer.data());
  if (!created) {
    return wcl::result_error<std::string>("mkdtemp(" + templ + "): " + strerror(errno));
  }

  return wcl::result_value<std::string>(std::string(created));
}

static std::string resolve_source_path(const std::string& stage_from_dir,
                                       const std::string& dest_path) {
  if (stage_from_dir.empty()) return dest_path;
  return stage_from_dir + "/" + dest_path;
}

wcl::result<std::vector<StageEntry>, std::string> stage_outputs(
    const std::vector<std::string>& output_paths, const std::string& stage_root,
    const std::string& stage_from_dir, bool in_place) {
  std::vector<StageEntry> entries;
  size_t next_stage_id = 0;

  for (const auto& dest_path : output_paths) {
    if (!is_valid_workspace_relative_path(dest_path)) {
      return wcl::result_error<std::vector<StageEntry>>("invalid workspace-relative output path: " +
                                                        dest_path);
    }

    const std::string source_path = resolve_source_path(stage_from_dir, dest_path);

    struct stat st;
    if (lstat(source_path.c_str(), &st) != 0) {
      return wcl::result_error<std::vector<StageEntry>>("lstat(" + source_path +
                                                        "): " + strerror(errno));
    }

    if (S_ISREG(st.st_mode)) {
      // In-place mode: hash directly from `source_path`; CAS ingest will atomically
      // rename the source into blobs/. Skip the per-output reflink. Used by runners
      // whose source tree is private and ephemeral (e.g. stagingDirNsRunner).
      std::string staging_path =
          in_place ? source_path : stage_root + "/" + std::to_string(next_stage_id++);
      if (!in_place) {
        auto copy_result = wcl::reflink_or_copy_file(source_path, staging_path,
                                                     static_cast<mode_t>(st.st_mode & 07777));
        if (!copy_result) {
          return wcl::result_error<std::vector<StageEntry>>("failed to stage file " + source_path +
                                                            ": " + strerror(copy_result.error()));
        }
      }

      entries.push_back(StageEntry{
          dest_path,
          EntryType::File,
          staging_path,
          static_cast<mode_t>(st.st_mode & 07777),
          st.st_mtim,
      });
    } else if (S_ISLNK(st.st_mode)) {
      auto target_result = read_symlink_target(source_path);
      if (!target_result) {
        return wcl::make_error<std::vector<StageEntry>, std::string>(target_result.error());
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
      return wcl::result_error<std::vector<StageEntry>>("unsupported output type for " +
                                                        source_path);
    }
  }

  return wcl::result_value<std::string>(std::move(entries));
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

// prim "stage_outputs" pathsLines stagingBase stageFromDir -> Result (Pair String (List Entry)) String
//
// WHEN:    Post-exec. Runners (workspaceRunner, fuseRunner, stagingDirNsRunner) call this
//          after a job finishes to snapshot declared outputs for hashing + CAS ingest.
// LAYOUT:  Produces a FLAT staging tree — `stage.XXXXXX/{0,1,2,...}` numbered slots, one
//          per output, in declaration order. Flatness decouples hashing from path nesting.
//          IN-PLACE MODE: pass empty stagingBase to skip the flat staging step; the
//          returned `staging_path` for each file points at its original source location
//          (under stage_from_dir). CAS ingest then atomically renames sources straight
//          into blobs/. Safe only when the source tree is private and ephemeral
//          (e.g. stagingDirNsRunner) — otherwise renaming files away from a shared
//          workspace would be visible to concurrent readers.
//
// where Entry = (destPath, type, stagingPathOrTarget, mode, mtimeSec, mtimeNsec)
// pathsLines:  newline-separated workspace-relative output paths.
// stagingBase: directory under which a fresh `stage.XXXXXX/` is created (mkdtemp).
//              Empty triggers in-place mode; the returned outer Pair's first element
//              is then "" (no flat staging root was created).
// stageFromDir: source-side base each output path is read from. Empty = wake's cwd
//               (the workspace); non-empty = a sandboxed runner's private staging tree.
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

  return args.size() == 3 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_stage_outputs) {
  EXPECT(3);
  STRING(paths_arg, 0);
  STRING(staging_base_arg, 1);
  STRING(stage_from_dir_arg, 2);

  auto fail = [&](const std::string& msg) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  };

  std::stringstream paths_stream(std::string(paths_arg->c_str(), paths_arg->size()));
  std::vector<std::string> output_paths = read_input_paths(paths_stream);

  // In-place mode: caller signals "no flat slot needed; hash directly from source paths"
  // by passing an empty stagingBase. CAS ingest will atomically rename the source files
  // out of stage_from_dir into the blobs/ tree. Used by stagingDirNsRunner whose
  // stage_from_dir is a private per-job tree that gets wiped at PostFinalize anyway.
  const bool in_place = (staging_base_arg->size() == 0);

  CleanupRoot cleanup;
  if (!in_place && !output_paths.empty()) {
    auto root_result = create_stage_root(staging_base_arg->c_str());
    if (!root_result) {
      fail(root_result.error());
      return;
    }
    cleanup.root = *root_result;
  }

  auto entries_result = stage_outputs(output_paths, cleanup.root.string(),
                                      std::string(stage_from_dir_arg->c_str()), in_place);
  if (!entries_result) {
    fail(entries_result.error());
    return;
  }
  const auto& entries = *entries_result;

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
}

// Materialize a Job's visible files, symlinks, and directories into `dest` staging directory
static wcl::result<bool, std::string> stage_visible_entry(const std::string& src,
                                                          const std::string& dest) {
  struct stat st;
  if (lstat(src.c_str(), &st) != 0) {
    return wcl::make_error<bool, std::string>("lstat(" + src + "): " + strerror(errno));
  }

  if (S_ISREG(st.st_mode)) {
    auto copy_result =
        wcl::reflink_or_copy_file(src, dest, static_cast<mode_t>(st.st_mode & 07777));
    if (!copy_result) {
      return wcl::make_error<bool, std::string>("reflink/copy " + src + " -> " + dest + ": " +
                                                strerror(copy_result.error()));
    }
    return wcl::make_result<bool, std::string>(true);
  }

  if (S_ISLNK(st.st_mode)) {
    auto target = read_symlink_target(src);
    if (!target) return wcl::make_error<bool, std::string>(target.error());
    if (symlink(target->c_str(), dest.c_str()) != 0) {
      return wcl::make_error<bool, std::string>("symlink(" + dest + " -> " + *target +
                                                "): " + strerror(errno));
    }
    return wcl::make_result<bool, std::string>(true);
  }

  if (S_ISDIR(st.st_mode)) {
    if (mkdir(dest.c_str(), static_cast<mode_t>(st.st_mode & 07777)) != 0 && errno != EEXIST) {
      return wcl::make_error<bool, std::string>("mkdir(" + dest + "): " + strerror(errno));
    }
    return wcl::make_result<bool, std::string>(true);
  }

  return wcl::make_error<bool, std::string>("unsupported source type for " + src);
}

// prim "stage_visible_inputs" workspaceDir stagingDir relPathsLines -> Result Unit String
//
// workspaceDir:   absolute path to wake's workspace root.
// stagingDir:     absolute path to the per-job stagingDir.
// relPathsLines:  newline-separated workspace-relative path of each visible.
//
// Prim utilized by stagingDirRunners to stage a Job's visible input list into a staging
// directory hierarchically (mirroring the workspace), before a job (wakebox) runs.
static PRIMTYPE(type_stage_visible_inputs) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);
  return args.size() == 3 && args[0]->unify(Data::typeString) &&
         args[1]->unify(Data::typeString) && args[2]->unify(Data::typeString) &&
         out->unify(result);
}

static PRIMFN(prim_stage_visible_inputs) {
  EXPECT(3);
  STRING(workspace_arg, 0);
  STRING(staging_dir_arg, 1);
  STRING(rel_paths_arg, 2);

  auto fail = [&](const std::string& msg) {
    runtime.heap.reserve(reserve_result() + String::reserve(msg.size()));
    auto err = String::claim(runtime.heap, msg);
    RETURN(claim_result(runtime.heap, false, err));
  };

  std::string workspace(workspace_arg->c_str(), workspace_arg->size());
  std::string staging_dir(staging_dir_arg->c_str(), staging_dir_arg->size());

  std::stringstream rel_stream(std::string(rel_paths_arg->c_str(), rel_paths_arg->size()));
  std::vector<std::string> rel_paths = read_input_paths(rel_stream);

  for (const auto& rel : rel_paths) {
    std::string src = workspace + "/" + rel;
    std::string dest = staging_dir + "/" + rel;

    fs::path parent = fs::path(dest).parent_path();
    if (!parent.empty()) {
      int err = mkdir_with_parents(parent.string(), 0755);
      if (err != 0) {
        fail("mkdir parents for " + dest + ": " + strerror(err));
        return;
      }
    }

    auto result = stage_visible_entry(src, dest);
    if (!result) {
      fail(result.error());
      return;
    }
  }

  runtime.heap.reserve(reserve_result() + reserve_unit());
  RETURN(claim_result(runtime.heap, true, claim_unit(runtime.heap)));
}

void prim_register_stage(PrimMap& pmap) {
  prim_register(pmap, "stage_outputs", prim_stage_outputs, type_stage_outputs, PRIM_IMPURE);
  prim_register(pmap, "stage_visible_inputs", prim_stage_visible_inputs,
                type_stage_visible_inputs, PRIM_IMPURE);
}
