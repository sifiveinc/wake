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

#ifndef ATTACH_H
#define ATTACH_H

#include "runtime/database.h"

// Attach a read/write shell to the live FUSE sandbox of a still-running job.
//
// Linux only, and only for jobs the calling user launched: it works entirely
// unprivileged (no helper, no capabilities) by fork()ing a single-threaded
// child that setns() into the job's own user+mount namespaces, which the
// kernel permits because the caller's euid owns those namespaces. The view is
// read/write -- a kernel-enforced read-only view would need CAP_SYS_ADMIN the
// owner doesn't have; see the design doc revision for why that was abandoned.
// Filesystem view only (no pid-namespace/process interaction). Returns the
// attached shell's exit status, or EXIT_FAILURE on error, for easy CLI use.
int attach_job(Database &db, long job_id);

#endif
