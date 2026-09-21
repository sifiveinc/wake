/* Wake FUSE launcher to capture inputs/outputs
 *
 * Copyright 2021 SiFive, Inc.
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

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "gopt/gopt-arg.h"
#include "gopt/gopt.h"
#include "json/json5.h"
#include "util/execpath.h"
#include "util/shell.h"
#include "wakefs/fuse.h"
#include "wakefs/materialize_staging.h"

namespace fs = std::filesystem;

void print_help() {
  const std::string interactive =
      "Interactive options                                                                       \n"
      "    -r --rootfs FILE         Use a squashfs file as the command's view of the root        \n"
      "                             filesystem.                                                  \n"
      "    -t --toolchain FILE      Make a toolchain visible on the command's view of the        \n"
      "                             filesystem.                                                  \n"
      "                             May be specified multiple times.                             \n"
      "    -b --bind DIR1:DIR2      Place the directory (or file) at DIR1 within the command's   \n"
      "                             view of the filesystem at location DIR2.                     \n"
      "                             May be specified multiple times.                             \n"
      "    -x                       Shorthand for '--bind $PWD:$PWD'                             \n"
      "    COMMAND                  The command to run.                                          \n"
      "";

  const std::string batch_and_help =
      "Batch options                                                                             \n"
      "    -p --params FILE         Json file specifying input parameters.                       \n"
      "    -o --output-stats FILE   Json file written containing output results and return code. \n"
      "    -s --force-shell         Run shell instead of command from params file.               \n"
      "                             Implies --allow-interactive.                                 \n"
      "                             Use 'eval $WAKEBOX_CMD' to run the command from params file. \n"
      "    -i --allow-interactive   Use default stdin, ignoring the params json file's stdin     \n"
      "                             value.                                                       \n"
#ifdef __linux__
      "    -b --bind DIR1:DIR2      Place the directory (or file) at DIR1 within the command's   \n"
      "                             view of the filesystem at location DIR2.                     \n"
      "                             May be specified multiple times.                             \n"
      "                             These are applied after the params file mount ops.           \n"
#endif
      "    -I --isolate-retcode     Don't allow COMMAND's return code to impact wakebox's return \n"
      "                             code.                                                        \n"
      "    -m --materialize-staging Materialize the staged files, and consumes them immediately  \n"
      "                             after the job specified by --params is complete. Requires    \n"
      "                             the --params argument.                                       \n"
      "    -P --materialize-previous RUN-ID                                                      \n"
      "                             Materialize only the staging files with the specified runID. \n"
      "    -M --materialize-manifest PATH                                                        \n"
      "                             Materialize only the staging files specified by the recovery \n"
      "                             manifest path.                                               \n"
      "                                                                                          \n"
      "Other options                                                                             \n"
      "    -h --help                Print usage                                                  \n"
      "";

#ifdef __linux__
  std::cout << "Usage: wakebox [OPTIONS] [COMMAND...]\n\n" << interactive << "\n" << batch_and_help;
#else
  std::cout
      << "Usage: wakebox [OPTIONS]\n\n"
      << "NOTE: Reduced command line options due to operating system support.\n"
      << "      Mount options, uid/gid control and network isolation in the input parameters file\n"
      << "      will be ignored.\n\n"
      << batch_and_help;
#endif
}

// Use the directory where wakebox was invoked as the recovery workspace.
bool resolve_workspace(std::string* workspace, std::string* error) {
  std::error_code ec;
  const fs::path current = fs::canonical(fs::current_path(), ec);
  if (ec) {
    *error = "canonicalize workspace: " + ec.message();
    return false;
  }
  if (!fs::is_directory(current, ec)) {
    *error = ec ? "inspect workspace: " + ec.message()
                : "workspace is not a directory: " + current.string();
    return false;
  }
  *workspace = current.string();
  return true;
}

bool discover_completed_manifests(const std::string& workspace,
                                  std::vector<wakefs::CompletedStagingManifest>* manifests,
                                  std::string* error) {
  const fs::path recovery = fs::path(workspace) / ".build" / "cas" / "staging" / "recovery";
  return wakefs::discover_completed_staging_manifests(recovery.string(), manifests, error);
}

void print_materialization_summary(const std::string& manifest_path,
                                   const wakefs::StagingMaterializationSummary& summary) {
  std::cout << manifest_path << ": materialized " << summary.materialized << ", consumed "
            << summary.consumed << ", failed " << summary.failed
            << (summary.manifest_removed ? ", manifest removed" : ", manifest retained") << std::endl;
}

bool parse_run_id(const char* text, int64_t* run_id) {
  if (!text || !*text) return false;
  try {
    size_t parsed = 0;
    const long long value = std::stoll(text, &parsed);
    if (parsed != std::strlen(text) || value < std::numeric_limits<int64_t>::min() ||
        value > std::numeric_limits<int64_t>::max())
      return false;
    *run_id = value;
    return true;
  } catch (...) {
    return false;
  }
}

int materialize_previous_workspace(const char* requested_run_id) {
  int64_t run_id;
  if (!parse_run_id(requested_run_id, &run_id)) {
    std::cerr << "--materialize-previous requires an integer Wake run ID." << std::endl;
    return 1;
  }
  std::string workspace;
  std::string error;
  if (!resolve_workspace(&workspace, &error)) {
    std::cerr << error << std::endl;
    return 1;
  }

  std::vector<wakefs::CompletedStagingManifest> manifests;
  if (!discover_completed_manifests(workspace, &manifests, &error)) {
    std::cerr << error << std::endl;
    return 1;
  }
  bool success = true;
  size_t selected = 0;
  for (const wakefs::CompletedStagingManifest& manifest : manifests) {
    if (!manifest.manifest.wake_run_id || *manifest.manifest.wake_run_id != run_id) continue;
    ++selected;
    if (manifest.manifest.workspace_root != workspace) {
      std::cerr << manifest.path << ": recorded workspace " << manifest.manifest.workspace_root
                << " does not match selected workspace " << workspace << std::endl;
      success = false;
      continue;
    }
    wakefs::StagingMaterializationSummary summary;
    std::string materialize_error;
    if (!wakefs::materialize_completed_workspace(manifest.path, manifest.manifest, &summary,
                                                  &materialize_error)) {
      success = false;
      std::cerr << manifest.path << ": "
                << (materialize_error.empty() ? "one or more entries failed" : materialize_error)
                << std::endl;
    }
    print_materialization_summary(manifest.path, summary);
  }
  if (selected == 0)
    std::cout << "No recovery manifests match Wake run " << run_id << " in " << workspace << std::endl;
  return success ? 0 : 1;
}

int materialize_manifest(const char* path) {
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(path, ec);
  if (ec || !fs::is_regular_file(status)) {
    std::cerr << (ec ? "inspect recovery manifest: " + ec.message()
                     : "recovery manifest is not a regular file")
              << std::endl;
    return 1;
  }
  wakefs::StagingManifest manifest;
  std::string error;
  if (!wakefs::read_staging_manifest(path, &manifest, &error)) {
    std::cerr << path << ": " << error << std::endl;
    return 1;
  }
  wakefs::StagingMaterializationSummary summary;
  const bool success = wakefs::materialize_completed_workspace(path, manifest, &summary, &error);
  if (!success)
    std::cerr << path << ": "
              << (error.empty() ? "one or more entries failed" : error) << std::endl;
  print_materialization_summary(path, summary);
  return success ? 0 : 1;
}

struct ImmediateMaterialization {
  bool success = false;
  bool manifest_removed = false;
  size_t materialized = 0;
  size_t consumed = 0;
  size_t failed = 0;
  std::string manifest_path;
  std::string error;
};

ImmediateMaterialization materialize_returned_manifest(const fuse_args& args,
                                                       const std::string& result_json) {
  ImmediateMaterialization result;
  std::stringstream parse_errors;
  JAST metadata;
  if (!JAST::parse(result_json, parse_errors, metadata) || metadata.kind != JSON_OBJECT) {
    result.error = "parse wakebox result metadata";
    return result;
  }
  auto manifest_field = metadata.get_opt("recovery_manifest");
  if (!manifest_field || (*manifest_field)->kind != JSON_STR || (*manifest_field)->value.empty()) {
    result.error = "wakebox result does not contain a recovery manifest";
    return result;
  }
  result.manifest_path = (*manifest_field)->value;

  std::error_code ec;
  const fs::path workspace = fs::canonical(args.working_dir, ec);
  if (ec) {
    result.error = "canonicalize workspace: " + ec.message();
    return result;
  }
  fs::path cas_root = args.cas_dir;
  if (cas_root.is_relative()) cas_root = workspace / cas_root;
  cas_root = fs::canonical(cas_root, ec);
  if (ec) {
    result.error = "canonicalize CAS root: " + ec.message();
    return result;
  }
  const fs::path recovery_dir = cas_root / "staging" / "recovery";
  const fs::path manifest = fs::path(result.manifest_path);
  const fs::file_status status = fs::symlink_status(manifest, ec);
  if (ec || !fs::is_regular_file(status)) {
    result.error = ec ? "inspect recovery manifest: " + ec.message()
                      : "recovery manifest is not a regular file";
    return result;
  }
  const fs::path manifest_parent = fs::canonical(manifest.parent_path(), ec);
  if (ec || manifest_parent != recovery_dir) {
    result.error = ec ? "canonicalize recovery manifest directory: " + ec.message()
                      : "recovery manifest is outside the recovery directory";
    return result;
  }

  wakefs::StagingManifest parsed;
  if (!wakefs::read_staging_manifest(result.manifest_path, &parsed, &result.error)) return result;
  if (parsed.workspace_root != workspace.string() ||
      parsed.cas_staging_root != (cas_root / "staging").string()) {
    result.error = "recovery manifest roots do not match this wakebox invocation";
    return result;
  }

  wakefs::StagingMaterializationSummary summary;
  result.success =
      wakefs::materialize_completed_workspace(result.manifest_path, parsed, &summary, &result.error);
  result.materialized = summary.materialized;
  result.consumed = summary.consumed;
  result.failed = summary.failed;
  result.manifest_removed = summary.manifest_removed;
  if (result.success) result.error.clear();
  return result;
}

// Decide the default working directory for the new process.
// Wakebox does not provide direct control of the command running dir at this time.
const std::string pick_running_dir(const struct fuse_args &fa) {
#ifdef __linux__
  // If we have a workspace mount, we want to default to that location.
  for (auto &x : fa.mount_ops) {
    if (x.type == "workspace") {
      if (x.destination[0] == '/')
        return x.destination + "/" + fa.directory;
      else
        // convert a workspace relative path into absolute path
        return fa.working_dir + "/" + x.destination + "/" + fa.directory;
    }
  }
  // If we're binding in the parent namespace's current working directory, use that.
  for (auto &x : fa.mount_ops)
    if (x.type == "bind" && fa.working_dir == x.source) return x.destination + "/" + fa.directory;

  // If we have a replacement rootfs, we know we atleast have "/".
  for (auto &x : fa.mount_ops)
    if (x.destination == "/") return "/" + fa.directory;

  // Try the current directory, which should exist if we have no replacement rootfs.
  return fa.working_dir + "/" + fa.directory;
#else
  // On non-linux platforms like MacOS, run_in_fuse is unable to re-map the fuse
  // mountpoint over the top of the original workspace.
  // It may expose the temporary fuse mountpoint as a component of absolute paths.
  return fa.daemon.mount_subdir + "/" + fa.directory;
#endif
}

// Interactive mode does not provide userid control at this time.
// Allows networking by default, no user control yet.
int run_interactive(const std::string &rootfs, const std::vector<std::string> &toolchains,
                    const std::vector<mount_op> &binds, const std::vector<std::string> &command) {
  struct fuse_args fa(get_cwd(), false);
  fa.command = command;

  if (!rootfs.empty()) fa.mount_ops.push_back({"squashfs", rootfs, "/", false});

  for (auto &tool : toolchains) fa.mount_ops.push_back({"squashfs", tool, "", false});

  fa.mount_ops.insert(fa.mount_ops.end(), binds.begin(), binds.end());

  if (rootfs.empty()) {
    fa.environment.push_back(std::string("HOME=") + std::getenv("HOME"));
    fa.environment.push_back(std::string("USER=") + std::getenv("USER"));
  }

  const char *term = std::getenv("TERM");
  fa.environment.push_back(std::string("TERM=") + term);

  fa.command_running_dir = pick_running_dir(fa);

  int retcode;
  std::string result;
  if (!run_in_fuse(fa, retcode, result)) return 1;
  return retcode;
}

int run_batch(const char *params_path, bool has_output, bool use_stdin_file, bool use_shell,
              bool isolate_retcode, bool materialize_staging, const char *result_path,
              const std::vector<mount_op> &binds) {
  // Read the params file
  std::ifstream ifs(params_path);
  const std::string json((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
  if (ifs.fail()) {
    std::cerr << "read " << params_path << ": " << strerror(errno) << std::endl;
    return 1;
  }
  ifs.close();

  fuse_args args(get_cwd(), use_stdin_file);
  if (!json_as_struct(json, args)) {
    std::cerr << "Failed to process spec '" << params_path << "'. Verify valid json and schema."
              << std::endl;
    return 1;
  }

  args.command_running_dir = pick_running_dir(args);

  if (args.command.empty() || args.command[0].empty()) {
    std::cerr << "No command was provided." << std::endl;
    return 1;
  }

  // Append CLI-specified binds after params file binds.
  args.mount_ops.insert(args.mount_ops.end(), binds.begin(), binds.end());

  if (use_shell) {
    std::stringstream ss;
    ss << "WAKEBOX_CMD=";
    for (auto &s : args.command) ss << shell_escape(s) << " ";
    std::string escaped = ss.str();
    args.environment.push_back(escaped.substr(0, escaped.length() - 1));

    args.use_stdin_file = false;
    args.command = {"/bin/sh"};
    std::cerr << "To execute the original command:\n\teval $WAKEBOX_CMD" << std::endl;
  }

  int retcode;
  std::string result;
  if (!has_output) {
    if (!run_in_fuse(args, retcode, result)) return 1;
    if (materialize_staging) {
      const ImmediateMaterialization materialization = materialize_returned_manifest(args, result);
      if (!materialization.success) {
        std::cerr << "materialize staging: " << materialization.error << std::endl;
        if (retcode == 0) return 1;
      } else {
        wakefs::StagingMaterializationSummary summary;
        summary.materialized = materialization.materialized;
        summary.consumed = materialization.consumed;
        summary.failed = materialization.failed;
        summary.manifest_removed = materialization.manifest_removed;
        print_materialization_summary(materialization.manifest_path, summary);
      }
    }

    if (isolate_retcode)
      return 0;
    else
      return retcode;
  }

  // Open the output file
  int out_fd = open(result_path, O_WRONLY | O_CREAT | O_CLOEXEC | O_TRUNC, 0664);
  if (out_fd < 0) {
    std::cerr << "open " << result_path << ": " << strerror(errno) << std::endl;
    return 1;
  }

  if (!run_in_fuse(args, retcode, result)) return 1;

  ImmediateMaterialization materialization;
  if (materialize_staging) {
    materialization = materialize_returned_manifest(args, result);
    if (!materialization.success) {
      std::cerr << "materialize staging: " << materialization.error << std::endl;
    } else {
      wakefs::StagingMaterializationSummary summary;
      summary.materialized = materialization.materialized;
      summary.consumed = materialization.consumed;
      summary.failed = materialization.failed;
      summary.manifest_removed = materialization.manifest_removed;
      print_materialization_summary(materialization.manifest_path, summary);
    }
  }

  // write output stats as json
  ssize_t wrote = write(out_fd, result.c_str(), result.length());
  if (wrote == -1) return errno;
  if (0 != close(out_fd)) return errno;

  if (materialize_staging && !materialization.success && retcode == 0)
    return 1;
  else if (isolate_retcode)
    return 0;
  else
    return retcode;
}

int main(int argc, char *argv[]) {
  unsigned int max_pairs = argc / 2;
  std::vector<char *> tools(max_pairs, nullptr);
  std::vector<char *> binds(max_pairs, nullptr);

  struct option options[] {
#ifdef __linux__
    {'r', "rootfs", GOPT_ARGUMENT_REQUIRED},
        {'t', "toolchain", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, tools.data(), max_pairs},
        {'b', "bind", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, binds.data(), max_pairs},
        {'x', "bind-cwd", GOPT_ARGUMENT_FORBIDDEN},
#endif
        {'p', "params", GOPT_ARGUMENT_REQUIRED}, {'o', "output-stats", GOPT_ARGUMENT_REQUIRED},
        {'s', "force-shell", GOPT_ARGUMENT_FORBIDDEN},
        {'i', "interactive", GOPT_ARGUMENT_FORBIDDEN},
        {'I', "isolate-retcode", GOPT_ARGUMENT_FORBIDDEN},
        {'m', "materialize-staging", GOPT_ARGUMENT_FORBIDDEN},
        {'P', "materialize-previous", GOPT_ARGUMENT_REQUIRED},
        {'M', "materialize-manifest", GOPT_ARGUMENT_REQUIRED},

        {'h', "help", GOPT_ARGUMENT_FORBIDDEN}, {
      0, 0, GOPT_LAST
    }
  };

  argc = gopt(argv, options);
  gopt_errors("wakebox", options);

  bool has_help = arg(options, "help")->count > 0;
  bool has_params_file = arg(options, "params")->count > 0;
  bool has_positional_cmd = argc > 1;
  bool isolate_retcode = arg(options, "isolate-retcode")->count > 0;
  bool materialize_staging = arg(options, "materialize-staging")->count > 0;
  bool materialize_previous = arg(options, "materialize-previous")->count > 0;
  bool materialize_manifest_path = arg(options, "materialize-manifest")->count > 0;

  if (has_help) {
    print_help();
    return 1;
  }

  if (materialize_staging && !has_params_file) {
    std::cerr << "--materialize-staging requires --params." << std::endl;
    return 1;
  }

  if (materialize_previous || materialize_manifest_path) {
    bool has_execution_options = has_params_file || has_positional_cmd || materialize_staging ||
                                 arg(options, "output-stats")->count > 0 || isolate_retcode ||
                                 arg(options, "force-shell")->count > 0 ||
                                 arg(options, "interactive")->count > 0;
#ifdef __linux__
    has_execution_options = has_execution_options || arg(options, "rootfs")->count > 0 ||
                            arg(options, "toolchain")->count > 0 || arg(options, "bind")->count > 0 ||
                            arg(options, "bind-cwd")->count > 0;
#endif
    if (materialize_previous && materialize_manifest_path) {
      std::cerr << "Choose only one recovery command." << std::endl;
      return 1;
    }
    if (has_execution_options) {
      std::cerr << "Recovery commands cannot be combined with payload execution options." << std::endl;
      return 1;
    }
    if (materialize_previous)
      return materialize_previous_workspace(arg(options, "materialize-previous")->argument);
    return materialize_manifest(arg(options, "materialize-manifest")->argument);
  }

  if (has_positional_cmd && has_params_file) {
    std::cerr << "The batch mode --params argument can't be used with the interactive"
                 " mode command argument."
              << std::endl;
    return 1;
  }

  std::vector<mount_op> bind_ops;
#ifdef __linux__
  for (unsigned int i = 0; i < arg(options, "bind")->count; i++) {
    const std::string s = binds[i];
    std::string source = s.substr(0, s.find(":"));
    std::string destination = s.substr(s.find(":") + 1);
    if (source.empty() || destination.empty() || s.find(":") == std::string::npos) {
      std::cerr << "Invalid bind: " << s << std::endl;
      return 1;
    }
    bind_ops.push_back({"bind", source, destination});
  }
#endif

  if (has_positional_cmd) {
    std::string rootfs;
    if (arg(options, "rootfs")->count > 0) rootfs = arg(options, "rootfs")->argument;

    std::vector<std::string> toolchains;
    for (unsigned int i = 0; i < arg(options, "toolchain")->count; i++)
      toolchains.push_back(tools[i]);
    if (arg(options, "bind-cwd")->count > 0) {
      std::string cwd = get_cwd();
      bind_ops.push_back({"create-dir", "", cwd});
      bind_ops.push_back({"bind", cwd, cwd});
    }

    std::vector<std::string> positional_params;
    for (int i = 1; i < argc; i++) positional_params.push_back(argv[i]);
    if (positional_params.empty()) {
      std::cerr << "Must provide a command." << std::endl;
      return 1;
    }
    return run_interactive(rootfs, toolchains, bind_ops, positional_params);

  } else if (has_params_file) {
    const char *params = arg(options, "params")->argument;
    bool has_output = arg(options, "output-stats")->count > 0;
    bool use_stdin_file = arg(options, "interactive")->count == 0;
    bool use_shell = arg(options, "force-shell")->count > 0;
    const char *result_path = arg(options, "output-stats")->argument;
    return run_batch(params, has_output, use_stdin_file, use_shell, isolate_retcode,
                     materialize_staging, result_path, bind_ops);
  }
  print_help();
  return 1;
}
