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

// OS/X only makes st_mtimespec available as an extension
#define _DARWIN_C_SOURCE 1

#include "mtime.h"

#include <sys/stat.h>

#ifdef __APPLE__
#define st_mtim st_mtimespec
#endif

int64_t getmtime_ns(const char *file) {
  struct stat sbuf;
  int ret = lstat(file, &sbuf);
  if (ret == -1) return -1;
  return sbuf.st_mtim.tv_nsec + sbuf.st_mtim.tv_sec * INT64_C(1000000000);
}
