/*
 * Copyright 2023 SiFive, Inc.
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

#include "filepath.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "optional.h"

namespace wcl {

// assumes that type != DT_UNKNOWN
static file_type dir_type_conv(unsigned char type) {
  switch (type) {
    case DT_BLK:
      return file_type::block;
    case DT_CHR:
      return file_type::character;
    case DT_DIR:
      return file_type::directory;
    case DT_FIFO:
      return file_type::fifo;
    case DT_LNK:
      return file_type::symlink;
    case DT_REG:
      return file_type::regular;
    case DT_SOCK:
      return file_type::socket;
  }
  return file_type::unknown;
}

static file_type stat_type_conv(mode_t mode) {
  switch (mode & S_IFMT) {
    case S_IFBLK:
      return file_type::block;
    case S_IFCHR:
      return file_type::character;
    case S_IFDIR:
      return file_type::directory;
    case S_IFIFO:
      return file_type::fifo;
    case S_IFLNK:
      return file_type::symlink;
    case S_IFREG:
      return file_type::regular;
    case S_IFSOCK:
      return file_type::socket;
  }
  return file_type::unknown;
}

void directory_iterator::step() {
  // checking for errors with readdir is a bit tricky.
  // you have to set errno to zero and see if it changes.
  errno = 0;
  dirent *entry = readdir(dir);
  if (entry == nullptr && errno != 0) {
    value = some(make_errno<directory_entry>());
    return;
  }

  // if errno is still zero but entry == nullptr then
  // we've hit the end and want to turn outselves into
  // the end pointer
  if (entry == nullptr) {
    dir = nullptr;
    dir_path = "";
    // We set value not just to empty but to an error for EBADF.
    // This mimics what would happen if you called readdir again
    // after passing the end of the DIR
    value = some(make_error<directory_entry, posix_error_t>(EBADF));
    return;
  }

  // Now that we have a good entry we need to construct
  // the wrapper
  directory_entry out;
  out.name = entry->d_name;

  // d_name might be missing so we stat if that
  // occurs.
  if (entry->d_type == DT_UNKNOWN) {
    std::string path = join_paths(dir_path, out.name);
    struct stat buf;
    stat(path.c_str(), &buf);
    out.type = stat_type_conv(buf.st_mode);
  } else {
    out.type = dir_type_conv(entry->d_type);
  }

  value = some(make_result<directory_entry, posix_error_t>(std::move(out)));
}

}  // namespace wcl
