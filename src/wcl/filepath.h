/*
 * Copyright 2022 SiFive, Inc.
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

#pragma once

#include <dirent.h>
#include <sys/types.h>

#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "result.h"

namespace wcl {

enum class file_type { block, character, directory, fifo, symlink, regular, socket, unknown };

struct directory_entry {
  std::string name;
  file_type type;
};

class directory_range;

class directory_iterator {
  friend class directory_range;

 private:
  std::string dir_path;
  DIR* dir = nullptr;
  std::optional<result<directory_entry, posix_error_t>> value;

  // This steps the current entry by one
  void step();
  directory_iterator(std::string dir_path, DIR* dir) : dir(dir), value() {}

 public:
  ~directory_iterator() {}
  directory_iterator() : dir_path(), dir(nullptr), value() {}
  directory_iterator(const directory_iterator&) = delete;
  directory_iterator(directory_iterator&& other) : dir(other.dir), value(std::move(other.value)) {
    other.dir = nullptr;
  }

  result<directory_entry, posix_error_t> operator*() {
    // on our first call we won't have a value set, so set it.
    if (!value && dir) {
      step();
    }

    if (!value && !dir) {
      return make_error<directory_entry, posix_error_t>(EBADF);
    }

    return *value;
  }

  directory_iterator& operator++() {
    step();
    return *this;
  }

  bool operator!=(const directory_iterator& other) { return dir != other.dir; }
};

class directory_range {
 private:
  DIR* dir = nullptr;
  std::string dir_path;

  directory_range(const std::string& path) : dir(nullptr), dir_path(path) {
    dir = opendir(path.c_str());
  }

 public:
  ~directory_range() {
    if (dir) {
      closedir(dir);
    }
  }
  directory_range(const directory_range&) = delete;
  directory_range(directory_range&& dir) : dir(dir.dir), dir_path(std::move(dir.dir_path)) {
    dir.dir = nullptr;
  }

  static result<directory_range, posix_error_t> open(const std::string& path) {
    directory_range out{path};
    if (out.dir == nullptr) {
      return make_errno<directory_range>();
    }

    return make_result<directory_range, posix_error_t>(std::move(out));
  }

  directory_iterator begin() { return directory_iterator(dir_path, dir); }

  directory_iterator end() { return directory_iterator{}; }
};

// filepath_iterator breaks up a filepath
// into its parts. So if you're iterating over
// foo/bar/baz then this iterator iterates over
// the sequence "foo" and then "bar" and then "baz"
class filepath_iterator {
 private:
  // TODO: This should be a string_view
  const std::string& str;
  size_t start = 0, final = 0;

  // This moves `final` to the next '/' or to the end of the path
  // and moves `start` past '/' if its currently on it.
  void next() {
    if (final < str.size() && str[final] == '/') final++;
    start = final;
    while (final < str.size() && str[final] != '/') {
      final++;
    }
  }

 public:
  filepath_iterator(const filepath_iterator& other)
      : str(other.str), start(other.start), final(other.final) {}
  filepath_iterator(filepath_iterator&& other)
      : str(other.str), start(other.start), final(other.final) {}
  explicit filepath_iterator(const std::string& str) : str(str), start(), final() { next(); }

  filepath_iterator(const std::string& str, size_t begin) : str(str), start(begin), final(begin) {
    next();
  }

  filepath_iterator operator++(int) {
    auto out = *this;
    next();
    return out;
  }

  filepath_iterator& operator++() {
    next();
    return *this;
  }

  // TODO: This should be a string_view
  std::string operator*() const { return std::string(str.begin() + start, str.begin() + final); }

  bool operator==(const filepath_iterator& iter) const { return start == iter.start; }

  bool operator!=(const filepath_iterator& iter) const { return start != iter.start; }
};

// filepath_range_ref is a range that can be constructed
// from a reference to an std::string that will outlive it.
// it then adapts the string to a range that uses filepath_iterator
// to iterate over the parts of a file path. Only use this if you
// know that the lifetime of the argument will outlive this iterator.
class filepath_range_ref {
 private:
  // TODO: This should be a string_view
  const std::string& str;

 public:
  filepath_range_ref(const std::string& str) : str(str) {}

  filepath_iterator begin() const { return filepath_iterator(str); }

  filepath_iterator end() const { return filepath_iterator(str, str.size()); }
};

// filepath_range is a range that can be constructed
// from anything an std::string can be constructed from.
// It then adapts the string to a range that uses filepath_iterator
// to iterate over the parts of a file path.
class filepath_range {
 private:
  // TODO: This should be a string_view
  const std::string str;

 public:
  filepath_range(const std::string& str) : str(str) {}

  filepath_iterator begin() const { return filepath_iterator(str); }

  filepath_iterator end() const { return filepath_iterator(str, str.size()); }
};

inline filepath_range make_filepath_range(const std::string& str) { return filepath_range(str); }

inline filepath_range make_filepath_range(std::string&& str) {
  return filepath_range(std::move(str));
}

inline filepath_range_ref make_filepath_range_ref(const std::string& str) {
  return filepath_range_ref(str);
}

// Returns the canonicalized version of string x.
//
// Ex:
//   . => .
//   hax/ => hax
//   foo/.././bar.z => bar.z
//   foo/../../bar.z => ../bar.z
inline std::string make_canonical(const std::string& x) {
  bool abs = x[0] == '/';

  std::stringstream str;
  if (abs) str << "/";

  std::vector<std::string> tokens;

  size_t tok = 0;
  size_t scan = 0;
  bool repeat;
  bool pop = false;
  do {
    scan = x.find_first_of('/', tok);
    repeat = scan != std::string::npos;
    std::string token(x, tok, repeat ? (scan - tok) : scan);
    tok = scan + 1;
    if (token == "..") {
      if (!tokens.empty()) {
        tokens.pop_back();
      } else if (!abs) {
        str << "../";
        pop = true;
      }
    } else if (!token.empty() && token != ".") {
      tokens.emplace_back(std::move(token));
    }
  } while (repeat);

  if (tokens.empty()) {
    if (abs) {
      return "/";
    } else {
      if (pop) {
        std::string out = str.str();
        out.resize(out.size() - 1);
        return out;
      } else {
        return ".";
      }
    }
  } else {
    str << tokens.front();
    for (auto i = tokens.begin() + 1; i != tokens.end(); ++i) str << "/" << *i;
    return str.str();
  }
}

inline std::string _join_paths(std::string a, const std::string& b) {
  if (a.back() != '/') a += '/';
  auto begin = b.begin();
  if (*begin == '/') ++begin;
  a.insert(a.end(), begin, b.end());
  return a;
}

inline std::string _join_paths(std::string a, const std::string& b, const std::string& c) {
  return _join_paths(_join_paths(a, b), c);
}

inline std::string _join_paths(std::string a, const std::string& b, const std::string& c,
                               const std::string& d) {
  return _join_paths(_join_paths(_join_paths(a, b), c), d);
}

inline std::string _join_paths(std::string a, const std::string& b, const std::string& c,
                               const std::string& d, const std::string& e) {
  return _join_paths(_join_paths(_join_paths(_join_paths(a, b), c), d), e);
}

template <class... Args>
inline std::string join_paths(Args&&... args) {
  return make_canonical(_join_paths(std::forward<Args>(args)...));
}

inline bool is_absolute(const char* x) { return x[0] == '/'; }

inline bool is_absolute(const std::string& x) { return is_absolute(x.c_str()); }

inline bool is_relative(const char* x) { return !is_absolute(x); }

inline bool is_relative(const std::string& x) { return !is_absolute(x.c_str()); }

// join takes a sequence of strings and concats that
// sequence with some seperator between it. It's like
// python's join method on strings. So ", ".join(seq)
// in python joins a list of strings with a comma. This
// function is a C++ equivlent.
template <class Iter>
inline std::string join(char sep, Iter begin, Iter end) {
  std::string out;
  for (; begin != end; ++begin) {
    out += *begin;
    auto begin_copy = begin;
    ++begin_copy;
    if (begin_copy != end) out += sep;
  }
  return out;
}

// Returns all the component parts of the given path.
inline std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> path_vec;
  for (std::string node : wcl::make_filepath_range_ref(path)) {
    path_vec.emplace_back(std::move(node));
  }

  return path_vec;
}

inline std::string relative_to(std::string relative, std::string path) {
  // TODO: Assert that relative is absolute
  // First make the path canonical.
  path = make_canonical(path);

  // If the path is relative then we can just return it as we don't
  // know what its relative to so we assume its already relative to
  // `relative`
  if (wcl::is_relative(path)) {
    return path;
  }
  relative = make_canonical(relative);

  // Since we now know that the path is absolute and canonical it must have
  // no special parts like .. or . in it. By iterating parts of both until they stop
  // matching we can eliminate as much of `relative` as possible and then prepend
  // `..` for each remaining element of `relative` to get the relative path.
  auto path_range = make_filepath_range(path);
  auto rel_range = make_filepath_range(relative);
  auto path_begin = path_range.begin();
  auto rel_begin = rel_range.begin();
  while (path_begin != path_range.end() && rel_begin != rel_range.end() &&
         *path_begin == *rel_begin) {
    ++path_begin;
    ++rel_begin;
  }

  // Now prepend the ".." paths
  std::string out;
  while (rel_begin != rel_range.end()) {
    out += "../";
    ++rel_begin;
  }

  // And finally append the remainder of path.
  out += join('/', path_begin, path_range.end());

  return out;
}

}  // namespace wcl
