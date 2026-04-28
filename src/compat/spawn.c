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

// vfork is a deprecated API in POSIX, but is defined in BSD
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "spawn.h"

#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <signal.h>
#include <sys/prctl.h>
#endif

pid_t wake_spawn(const char *cmd, char **cmdline, char **environ) {
  pid_t parent = getpid();
  (void)parent;
  pid_t pid = vfork();
  if (pid == 0) {
#ifdef __linux__
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) _exit(1);
    if (getppid() != parent) _exit(2);
#endif
    execve(cmdline[0], cmdline, environ);
    _exit(127);
  }
  return pid;
}
