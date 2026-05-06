/*
 * Copyright 2019 SiFive, Inc.
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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "wakefiles.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <re2/re2.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wcl/tracing.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>

#include "compat/readable.h"
#include "compat/windows.h"
#include "util/diagnostic.h"
#include "util/execpath.h"
#include "util/file.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

// clang-format off
EM_ASYNC_JS(char *, vscode_getfiles, (const char *dir, int *ok), {
  let files  = [];

  function shouldSkip(dirName) {
    return dirName === '.build' || dirName === '.fuse' || dirName === '.git';
  }

  const Path = require('path').posix;
  const FS   = require('fs');

  function walkTreeNode(dir) {
   FS.readdirSync(dir, {withFileTypes: true}).forEach(dirent => {
     const absolute = Path.join(dir, dirent.name);
     if (!dirent.isDirectory()) {
       files.push(absolute);
       return;
     }
     if (shouldSkip(dirent.name)) {
       return;
     }
     walkTreeNode(absolute);
   });
  }

  async function walkTreeWeb(dir) {
    if (dir.slice(-1) !== '/') {
      dir += '/';
    }
    const dirFiles = await wakeLspModule.sendRequest('readDir', dir);
    for (const dirent of dirFiles) {
      const absolute = new URL(dirent[0], dir).href; // dirent.name
      if (!dirent[1]) { // not a directory
        files.push(absolute);
        continue;
      }
      if (shouldSkip(dirent[0])) {
        continue;
      }
      await walkTreeWeb(absolute);
    }
  }

  try {
    if (ENVIRONMENT_IS_NODE) {
      walkTreeNode(UTF8ToString(dir));
    } else {
      await walkTreeWeb(UTF8ToString(dir));
    }
    const sep = String.fromCharCode(0);
    const out = files.join(sep) + sep;

    const lengthBytes = lengthBytesUTF8(out) + 1;
    const stringOnWasmHeap = _malloc(lengthBytes);
    stringToUTF8(out, stringOnWasmHeap, lengthBytes);
    setValue(ok, 1, 'i32');
    return stringOnWasmHeap;
  } catch (err) {
    const lengthBytes = lengthBytesUTF8(err.message) + 1;
    const stringOnWasmHeap = _malloc(lengthBytes);
    stringToUTF8(err.message, stringOnWasmHeap, lengthBytes);
    setValue(ok, 0, 'i32');
    return stringOnWasmHeap;
  }
});

EM_ASYNC_JS(char *, vscode_get_packaged_stdlib_files, (int *ok), {
  let files  = [];
  try {
    const libFiles = await wakeLspModule.sendRequest('getStdLibFiles');
    for (const file of libFiles) {
      files.push(file);
    }

    const sep = String.fromCharCode(0);
    const out = files.join(sep) + sep;

    const lengthBytes = lengthBytesUTF8(out) + 1;
    const stringOnWasmHeap = _malloc(lengthBytes);
    stringToUTF8(out, stringOnWasmHeap, lengthBytes);
    setValue(ok, 1, 'i32');
    return stringOnWasmHeap;
  } catch (err) {
    const lengthBytes = lengthBytesUTF8(err.message) + 1;
    const stringOnWasmHeap = _malloc(lengthBytes);
    stringToUTF8(err.message, stringOnWasmHeap, lengthBytes);
    setValue(ok, 0, 'i32');
    return stringOnWasmHeap;
  }
});
// clang-format on

bool push_files(int ok, char *files, std::vector<std::string> &out, const re2::RE2 &re,
                size_t skip) {
  if (ok) {
    for (char *file = files; *file; file += strlen(file) + 1) {
      std::string_view p(file + skip, strlen(file) - skip);
      if (RE2::FullMatch(p, re)) {
        out.emplace_back(file);
      }
    }
    free(files);
    return false;
  } else {
    fputs(files, stderr);
    fputs("\n", stderr);
    free(files);
    return true;
  }
}

bool push_files(std::vector<std::string> &out, const std::string &path, const re2::RE2 &re,
                size_t skip, FILE * /* warn_dest*/) {
  int ok;
  char *files = vscode_getfiles(path.c_str(), &ok);
  return push_files(ok, files, out, re, skip);
}

bool push_packaged_stdlib_files(std::vector<std::string> &out, const re2::RE2 &re, size_t skip) {
  int ok;
  char *files = vscode_get_packaged_stdlib_files(&ok);
  return push_files(ok, files, out, re, skip);
}

#else

struct profile_data {
  std::chrono::time_point<std::chrono::steady_clock> start{std::chrono::steady_clock::now()};
  size_t explored{0};
  bool alerted_user{false};
  FILE *dest{stdout};
};

// TODO : This function should probably be under compat in a files / vfs module
// Determines if *f* is a directory inside of *dirfd* using the best available method.
// Sets *failed* to true if unable to successfully determine directory status
static bool push_files_is_directory(int dirfd, const std::string &path, struct dirent *f,
                                    bool *failed) {
  struct stat sbuf;

#if defined(DT_DIR)
  if (f->d_type != DT_UNKNOWN) {
    return f->d_type == DT_DIR;
  }
#endif

  if (fstatat(dirfd, f->d_name, &sbuf, AT_SYMLINK_NOFOLLOW) != 0) {
    fprintf(stderr, "Failed to fstatat %s/%s: %s\n", path.c_str(), f->d_name, strerror(errno));
    *failed = true;
    return false;
  }

  return S_ISDIR(sbuf.st_mode);
}

// Recursively fills *out* with all paths under *dirfd* matching *re*. Automatically
// skips directories that cannot have source files in them(.git, .build, .fuse).
static bool push_files(std::vector<std::string> &out, const std::string &path, int dirfd,
                       const RE2 &re, size_t skip, struct profile_data *profile) {
  auto dir = fdopendir(dirfd);
  if (!dir) {
    close(dirfd);
    fprintf(stderr, "Failed to fdopendirat %s: %s\n", path.c_str(), strerror(errno));
    return true;
  }

  struct dirent *f;
  bool failed = false;
  for (errno = 0; 0 != (f = readdir(dir)); errno = 0) {
    profile->explored++;
    std::string fdname(f->d_name);
    std::string name(path == "." ? f->d_name : (path + "/" + f->d_name));

    // Relative directories should never be pushed
    if (fdname == "." || fdname == "..") continue;

    // These directories should never be pushed
    if (name == ".build" || name == ".fuse" || name == ".git") continue;

    if (!push_files_is_directory(dirfd, path, f, &failed)) {
      // Append the current file if it matches the regex
      std::string_view p(name.c_str() + skip, name.size() - skip);
      if (RE2::FullMatch(p, re)) out.emplace_back(std::move(name));
      continue;
    }

    int fd = openat(dirfd, f->d_name, O_RDONLY);
    if (fd == -1) {
      fprintf(stderr, "Failed to openat %s/%s: %s\n", path.c_str(), f->d_name, strerror(errno));
      failed = true;
      continue;
    }

    // Check elapsed time and emit a message if this is taking too long.
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - profile->start).count() >
        1000) {
      profile->start = now;
      profile->alerted_user = true;

      fprintf(profile->dest, "Finding wake files is taking longer than expected (%ld explored).\r",
              profile->explored);
      fflush(profile->dest);
    }

    // Recurse & capture any failures
    if (push_files(out, name, fd, re, skip, profile)) failed = true;
  }

  if (errno != 0 && !failed) {
    fprintf(stderr, "Failed to readdir %s: %s\n", path.c_str(), strerror(errno));
    return true;
  }

  if (closedir(dir) != 0) {
    fprintf(stderr, "Failed to closedir %s: %s\n", path.c_str(), strerror(errno));
    return true;
  }

  return failed;
}

bool push_files(std::vector<std::string> &out, const std::string &path, const RE2 &re, size_t skip,
                FILE *user_warning_dest) {
  int flags, dirfd = open(path.c_str(), O_RDONLY);
  if ((flags = fcntl(dirfd, F_GETFD, 0)) != -1) fcntl(dirfd, F_SETFD, flags | FD_CLOEXEC);

  if (dirfd == -1) {
    return true;
  }

  struct profile_data profile;
  profile.dest = user_warning_dest;

  bool ret = push_files(out, path, dirfd, re, skip, &profile);

  if (profile.alerted_user) {
    fprintf(profile.dest, "\n");
  }

  return ret;
}
#endif

struct WakeFilter {
  size_t prefix;
  bool allow;
  std::unique_ptr<RE2> exp;
  WakeFilter(size_t prefix_, bool allow_, const std::string &exp, const RE2::Options &opts)
      : prefix(prefix_), allow(allow_), exp(new RE2(exp, opts)) {}
};

std::string glob2regexp(const std::string &glob) {
  std::string exp("(?s)");
  size_t s = 0, e;
  while ((e = glob.find_first_of("\\[?*", s)) != std::string::npos) {
    std::string_view piece(glob.c_str() + s, e - s);
    exp.append(RE2::QuoteMeta(piece));
    if (glob[e] == '\\') {
      // Trailing \ is left as a raw '\'
      // Don't bother escaping a codepoint start
      if (e + 1 == glob.size() || static_cast<unsigned char>(glob[e + 1]) >= 0x80) {
        s = e + 1;
      } else {
        std::string_view piece(glob.c_str() + e + 1, 1);
        exp.append(RE2::QuoteMeta(piece));
        s = e + 2;
      }
    } else if (glob[e] == '[') {
      // To include ']' in a clause, use: []abc]
      // If no closing ']' is found, just stop processing the glob
      size_t c = glob.find_first_of("]", e + 2);
      if (c == std::string::npos) {
        s = e;
        break;
      }
      exp.push_back('[');
      size_t is = e + 1, ie;
      while ((ie = glob.find_first_of("\\[", is)) != std::string::npos) {
        exp.append(glob, is, ie - is);
        exp.push_back('\\');
        exp.push_back(glob[ie]);
        is = ie + 1;
      }
      exp.append(glob, is, c - is);
      exp.push_back(']');
      s = c + 1;
    } else if (glob[e] == '?') {
      // ? can match any non-/ character
      exp.append("[^/]");
      s = e + 1;
    } else if (e > 0 && glob.size() == e + 2 && glob[e - 1] == '/' && glob[e + 1] == '*') {
      // trailing /** -> match everything inside
      exp.append(".+");
      s = e + 2;
    } else if (e == 0 && glob.size() > 2 && glob[1] == '*' && glob[2] == '/') {
      // leading **/ -> any subdirectory
      exp.append("([^/]*/)*");
      s = e + 3;
    } else if (e > 0 && glob.size() > e + 2 && glob[e - 1] == '/' && glob[e + 1] == '*' &&
               glob[e + 2] == '/') {
      // /**/ somewhere in the string
      exp.append("([^/]*/)*");
      s = e + 3;
    } else {
      // just a normal * -> match any number of non-/ characters
      exp.append("[^/]*");
      s = e + 1;
    }
  }
  exp.append(RE2::QuoteMeta(glob.c_str() + s));
  return exp;
}

class DiagnosticIgnorer : public DiagnosticReporter {
  void report(Diagnostic diagnostic) {}
};

static void process_ignorefile(const std::string &path, std::vector<WakeFilter> &filters) {
  DiagnosticIgnorer ignorer;
  std::string wakeignore = path + ".wakeignore";
  ExternalFile file(ignorer, wakeignore.c_str());
  StringSegment segment = file.segment();
  std::stringstream in;
  in.write(reinterpret_cast<const char *>(segment.start), segment.size());

  RE2::Options options;
  options.set_log_errors(false);
  options.set_one_line(true);

  std::string line;
  while (std::getline(in, line)) {
    // strip trailing whitespace (including windows CR)
    size_t found = line.find_last_not_of(" \t\f\v\n\r");
    if (found != std::string::npos) {
      line.erase(found + 1);
    } else {
      line.clear();  // entirely whitespace
    }
    // allow empty lines and comments
    if (line == "" || line[0] == '#') continue;
    // check for negation
    bool allow = line[0] == '!';
    if (allow) line.erase(0, 1);
    // create a regular expression for .gitignore style syntax
    filters.emplace_back(path.size(), allow, glob2regexp(line), options);
  }
}

static std::vector<std::string> filter_wakefiles(std::vector<std::string> &&wakefiles,
                                                 const std::string &basedir, bool verbose) {
  std::string curdir = basedir;  // Either "" or ".+/"
  if (curdir == ".") {
    curdir.clear();
  } else if (!curdir.empty() && curdir.back() != '/') {
    curdir.push_back('/');
  }

  std::vector<WakeFilter> filters;
  std::vector<std::string> output;
  output.reserve(wakefiles.size());

  process_ignorefile(curdir, filters);

  for (auto &wakefile : wakefiles) {
    // Unwind curdir
    while (!curdir.empty() && wakefile.compare(0, curdir.size(), curdir) != 0) {
      size_t slash = curdir.find_last_of('/', curdir.size() - 2);
      if (slash == std::string::npos) {
        curdir.clear();
      } else {
        curdir.erase(slash + 1);
      }
    }

    // Expire any patterns from directories we've left
    while (!filters.empty() && filters.back().prefix > curdir.size()) filters.pop_back();

    // Descend into directory
    size_t slash;
    while ((slash = wakefile.find_first_of('/', curdir.size())) != std::string::npos) {
      curdir.append(wakefile, curdir.size(), slash + 1 - curdir.size());
      process_ignorefile(curdir, filters);
    }

    // See if any rules exclude this file
    bool skip = false;
    size_t prefix = std::string::npos;
    for (auto &filter : filters) {
      std::string_view piece(wakefile.c_str() + filter.prefix, wakefile.size() - filter.prefix);
      if (skip == filter.allow && RE2::FullMatch(piece, *filter.exp)) {
        skip = !filter.allow;
        prefix = filter.prefix;
      }
    }

    if (skip) {
      if (verbose) {
        fprintf(stderr, "Skipping %s due to %s.wakeignore\n", wakefile.c_str(),
                wakefile.substr(0, prefix).c_str());
      }
    } else {
      output.emplace_back(std::move(wakefile));
    }
  }

  return output;
}

std::vector<std::string> find_external_stdlib_files(bool &ok, bool verbose,
                                                    const std::string &libdir,
                                                    FILE *user_warning_dest, const RE2 &exp) {
  std::vector<std::string> libfiles;
#ifdef __EMSCRIPTEN__
  // clang-format off
    int isNode = EM_ASM_INT({
      return ENVIRONMENT_IS_NODE;
    });
  // clang-format on
  if (isNode) {
    if (push_files(libfiles, libdir, exp, 0, user_warning_dest)) ok = false;
  } else {
    if (push_packaged_stdlib_files(libfiles, exp, 0)) ok = false;
  }
#else
  if (push_files(libfiles, libdir, exp, 0, user_warning_dest)) ok = false;
#endif
  std::sort(libfiles.begin(), libfiles.end());
  return filter_wakefiles(std::move(libfiles), libdir, verbose);
}

std::vector<std::string> find_workspace_wakefiles_via_search(bool &ok, bool verbose,
                                                             const std::string &workdir,
                                                             FILE *user_warning_dest,
                                                             const RE2 &exp) {
  std::vector<std::string> workfiles;
  if (push_files(workfiles, workdir, exp, 0, user_warning_dest)) ok = false;
  std::sort(workfiles.begin(), workfiles.end());
  return filter_wakefiles(std::move(workfiles), workdir, verbose);
}

std::vector<std::string> find_workspace_wakefiles_via_manifest(const std::string &manifest_path,
                                                               const std::string &workdir,
                                                               bool strict) {
  std::vector<std::string> workfiles;
  std::vector<std::string> unreadable_files;
  DiagnosticIgnorer ignorer;
  ExternalFile file(ignorer, manifest_path.c_str());
  StringSegment segment = file.segment();
  std::stringstream in;
  in.write(reinterpret_cast<const char *>(segment.start), segment.size());

  std::string prefix = (workdir == ".") ? "" : (workdir + "/");

  std::string line;
  while (std::getline(in, line)) {
    // strip trailing whitespace (including windows CR)
    size_t found = line.find_last_not_of(" \t\f\v\n\r");
    if (found != std::string::npos) {
      line.erase(found + 1);
    } else {
      line.clear();  // entirely whitespace
    }

    // allow empty lines and comments
    if (line == "" || line[0] == '#') continue;

    if (is_readable(line.c_str())) {
      workfiles.push_back(prefix + line);
    } else {
      unreadable_files.push_back(line);
    }
  }

  for (const auto &file : unreadable_files) {
    wcl::log::error(
        "File '%s' listed in '%s/.wakemanifest' does not exist. Either remove the path from "
        "'.wakemanifest' or create a valid wake file at '%s'.",
        file.c_str(), workdir.c_str(), file.c_str())
        .urgent()();
  }

  if (strict && !unreadable_files.empty()) {
    exit(1);
  }

  return workfiles;
}
std::vector<std::string> find_all_wakefiles(bool &ok, bool workspace, bool verbose,
                                            const std::string &libdir, const std::string &workdir,
                                            FILE *user_warning_dest, bool strict) {
  ok = true;
  RE2::Options options;
  options.set_log_errors(false);
  options.set_one_line(true);
  RE2 exp("(?s).*[^/]\\.wake", options);

  std::vector<std::string> libfiles, workfiles;

  std::string boolean_path = workdir + "/share/wake/lib/core/boolean.wake";
  bool workspace_contains_stdlib = workspace && is_readable(boolean_path.c_str());
  if (!workspace_contains_stdlib) {
    libfiles = find_external_stdlib_files(ok, verbose, libdir, user_warning_dest, exp);
  }

  if (!workspace) {
    return libfiles;
  }

  std::string manifest_path = workdir + "/.wakemanifest";
  if (is_readable(manifest_path.c_str())) {
    workfiles = find_workspace_wakefiles_via_manifest(manifest_path, workdir, strict);
  } else {
    workfiles = find_workspace_wakefiles_via_search(ok, verbose, workdir, user_warning_dest, exp);
  }

  // Combine the two sorted vectors into one sorted vector
  std::vector<std::string> output;
  output.reserve(libfiles.size() + workfiles.size());
  std::merge(std::make_move_iterator(libfiles.begin()), std::make_move_iterator(libfiles.end()),
             std::make_move_iterator(workfiles.begin()), std::make_move_iterator(workfiles.end()),
             std::back_inserter(output));

  // Eliminate any files present in both vectors
  auto it = std::unique(output.begin(), output.end());
  output.resize(std::distance(output.begin(), it));

  return output;
}
