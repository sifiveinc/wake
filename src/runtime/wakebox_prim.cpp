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
#include <string>

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
// check_spec_ready: walk every Promise in the spec tree, returning the first
// unfulfilled one. No data is extracted; this drives the retry loop in
// WriteWakeboxJson::execute so it suspends on exactly the right Promise.
// ---------------------------------------------------------------------------
static Promise *check_spec_ready(Record *spec) {
  Promise *p;

  // Field 0: Label (String)
  p = spec->at(0);
  if (!*p) return p;

  // Fields 1-2: Command, Environment (List String)
  for (int f = 1; f <= 2; ++f) {
    p = spec->at(f);
    if (!*p) return p;
    Record *list = p->coerce<Record>();
    while (list->cons == &List->members[1]) {
      Promise *head = list->at(0);
      if (!*head) return head;
      Promise *tail = list->at(1);
      if (!*tail) return tail;
      list = tail->coerce<Record>();
    }
  }

  // Field 3: Visible (List Path) — Path has 5 fields
  p = spec->at(3);
  if (!*p) return p;
  {
    Record *list = p->coerce<Record>();
    while (list->cons == &List->members[1]) {
      Promise *head = list->at(0);
      if (!*head) return head;
      Record *path = head->coerce<Record>();
      for (int i = 0; i < 5; ++i) {
        Promise *f = path->at(i);
        if (!*f) return f;
      }
      Promise *tail = list->at(1);
      if (!*tail) return tail;
      list = tail->coerce<Record>();
    }
  }

  // Fields 4-8: Directory, Stdin, CasDir, IsolateNetwork, IsolatePids
  for (int f = 4; f <= 8; ++f) {
    p = spec->at(f);
    if (!*p) return p;
  }

  // Field 9: MountOps (List WakeboxMountOp)
  // WakeboxBindOp=3 fields, WakeboxSquashfsOp=2, all others=1
  p = spec->at(9);
  if (!*p) return p;
  {
    Record *list = p->coerce<Record>();
    while (list->cons == &List->members[1]) {
      Promise *head = list->at(0);
      if (!*head) return head;
      Record *entry = head->coerce<Record>();
      int nf = (entry->cons->index == 0) ? 3 : (entry->cons->index == 1) ? 2 : 1;
      for (int i = 0; i < nf; ++i) {
        Promise *f = entry->at(i);
        if (!*f) return f;
      }
      Promise *tail = list->at(1);
      if (!*tail) return tail;
      list = tail->coerce<Record>();
    }
  }

  // Fields 10-11: Hostname, Domainname (String)
  for (int f = 10; f <= 11; ++f) {
    p = spec->at(f);
    if (!*p) return p;
  }

  // Fields 12-14: Option Integer (UserId, GroupId, CommandTimeout)
  for (int f = 12; f <= 14; ++f) {
    p = spec->at(f);
    if (!*p) return p;
    Record *opt = p->coerce<Record>();
    if (opt->cons->index == 0) {  // Some — check the inner Integer
      Promise *inner = opt->at(0);
      if (!*inner) return inner;
    }
  }

  // Field 15: Runner (String)
  p = spec->at(15);
  if (!*p) return p;

  return nullptr;
}

// ---------------------------------------------------------------------------
// JStream: minimal helper for streaming JSON to an ostream.
// All Promises are fully fulfilled before this is called.
// ---------------------------------------------------------------------------
struct JStream {
  std::ostream &os;
  const int indent;
  int depth = 0;

  void nl() {
    if (indent <= 0) return;
    os << '\n';
    for (int i = 0; i < depth * indent; ++i) os << ' ';
  }

  void key(const char *k) {
    os << '"' << k << (indent > 0 ? "\": " : "\":");
  }

  void str(const std::string &s) { os << '"' << json_escape(s) << '"'; }
};

static void write_str_array(JStream &js, Record *list) {
  js.os << '[';
  ++js.depth;
  bool first = true;
  while (list->cons == &List->members[1]) {
    if (!first) js.os << ',';
    first = false;
    js.nl();
    js.str(list->at(0)->coerce<String>()->as_str());
    list = list->at(1)->coerce<Record>();
  }
  --js.depth;
  if (!first) js.nl();
  js.os << ']';
}

// PathType constructor indices (io.wake declaration order):
//   0=RegularFile, 1=Directory, 2=CharDevice, 3=BlockDevice,
//   4=Fifo, 5=Symlink, 6=Socket
static const char *k_path_type_strings[] = {"file",  "directory", nullptr, nullptr,
                                             nullptr, "symlink",   nullptr};

// MountOp type strings (runner.wake declaration order):
//   0=bind, 1=squashfs, 2=tmpfs, 3=create-dir, 4=create-file, 5=workspace
static const char *k_mount_type_strings[] = {"bind",       "squashfs",    "tmpfs",
                                              "create-dir", "create-file", "workspace"};

// Stream spec JSON directly to filepath without an intermediate SpecData or
// JAST tree. All Promises must be fulfilled (call check_spec_ready first).
// Returns an empty string on success, or an error message on failure.
static std::string stream_spec_json(Record *spec, const char *filepath, int indent) {
  (void)unlink(filepath);
  std::ofstream out(filepath, std::ios_base::trunc);
  if (out.fail()) return std::string("write_wakebox_spec: failed to open ") + filepath;

  JStream js{out, indent};
  bool need_sep = false;
  auto sep = [&]() {
    if (need_sep) out << ',';
    need_sep = true;
    js.nl();
  };

  out << '{';
  ++js.depth;

  // label
  sep();
  js.key("label");
  js.str(spec->at(0)->coerce<String>()->as_str());

  // command, environment
  sep();
  js.key("command");
  write_str_array(js, spec->at(1)->coerce<Record>());
  sep();
  js.key("environment");
  write_str_array(js, spec->at(2)->coerce<Record>());

  // visible
  sep();
  js.key("visible");
  out << '[';
  {
    ++js.depth;
    bool first = true;
    Record *list = spec->at(3)->coerce<Record>();
    while (list->cons == &List->members[1]) {
      if (!first) out << ',';
      first = false;
      js.nl();

      Record *path = list->at(0)->coerce<Record>();
      Record *pt = path->at(1)->coerce<Record>();
      int pt_idx = pt->cons->index;
      if (pt_idx < 0 || pt_idx >= 7 || k_path_type_strings[pt_idx] == nullptr) {
        out.close();
        (void)unlink(filepath);
        return std::string("write_wakebox_spec: unsupported path type '") +
               pt->cons->ast.name + "' for wakebox (only file, directory, symlink are valid)";
      }

      mpz_t mv = {path->at(3)->coerce<Integer>()->wrap()};
      mpz_t tv = {path->at(4)->coerce<Integer>()->wrap()};

      out << '{';
      ++js.depth;
      bool pf = false;
      auto pk = [&]() {
        if (pf) out << ',';
        pf = true;
        js.nl();
      };
      pk(); js.key("path"); js.str(path->at(0)->coerce<String>()->as_str());
      pk(); js.key("type"); out << '"' << k_path_type_strings[pt_idx] << '"';
      pk(); js.key("hash"); js.str(path->at(2)->coerce<String>()->as_str());
      pk(); js.key("mode"); out << mpz_get_si(mv);
      pk(); js.key("mtime"); out << mpz_get_si(tv);
      --js.depth;
      js.nl();
      out << '}';

      list = list->at(1)->coerce<Record>();
    }
    --js.depth;
    if (!first) js.nl();
    out << ']';
  }

  // directory, stdin, cas-dir
  sep(); js.key("directory"); js.str(spec->at(4)->coerce<String>()->as_str());
  sep(); js.key("stdin");     js.str(spec->at(5)->coerce<String>()->as_str());
  sep(); js.key("cas-dir");   js.str(spec->at(6)->coerce<String>()->as_str());

  // isolate-network, isolate-pids
  bool iso_net = (spec->at(7)->coerce<Record>()->cons == &Boolean->members[0]);
  bool iso_pid = (spec->at(8)->coerce<Record>()->cons == &Boolean->members[0]);
  sep(); js.key("isolate-network"); out << (iso_net ? "true" : "false");
  sep(); js.key("isolate-pids");    out << (iso_pid ? "true" : "false");

  // mount-ops
  sep();
  js.key("mount-ops");
  out << '[';
  {
    ++js.depth;
    bool first = true;
    Record *list = spec->at(9)->coerce<Record>();
    while (list->cons == &List->members[1]) {
      if (!first) out << ',';
      first = false;
      js.nl();

      Record *entry = list->at(0)->coerce<Record>();
      int idx = entry->cons->index;

      out << '{';
      ++js.depth;
      bool mf = false;
      auto mk = [&]() {
        if (mf) out << ',';
        mf = true;
        js.nl();
      };
      mk(); js.key("type"); out << '"' << k_mount_type_strings[idx] << '"';
      if (idx == 0 || idx == 1) {  // bind or squashfs — have source + destination
        mk(); js.key("source");      js.str(entry->at(0)->coerce<String>()->as_str());
        mk(); js.key("destination"); js.str(entry->at(1)->coerce<String>()->as_str());
        if (idx == 0) {  // bind also has read_only
          bool ro = (entry->at(2)->coerce<Record>()->cons == &Boolean->members[0]);
          mk(); js.key("read_only"); out << (ro ? "true" : "false");
        }
      } else {  // tmpfs, create-dir, create-file, workspace — destination only
        mk(); js.key("destination"); js.str(entry->at(0)->coerce<String>()->as_str());
      }
      --js.depth;
      js.nl();
      out << '}';

      list = list->at(1)->coerce<Record>();
    }
    --js.depth;
    if (!first) js.nl();
    out << ']';
  }

  // Optional string fields
  auto opt_str = [&](const char *k, int f) {
    std::string s = spec->at(f)->coerce<String>()->as_str();
    if (!s.empty()) {
      sep();
      js.key(k);
      js.str(s);
    }
  };
  opt_str("hostname", 10);
  opt_str("domainname", 11);

  // Optional integer fields
  auto opt_int = [&](const char *k, int f) {
    Record *opt = spec->at(f)->coerce<Record>();
    if (opt->cons->index == 0) {  // Some
      mpz_t v = {opt->at(0)->coerce<Integer>()->wrap()};
      sep();
      js.key(k);
      out << mpz_get_si(v);
    }
  };
  opt_int("user-id", 12);
  opt_int("group-id", 13);
  opt_int("command-timeout", 14);

  opt_str("runner", 15);

  --js.depth;
  js.nl();
  out << '}';
  if (indent > 0) out << '\n';

  if (out.bad()) return std::string("write_wakebox_spec: failed to write ") + filepath;
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
  // Pass 1: check all Promises are fulfilled; suspend on the first that isn't.
  if (Promise *broken = check_spec_ready(spec.get())) {
    next = nullptr;
    broken->await(runtime, this);
    return;
  }

  // Pass 2: stream JSON directly from the Wake tuple — no intermediate copies.
  std::string err = stream_spec_json(spec.get(), filepath->c_str(), indent);

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

  int indent = (mpz_fits_sint_p(indent_mpz) && mpz_get_si(indent_mpz) > 0) ? mpz_get_si(indent_mpz) : 0;

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
