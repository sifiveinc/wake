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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "wakebox_prim.h"

#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json/json5.h"
#include "prim.h"
#include "record_helpers.h"
#include "tuple.h"
#include "types/data.h"
#include "types/datatype.h"
#include "types/sums.h"
#include "value.h"

// ---------------------------------------------------------------------------
// WakeboxSpec field layout (must match the tuple declaration order in Wake):
//
//   WakeboxSpec =
//     [0]  Label            : String
//     [1]  Command          : List String
//     [2]  Environment      : List String
//     [3]  Visible          : List Path
//     [4]  Directory        : String
//     [5]  Stdin            : String
//     [6]  CasDir           : String
//     [7]  IsolateNetwork   : Boolean
//     [8]  IsolatePids      : Boolean
//     [9]  MountOps         : List WakeboxMountOp
//     [10] Hostname         : String
//     [11] Domainname       : String
//     [12] UserId           : Option Integer
//     [13] GroupId          : Option Integer
//     [14] CommandTimeout   : Option Integer
//     [15] Runner           : String
//
//   Path (from io.wake / path.wake) =
//     [0] Name    : String
//     [1] Type    : PathType   -- ADT, mapped to string via cons->index (see below)
//     [2] Hash    : String
//     [3] Mode    : Integer
//     [4] MtimeNs : Integer
//
//   PathType constructor indices (declaration order in io.wake):
//     0 = PathTypeRegularFile  -> "file"
//     1 = PathTypeDirectory    -> "directory"
//     2 = PathTypeCharacterDevice (unsupported by wakebox)
//     3 = PathTypeBlockDevice     (unsupported by wakebox)
//     4 = PathTypeFifo            (unsupported by wakebox)
//     5 = PathTypeSymlink      -> "symlink"
//     6 = PathTypeSocket          (unsupported by wakebox)
//
//   WakeboxMountOp constructor indices (declaration order in runner.wake):
//     0 = WakeboxBindOp(source:String, dest:String, readonly:Boolean)
//     1 = WakeboxSquashfsOp(source:String, dest:String)
//     2 = WakeboxTmpfsOp(dest:String)
//     3 = WakeboxCreateDirOp(dest:String)
//     4 = WakeboxCreateFileOp(dest:String)
//     5 = WakeboxFuseWorkspaceOp(dest:String)
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Collected plain-C++ representation of a WakeboxSpec
// ---------------------------------------------------------------------------

struct VisEntry {
  std::string path;
  std::string type;
  std::string hash;
  long mode;
  long mtime;
};

struct MountEntry {
  std::string type;
  std::string source;  // empty = omit from JSON
  std::string dest;
  bool read_only = false;
  bool has_read_only = false;  // only WakeboxBindOp carries readonly
};

struct SpecData {
  // Set to a non-empty string on a validation error (e.g. unsupported path type).
  // collect_spec returns nullptr in this case — callers must check this field.
  std::string error;

  std::string label;
  std::vector<std::string> command;
  std::vector<std::string> environment;
  std::vector<VisEntry> visible;
  std::string directory;
  std::string stdin_str;
  std::string cas_dir;
  bool isolate_network;
  bool isolate_pids;
  std::vector<MountEntry> mounts;
  std::string hostname;
  std::string domainname;
  std::string runner;
  bool has_user_id = false;
  bool has_group_id = false;
  bool has_command_timeout = false;
  long user_id = 0;
  long group_id = 0;
  long command_timeout = 0;
};

// Collect all fields from a WakeboxMountOp ADT value into a MountEntry.
// WakeboxMountOp constructor indices (runner.wake declaration order):
//   0=WakeboxBindOp(source,dest,readonly), 1=WakeboxSquashfsOp(source,dest),
//   2=WakeboxTmpfsOp(dest), 3=WakeboxCreateDirOp(dest),
//   4=WakeboxCreateFileOp(dest), 5=WakeboxFuseWorkspaceOp(dest)
static Promise *collect_mount_entry(Record *entry, MountEntry &me) {
  static const char *strings[] = {"bind",       "squashfs",    "tmpfs",
                                  "create-dir", "create-file", "workspace"};
  int idx = entry->cons->index;
  if (idx < 0 || idx >= 6) return nullptr;
  me.type = strings[idx];

  Promise *p;
  switch (idx) {
    case 0: {  // WakeboxBindOp(source, dest, readonly)
      p = entry->at(0);
      if (!*p) return p;
      me.source = p->coerce<String>()->as_str();
      p = entry->at(1);
      if (!*p) return p;
      me.dest = p->coerce<String>()->as_str();
      p = entry->at(2);
      if (!*p) return p;
      me.read_only = (p->coerce<Record>()->cons == &Boolean->members[0]);
      me.has_read_only = true;
      break;
    }
    case 1: {  // WakeboxSquashfsOp(source, dest)
      p = entry->at(0);
      if (!*p) return p;
      me.source = p->coerce<String>()->as_str();
      p = entry->at(1);
      if (!*p) return p;
      me.dest = p->coerce<String>()->as_str();
      break;
    }
    default: {  // TmpfsOp, CreateDirOp, CreateFileOp, FuseWorkspaceOp — only dest
      p = entry->at(0);
      if (!*p) return p;
      me.dest = p->coerce<String>()->as_str();
      break;
    }
  }
  return nullptr;
}

// Map a PathType ADT record to its wakebox JSON string.
// Returns nullptr and sets `out` on success.
// Returns nullptr and sets `error` for unsupported types.
// Returns the unfulfilled Promise if the PathType field is not yet ready.
static Promise *get_path_type_string(Record *path_rec, std::string &out, std::string &error) {
  // PathType is at field index 1 of the Path tuple.
  Promise *p = path_rec->at(1);
  if (!*p) return p;
  Record *pt = p->coerce<Record>();
  // PathType constructor indices (io.wake declaration order):
  //   0=RegularFile, 1=Directory, 2=CharDevice, 3=BlockDevice,
  //   4=Fifo,        5=Symlink,   6=Socket
  static const char *strings[] = {"file",  "directory", nullptr, nullptr,
                                  nullptr, "symlink",   nullptr};
  int idx = pt->cons->index;
  if (idx < 0 || idx >= 7 || strings[idx] == nullptr) {
    // Get the constructor name for a useful error message.
    error = "write_wakebox_spec: unsupported path type '" + pt->cons->ast.name +
            "' for wakebox (only file, directory, symlink are valid)";
    return nullptr;
  }
  out = strings[idx];
  return nullptr;
}

// Try to collect all values from the WakeboxSpec record into plain C++ data.
// Returns nullptr if everything is fulfilled, or the first unfulfilled Promise.
// The caller must discard and reinitialise `data` on every retry since a
// partial traversal may have partially filled vectors.
static Promise *collect_spec(Record *spec, SpecData &data) {
  Promise *broken;

#define TRY(expr)              \
  do {                         \
    broken = (expr);           \
    if (broken) return broken; \
  } while (0)

  TRY(rh::string(spec, 0, data.label));
  TRY(rh::string_list(spec, 1, data.command));
  TRY(rh::string_list(spec, 2, data.environment));

  // Field 3: Visible — List Path
  // Path layout: [0]=Name:String, [1]=Type:PathType, [2]=Hash:String,
  //              [3]=Mode:Integer, [4]=MtimeNs:Integer
  {
    Promise *p3 = spec->at(3);
    if (!*p3) return p3;
    Record *list = p3->coerce<Record>();
    while (list->cons == &List->members[1]) {  // Cons
      Promise *head = list->at(0);
      if (!*head) return head;
      Record *path = head->coerce<Record>();

      VisEntry ve;
      TRY(rh::string(path, 0, ve.path));
      {
        Promise *broken = get_path_type_string(path, ve.type, data.error);
        if (broken) return broken;
        if (!data.error.empty()) return nullptr;
      }
      TRY(rh::string(path, 2, ve.hash));
      TRY(rh::integer(path, 3, ve.mode));
      TRY(rh::integer(path, 4, ve.mtime));
      data.visible.push_back(std::move(ve));

      Promise *tail = list->at(1);
      if (!*tail) return tail;
      list = tail->coerce<Record>();
    }
  }

  TRY(rh::string(spec, 4, data.directory));
  TRY(rh::string(spec, 5, data.stdin_str));
  TRY(rh::string(spec, 6, data.cas_dir));
  TRY(rh::boolean(spec, 7, data.isolate_network));
  TRY(rh::boolean(spec, 8, data.isolate_pids));

  // Field 9: MountOps — List WakeboxMountOp
  {
    Promise *p9 = spec->at(9);
    if (!*p9) return p9;
    Record *list = p9->coerce<Record>();
    while (list->cons == &List->members[1]) {  // Cons
      Promise *head = list->at(0);
      if (!*head) return head;
      Record *entry = head->coerce<Record>();

      MountEntry me;
      TRY(collect_mount_entry(entry, me));
      data.mounts.push_back(std::move(me));

      Promise *tail = list->at(1);
      if (!*tail) return tail;
      list = tail->coerce<Record>();
    }
  }

  TRY(rh::string(spec, 10, data.hostname));
  TRY(rh::string(spec, 11, data.domainname));
  TRY(rh::opt_integer(spec, 12, data.has_user_id, data.user_id));
  TRY(rh::opt_integer(spec, 13, data.has_group_id, data.group_id));
  TRY(rh::opt_integer(spec, 14, data.has_command_timeout, data.command_timeout));
  TRY(rh::string(spec, 15, data.runner));

#undef TRY

  return nullptr;  // all fields fulfilled
}

static void write_jast_pretty(std::ostream &os, const JAST &jast, int indent, int depth) {
  switch (jast.kind) {
    case JSON_OBJECT: {
      if (jast.children.empty()) {
        os << "{}";
        return;
      }
      std::string inner(static_cast<size_t>((depth + 1) * indent), ' ');
      std::string outer(static_cast<size_t>(depth * indent), ' ');
      os << "{\n";
      for (size_t i = 0; i < jast.children.size(); ++i) {
        os << inner << '"' << json_escape(jast.children[i].first) << "\": ";
        write_jast_pretty(os, jast.children[i].second, indent, depth + 1);
        if (i + 1 < jast.children.size()) os << ',';
        os << '\n';
      }
      os << outer << '}';
      break;
    }
    case JSON_ARRAY: {
      if (jast.children.empty()) {
        os << "[]";
        return;
      }
      std::string inner(static_cast<size_t>((depth + 1) * indent), ' ');
      std::string outer(static_cast<size_t>(depth * indent), ' ');
      os << "[\n";
      for (size_t i = 0; i < jast.children.size(); ++i) {
        os << inner;
        write_jast_pretty(os, jast.children[i].second, indent, depth + 1);
        if (i + 1 < jast.children.size()) os << ',';
        os << '\n';
      }
      os << outer << ']';
      break;
    }
    default:
      os << jast;
      break;
  }
}

// Build a JAST from fully-collected SpecData and write it to filepath.
// Returns an empty string on success, or an error message.
static std::string write_spec_json(const SpecData &d, const char *filepath, int indent) {
  JAST root(JSON_OBJECT);

  root.add("label", d.label);

  JAST &cmd = root.add("command", JSON_ARRAY);
  for (const auto &s : d.command) cmd.add(s);

  JAST &env = root.add("environment", JSON_ARRAY);
  for (const auto &s : d.environment) env.add(s);

  JAST &vis = root.add("visible", JSON_ARRAY);
  for (const auto &ve : d.visible) {
    JAST &obj = vis.add(JSON_OBJECT);
    obj.add("path", ve.path);
    obj.add("type", ve.type);
    obj.add("hash", ve.hash);
    obj.add("mode", (long)ve.mode);
    obj.add("mtime", (long long)ve.mtime);
  }

  root.add("directory", d.directory);
  root.add("stdin", d.stdin_str);
  root.add("cas-dir", d.cas_dir);
  root.add_bool("isolate-network", d.isolate_network);
  root.add_bool("isolate-pids", d.isolate_pids);

  JAST &mops = root.add("mount-ops", JSON_ARRAY);
  for (const auto &me : d.mounts) {
    JAST &obj = mops.add(JSON_OBJECT);
    obj.add("type", me.type);
    if (!me.source.empty()) obj.add("source", me.source);
    obj.add("destination", me.dest);
    if (me.has_read_only) obj.add_bool("read_only", me.read_only);
  }

  if (!d.hostname.empty()) root.add("hostname", d.hostname);
  if (!d.domainname.empty()) root.add("domainname", d.domainname);
  if (d.has_user_id) root.add("user-id", d.user_id);
  if (d.has_group_id) root.add("group-id", d.group_id);
  if (d.has_command_timeout) root.add("command-timeout", d.command_timeout);
  if (!d.runner.empty()) root.add("runner", d.runner);

  // Unlink any existing file first so we don't fail on a stale copy.
  (void)unlink(filepath);

  std::ofstream out(filepath, std::ios_base::trunc);
  if (out.fail()) {
    std::string err = "write_wakebox_spec: failed to open ";
    err += filepath;
    return err;
  }
  if (indent > 0) {
    write_jast_pretty(out, root, indent, 0);
    out << '\n';
  } else {
    out << root;
  }
  if (out.bad()) {
    std::string err = "write_wakebox_spec: failed to write ";
    err += filepath;
    return err;
  }
  return {};
}

// ---------------------------------------------------------------------------
// Continuation: WriteWakeboxJson
//
// On each execute() call, attempts to collect all fields from the spec.  If
// any Promise is still unfulfilled the continuation suspends itself on that
// Promise (next=nullptr; p->await(runtime, this)) and will be rescheduled
// when the value becomes available — the same "retry on first broken" pattern
// used by CHash in prim.cpp.  Once all fields are ready the JSON is written
// and the result Promise (held via `cont`) is fulfilled.
// ---------------------------------------------------------------------------

struct WriteWakeboxJson final : public GCObject<WriteWakeboxJson, Continuation> {
  HeapPointer<Record> spec;
  HeapPointer<String> filepath;
  HeapPointer<Continuation> cont;  // fulfiller for the prim's output slot
  int indent;                      // spaces per level (0 = compact); plain int, not GC-managed

  WriteWakeboxJson(Record *s, String *f, Continuation *c, int i)
      : spec(s), filepath(f), cont(c), indent(i) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (spec.*memberfn)(arg);
    arg = (filepath.*memberfn)(arg);
    arg = (cont.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

void WriteWakeboxJson::execute(Runtime &runtime) {
  SpecData data;
  Promise *broken = collect_spec(spec.get(), data);

  if (broken) {
    // Suspend until this Promise is fulfilled, then retry from scratch.
    next = nullptr;
    broken->await(runtime, this);
    return;
  }

  // Propagate validation errors (e.g. unsupported PathType) as Fail results.
  std::string err =
      data.error.empty() ? write_spec_json(data, filepath->c_str(), indent) : data.error;

  // Reserve heap before any allocation.
  size_t need = reserve_result() + (err.empty() ? reserve_unit() : String::reserve(err.size()));
  runtime.heap.reserve(need);

  Value *result;
  if (err.empty()) {
    result = claim_result(runtime.heap, true, claim_unit(runtime.heap));
  } else {
    result = claim_result(runtime.heap, false, String::claim(runtime.heap, err));
  }

  cont->resume(runtime, result);
}

}  // namespace

// ---------------------------------------------------------------------------
// PRIMTYPE and PRIMFN
// ---------------------------------------------------------------------------

static PRIMTYPE(type_write_wakebox_spec) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeUnit);
  result[1].unify(Data::typeString);
  // arg0 is WakeboxSpec — any record type; Wake's type checker enforces the exact type
  // at the call site via the typed wrapper function in runner.wake.
  TypeVar spec_type;
  spec_type.setDOB();  // default TypeVar ctor leaves var_dob=0; do_unify asserts it non-zero
  return args.size() == 3 && args[0]->unify(spec_type) && args[1]->unify(Data::typeInteger) &&
         args[2]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_write_wakebox_spec) {
  EXPECT(3);
  RECORD(spec, 0);
  INTEGER_MPZ(indent_mpz, 1);
  STRING(filepath, 2);

  int indent = mpz_fits_sint_p(indent_mpz) ? mpz_get_si(indent_mpz) : 0;
  if (indent < 0) indent = 0;

  // Reserve space for WriteWakeboxJson + the fulfiller continuation.
  runtime.heap.reserve(WriteWakeboxJson::reserve() + Tuple::fulfiller_pads);

  Continuation *fulfiller = scope->claim_fulfiller(runtime, output);
  WriteWakeboxJson *wbj = WriteWakeboxJson::claim(runtime.heap, spec, filepath, fulfiller, indent);
  wbj->execute(runtime);
}

void prim_register_wakebox(PrimMap &pmap) {
  prim_register(pmap, "write_wakebox_spec", prim_write_wakebox_spec, type_write_wakebox_spec,
                PRIM_IMPURE);
}
