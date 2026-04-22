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

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "json/json5.h"
#include "util/mkdir_parents.h"
#include "wcl/file_ops.h"

namespace fs = std::filesystem;

namespace {

struct Args {
  std::string staging_base;
  bool read_stdin = false;
  std::vector<std::string> output_paths;
};

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

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " --staging-base <path> [@|<output-path>...]\n";
}

bool parse_args(int argc, char** argv, Args& out) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next_value = [&](const char* flag, std::string& dest) -> bool {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << flag << "\n";
        return false;
      }
      dest = argv[++i];
      return true;
    };

    if (arg == "--staging-base") {
      if (!next_value("--staging-base", out.staging_base)) return false;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return false;
    } else if (arg == "@") {
      out.read_stdin = true;
    } else {
      out.output_paths.push_back(arg);
    }
  }

  if (out.staging_base.empty()) {
    std::cerr << "--staging-base is required\n";
    return false;
  }

  if (out.read_stdin && !out.output_paths.empty()) {
    std::cerr << "'@' cannot be combined with explicit output paths\n";
    return false;
  }

  if (!out.read_stdin && out.output_paths.empty()) {
    std::cerr << "at least one output path or '@' is required\n";
    return false;
  }

  return true;
}

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

std::string render_json(const std::string& stage_root, const std::vector<StageEntry>& entries) {
  std::ostringstream out;
  out << "{\"staging_root\":\"" << json_escape(stage_root) << "\",\"staging_files\":{";

  bool first = true;
  for (const auto& entry : entries) {
    if (!first) out << ",";
    first = false;

    out << "\"" << json_escape(entry.dest_path) << "\":{";
    switch (entry.type) {
      case EntryType::File:
        out << "\"type\":\"file\""
            << ",\"staging_path\":\"" << json_escape(entry.staging_path_or_target) << "\""
            << ",\"mode\":" << (entry.mode & 07777) << ",\"mtime_sec\":" << entry.mtime.tv_sec
            << ",\"mtime_nsec\":" << entry.mtime.tv_nsec;
        break;
      case EntryType::Symlink:
        out << "\"type\":\"symlink\""
            << ",\"target\":\"" << json_escape(entry.staging_path_or_target) << "\""
            << ",\"mtime_sec\":" << entry.mtime.tv_sec << ",\"mtime_nsec\":" << entry.mtime.tv_nsec;
        break;
      case EntryType::Directory:
        out << "\"type\":\"directory\""
            << ",\"mode\":" << (entry.mode & 07777) << ",\"mtime_sec\":" << entry.mtime.tv_sec
            << ",\"mtime_nsec\":" << entry.mtime.tv_nsec;
        break;
    }
    out << "}";
  }

  out << "}}";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    return 1;
  }

  try {
    CleanupRoot cleanup;
    cleanup.root = create_stage_root(args.staging_base);

    std::vector<std::string> output_paths = args.output_paths;
    if (args.read_stdin) {
      output_paths = read_input_paths(std::cin);
    }
    std::vector<StageEntry> entries = stage_outputs(output_paths, cleanup.root.string());
    std::string json = render_json(cleanup.root.string(), entries);
    std::cout << json << std::endl;

    cleanup.keep = true;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
