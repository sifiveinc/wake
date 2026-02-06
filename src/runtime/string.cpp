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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "blake3/blake3.h"
#include "gc.h"
#include "json/utf8.h"
#include "prim.h"
#include "status.h"
#include "types/data.h"
#include "types/internal.h"
#include "types/type.h"
#include "utf8proc/utf8proc.h"
#include "util/shell.h"
#include "util/term.h"
#include "util/unlink.h"
#include "value.h"
#include "wcl/tracing.h"
#include "wcl/xoshiro_256.h"

static PRIMFN(prim_vcat) {
  (void)data;

  size_t size = 0;
  for (size_t i = 0; i < nargs; ++i) {
    STRING(s, i);
    size += s->size();
  }

  String *out = String::alloc(runtime.heap, size);
  out->c_str()[size] = 0;

  size = 0;
  for (size_t i = 0; i < nargs; ++i) {
    String *s = static_cast<String *>(args[i]);
    memcpy(out->c_str() + size, s->c_str(), s->size());
    size += s->size();
  }

  RETURN(out);
}

static PRIMTYPE(type_strlen) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeInteger);
}

static PRIMFN(prim_strlen) {
  EXPECT(1);
  STRING(arg, 0);
  MPZ out(arg->size());
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMTYPE(type_lcat) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(list) && out->unify(Data::typeString);
}

struct CCat final : public GCObject<CCat, Continuation> {
  HeapPointer<Record> list;
  HeapPointer<Record> progress;
  HeapPointer<Scope> scope;
  size_t output;

  CCat(Record *list_, Scope *scope_, size_t output_)
      : list(list_), progress(list_), scope(scope_), output(output_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (list.*memberfn)(arg);
    arg = (progress.*memberfn)(arg);
    arg = (scope.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

void CCat::execute(Runtime &runtime) {
  while (progress->size() == 2 && *progress->at(0) && *progress->at(1))
    progress = progress->at(1)->coerce<Record>();

  if (progress->size() == 2) {
    next = nullptr;  // reschedule
    if (*progress->at(0)) {
      progress->at(1)->await(runtime, this);
    } else {
      progress->at(0)->await(runtime, this);
    }
  } else {
    size_t size = 0;
    for (Record *scan = list.get(); scan->size() == 2; scan = scan->at(1)->coerce<Record>())
      size += scan->at(0)->coerce<String>()->size();

    String *out = String::alloc(runtime.heap, size);
    out->c_str()[size] = 0;

    size = 0;
    for (Record *scan = list.get(); scan->size() == 2; scan = scan->at(1)->coerce<Record>()) {
      String *s = scan->at(0)->coerce<String>();
      memcpy(out->c_str() + size, s->c_str(), s->size());
      size += s->size();
    }

    scope->at(output)->fulfill(runtime, out);
  }
}

static PRIMFN(prim_lcat) {
  EXPECT(1);
  RECORD(list, 0);
  runtime.schedule(CCat::alloc(runtime.heap, list, scope, output));
}

static PRIMTYPE(type_explode) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(list);
}

static PRIMFN(prim_explode) {
  EXPECT(1);
  STRING(arg0, 0);

  // worst-case estimate
  size_t need = reserve_list(arg0->size()) + arg0->size() * String::reserve(4);
  runtime.heap.reserve(need);

  std::vector<Value *> vals;
  uint32_t rune;

  int got;
  for (const char *ptr = arg0->c_str(); *ptr; ptr += got) {
    got = pop_utf8(&rune, ptr);
    if (got < 1) got = 1;
    vals.push_back(String::claim(runtime.heap, ptr, got));
  }

  RETURN(claim_list(runtime.heap, vals.size(), vals.data()));
}

static PRIMTYPE(type_read) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(result);
}

static PRIMTYPE(type_breadcrumb) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_breadcrumb) {
  EXPECT(1);
  STRING(request_str, 0);

  wcl::log::info(request_str->c_str())({{"source", "wake"}});

  runtime.heap.reserve(reserve_unit());
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_stat) {
  TypeVar result;
  TypeVar p1;
  TypeVar p2;
  Data::typePair.clone(p1);
  Data::typePair.clone(p2);
  Data::typeResult.clone(result);
  p2[0].unify(Data::typeInteger);
  p2[1].unify(Data::typeInteger);
  p1[0].unify(Data::typeInteger);
  p1[1].unify(p2);
  result[0].unify(p1);
  result[1].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_stat) {
  EXPECT(1);
  STRING(path, 0);

  size_t max_error = path->size() + 100;

  struct stat buf;
  if (lstat(path->c_str(), &buf) < 0) {
    std::stringstream str;
    str << "stat " << path->c_str() << ": " << strerror(errno);
    std::string s = str.str();

    size_t len = std::min(s.size(), max_error);
    String *out = String::claim(runtime.heap, s.c_str(), len);
    RETURN(claim_result(runtime.heap, false, out));
  }

  int mode = 0;
  mode |= !!(buf.st_mode & S_IXOTH) << 0;
  mode |= !!(buf.st_mode & S_IWOTH) << 1;
  mode |= !!(buf.st_mode & S_IROTH) << 2;
  mode |= !!(buf.st_mode & S_IXGRP) << 3;
  mode |= !!(buf.st_mode & S_IWGRP) << 4;
  mode |= !!(buf.st_mode & S_IRGRP) << 5;
  mode |= !!(buf.st_mode & S_IXUSR) << 6;
  mode |= !!(buf.st_mode & S_IWUSR) << 7;
  mode |= !!(buf.st_mode & S_IRUSR) << 8;

  int type = -1;
  switch (buf.st_mode & S_IFMT) {
    case S_IFREG:
      type = 0;
      break;
    case S_IFDIR:
      type = 1;
      break;
    case S_IFCHR:
      type = 2;
      break;
    case S_IFBLK:
      type = 3;
      break;
    case S_IFIFO:
      type = 4;
      break;
    case S_IFLNK:
      type = 5;
      break;
    case S_IFSOCK:
      type = 6;
      break;
  }

  runtime.heap.reserve(reserve_result() + Integer::reserve(mode) + Integer::reserve(type) +
                       Integer::reserve(buf.st_size) + 2 * reserve_tuple2());
  auto mode_out = Integer::claim(runtime.heap, mode);
  auto type_out = Integer::claim(runtime.heap, type);
  auto size_out = Integer::claim(runtime.heap, buf.st_size);
  auto p1 = claim_tuple2(runtime.heap, type_out, mode_out);
  auto p2 = claim_tuple2(runtime.heap, size_out, p1);

  RETURN(claim_result(runtime.heap, true, p2));
}

static PRIMFN(prim_read) {
  EXPECT(1);
  STRING(path, 0);

  size_t max_error = path->size() + 100;
  size_t need_fail = reserve_result() + String::reserve(max_error);
  runtime.heap.reserve(need_fail);

  std::ifstream t(path->c_str(), std::ios::in | std::ios::binary);
  if (t) {
    t.seekg(0, t.end);
    auto size = t.tellg();

    if (size != -1) {
      size_t need_pass = need_fail + String::reserve(size);
      runtime.heap.reserve(need_pass);

      String *out = String::claim(runtime.heap, size);
      out->c_str()[size] = 0;

      t.seekg(0, t.beg);
      t.read(out->c_str(), out->size());
      if (t) RETURN(claim_result(runtime.heap, true, out));
    }
  }

  std::stringstream str;
  str << "read " << path->c_str() << ": " << strerror(errno);
  std::string s = str.str();

  size_t len = std::min(s.size(), max_error);
  String *out = String::claim(runtime.heap, s.c_str(), len);
  RETURN(claim_result(runtime.heap, false, out));
}

static PRIMTYPE(type_write) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.size() == 3 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeString) && args[2]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_write) {
  EXPECT(3);
  INTEGER_MPZ(mode, 0);
  STRING(path, 1);
  STRING(body, 2);

  // Reservation must happen first so we don't have re-entrant side-effects
  size_t max_error = path->size() + 100;
  size_t need = reserve_result() + String::reserve(max_error);
  runtime.heap.reserve(need);

  REQUIRE(mpz_cmp_si(mode, 0) >= 0);
  REQUIRE(mpz_cmp_si(mode, 0x1ff) <= 0);
  long mask = mpz_get_si(mode);

  // We want `write` to overwrite existing files so that
  // each time the build runs it isn't blocked by previous files.
  // However we want it to fail if it attempts to delete a directory.
  // We need to give the user a good error message in that case.
  if (unlink(path->c_str()) < 0 && errno != ENOENT) {
    if (errno == EISDIR) {
      std::string error = path->c_str();
      error +=
          " is a directory and cannot be overwritten. If this is intentional please manually "
          "delete this directory";
      size_t len = std::min(error.size(), max_error);
      String *out = String::claim(runtime.heap, error.c_str(), len);
      RETURN(claim_result(runtime.heap, false, out));
    }
    std::string error = strerror(errno);
    size_t len = std::min(error.size(), max_error);
    String *out = String::claim(runtime.heap, error.c_str(), len);
    RETURN(claim_result(runtime.heap, false, out));
  }

  std::ofstream t(path->c_str(), std::ios_base::trunc);
  if (!t.fail()) {
    t.write(body->c_str(), body->size());
    if (!t.bad()) {
      chmod(path->c_str(), mask);
      auto out = claim_result(runtime.heap, true, args[1]);
      RETURN(out);
    }
  }

  std::stringstream str;
  str << "write " << path->c_str() << ": " << strerror(errno);
  std::string s = str.str();

  size_t len = std::min(s.size(), max_error);
  String *out = String::claim(runtime.heap, s.c_str(), len);
  RETURN(claim_result(runtime.heap, false, out));
}

static PRIMTYPE(type_unlink) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_unlink) {
  EXPECT(1);
  STRING(path, 0);

  // Reservation must happen first so we don't have re-entrant side-effects
  runtime.heap.reserve(reserve_unit());

  // don't care if this succeeds
  (void)unlink(path->c_str());

  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_getenv) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(list);
}

static PRIMFN(prim_getenv) {
  EXPECT(1);
  STRING(arg0, 0);
  const char *env = getenv(arg0->c_str());
  if (env) {
    size_t len = strlen(env);
    size_t need = reserve_list(1) + String::reserve(len);
    runtime.heap.reserve(need);
    Value *out = String::claim(runtime.heap, env, len);
    RETURN(claim_list(runtime.heap, 1, &out));
  } else {
    RETURN(alloc_nil(runtime.heap));
  }
}

static PRIMTYPE(type_mkdir) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.size() == 2 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_mkdir) {
  EXPECT(2);
  INTEGER_MPZ(mode, 0);
  STRING(path, 1);

  // Reservation must happen first so we don't have re-entrant side-effects
  size_t max_error = path->size() + 100;
  size_t need = reserve_result() + String::reserve(max_error);
  runtime.heap.reserve(need);

  REQUIRE(mpz_cmp_si(mode, 0) >= 0);
  REQUIRE(mpz_cmp_si(mode, 0x1ff) <= 0);
  long mask = mpz_get_si(mode);

  // Remove any file or link that might be in the way
  // If this fails, it's ok. It will lead to mkdir() below failing with
  // an appropriate and hopefully more helpful error message.
  (void)unlink(path->c_str());

  if (mkdir(path->c_str(), mask) == 0) {
    RETURN(claim_result(runtime.heap, true, args[1]));
  } else if (errno == EEXIST || errno == EISDIR) {
    // Even if we can't chmod an existing absolute path, we have to proceed.
    // Otherwise, you would not be able to mkdir() into the rootfs at all,
    // because root owns all the root directories.
    if (chmod(path->c_str(), mask) == 0 || path->c_str()[0] == '/') {
      RETURN(claim_result(runtime.heap, true, args[1]));
    } else {
      std::stringstream str;
      str << "mkdir(chmod) " << path->c_str() << ": " << strerror(errno);
      std::string s = str.str();

      size_t len = std::min(s.size(), max_error);
      String *out = String::claim(runtime.heap, s.c_str(), len);
      RETURN(claim_result(runtime.heap, false, out));
    }
  } else {
    std::stringstream str;
    str << "mkdir " << path->c_str() << ": " << strerror(errno);
    std::string s = str.str();

    size_t len = std::min(s.size(), max_error);
    String *out = String::claim(runtime.heap, s.c_str(), len);
    RETURN(claim_result(runtime.heap, false, out));
  }
}

static PRIMTYPE(type_format) {
  return args.size() == 1 &&
         // don't unify args[0] => allow any
         out->unify(Data::typeString);
}

struct CFormat final : public GCObject<CFormat, Continuation> {
  HeapPointer<HeapObject> obj;
  HeapPointer<Continuation> cont;

  CFormat(HeapObject *obj_, Continuation *cont_) : obj(obj_), cont(cont_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (obj.*memberfn)(arg);
    arg = (cont.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

void CFormat::execute(Runtime &runtime) {
  std::stringstream buffer;
  buffer << obj.get();
  cont->resume(runtime, String::alloc(runtime.heap, buffer.str()));
}

static PRIMFN(prim_format) {
  EXPECT(1);
  runtime.heap.reserve(Tuple::fulfiller_pads + reserve_hash() + CFormat::reserve());
  runtime.schedule(
      claim_hash(runtime.heap, args[0],
                 CFormat::claim(runtime.heap, args[0], scope->claim_fulfiller(runtime, output))));
}

static PRIMTYPE(type_print) {
  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeUnit);
}

static PRIMFN(prim_print) {
  EXPECT(2);
  STRING(stream, 0);
  STRING(message, 1);
  runtime.heap.reserve(reserve_unit());
  // prim "print" is now assured to receive a newline
  status_get_generic_stream(stream->c_str()) << message->as_str();
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_version) { return args.size() == 0 && out->unify(Data::typeString); }

static PRIMFN(prim_version) {
  EXPECT(0);
  StringInfo *info = static_cast<StringInfo *>(data);
  RETURN(String::alloc(runtime.heap, info->version));
}

static PRIMTYPE(type_level) { return args.size() == 0 && out->unify(Data::typeInteger); }

static PRIMFN(prim_level) {
  EXPECT(0);
  StringInfo *info = static_cast<StringInfo *>(data);

  int x;
  if (info->quiet) {
    x = 0;
  } else if (info->verbose) {
    if (info->debug) {
      x = 3;
    } else {
      x = 2;
    }
  } else {
    x = 1;
  }

  MPZ out(x);
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMFN(prim_scmp) {
  EXPECT(2);
  STRING(arg0, 0);
  STRING(arg1, 1);
  RETURN(alloc_order(runtime.heap, arg0->compare(*arg1)));
}

static PRIMTYPE(type_normalize) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

struct UTF8Out {
  String *in;
  utf8proc_uint8_t *dst;
  ssize_t len;

  ~UTF8Out() {
    if (len >= 0) free(dst);
  }
  UTF8Out(String *in_, unsigned long opt) : in(in_) {
    len = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(in->c_str()), in->size(), &dst,
                       static_cast<utf8proc_option_t>(opt));
  }

  String *copy(Heap &heap) const {
    if (len < 0) return in;
    return String::alloc(heap, reinterpret_cast<const char *>(dst), static_cast<size_t>(len));
  }
};

static PRIMFN(prim_sNFC) {
  EXPECT(1);
  STRING(arg0, 0);

  UTF8Out out(arg0, UTF8PROC_COMPOSE | UTF8PROC_REJECTNA);

  RETURN(out.copy(runtime.heap));
}

static PRIMFN(prim_sNFKC) {
  EXPECT(1);
  STRING(arg0, 0);

  UTF8Out out(arg0, UTF8PROC_COMPOSE | UTF8PROC_COMPAT | UTF8PROC_IGNORE | UTF8PROC_LUMP |
                        UTF8PROC_REJECTNA);

  RETURN(out.copy(runtime.heap));
}

static PRIMTYPE(type_code2str) {
  return args.size() == 1 && args[0]->unify(Data::typeInteger) && out->unify(Data::typeString);
}

static PRIMFN(prim_code2str) {
  EXPECT(1);
  INTEGER_MPZ(arg0, 0);
  std::string str;
  bool ok = mpz_fits_slong_p(arg0);
  long x = ok ? mpz_get_si(arg0) : 0;
  ok = ok && x >= 0;
  ok = ok && push_utf8(str, x);
  RETURN(String::alloc(runtime.heap, ok ? str : ""));
}

static PRIMFN(prim_bin2str) {
  EXPECT(1);
  INTEGER_MPZ(arg0, 0);
  bool ok = mpz_fits_slong_p(arg0);
  long x = ok ? mpz_get_si(arg0) : 0;
  ok = ok && x >= 0 && x < 256;
  char c[2] = {static_cast<char>(x), 0};
  if (ok) {
    RETURN(String::alloc(runtime.heap, c, 1));
  } else {
    RETURN(String::alloc(runtime.heap, ""));
  }
}

static PRIMTYPE(type_str2code) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeInteger);
}

static PRIMFN(prim_str2code) {
  EXPECT(1);
  STRING(arg0, 0);
  uint32_t rune;
  int x = pop_utf8(&rune, arg0->c_str());
  MPZ out(x >= 1 ? rune : arg0->c_str()[0]);
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMFN(prim_str2bin) {
  EXPECT(1);
  STRING(arg0, 0);
  MPZ out(static_cast<unsigned char>(arg0->c_str()[0]));
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMTYPE(type_str2bytes) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeInteger);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(list);
}

static PRIMFN(prim_str2bytes) {
  EXPECT(1);
  STRING(arg0, 0);

  // Reserve memory for a list of size() elements + size() number of integers up to the value 255
  size_t need = reserve_list(arg0->size()) + arg0->size() * Integer::reserve(255);
  runtime.heap.reserve(need);

  std::vector<Value *> vals;
  for (size_t idx = 0; idx < arg0->size(); idx++) {
    MPZ out(static_cast<unsigned char>(arg0->c_str()[idx]));
    vals.push_back(Integer::claim(runtime.heap, out));
  }

  RETURN(claim_list(runtime.heap, vals.size(), vals.data()));
}

static PRIMTYPE(type_cwd) { return args.size() == 0 && out->unify(Data::typeString); }

static PRIMFN(prim_cwd) {
  EXPECT(0);
  StringInfo *info = static_cast<StringInfo *>(data);
  RETURN(String::alloc(runtime.heap, info->wake_cwd));
}

static PRIMTYPE(type_cmdline) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeString);
  return args.size() == 0 && out->unify(list);
}

static PRIMFN(prim_cmdline) {
  EXPECT(0);
  StringInfo *info = static_cast<StringInfo *>(data);

  size_t need = 0, len = 0;
  for (char **arg = info->cmdline; *arg; ++arg) {
    need += String::reserve(strlen(*arg));
    ++len;
  }
  need += reserve_list(len);
  runtime.heap.reserve(need);

  std::vector<Value *> vals;
  for (char **arg = info->cmdline; *arg; ++arg) vals.push_back(String::claim(runtime.heap, *arg));
  RETURN(claim_list(runtime.heap, vals.size(), vals.data()));
}

static PRIMTYPE(type_uname) {
  TypeVar pair;
  Data::typePair.clone(pair);
  pair[0].unify(Data::typeString);
  pair[1].unify(Data::typeString);
  return args.size() == 0 && out->unify(pair);
}

static PRIMFN(prim_uname) {
  EXPECT(0);
  struct utsname uts;
  int ret = uname(&uts);
  REQUIRE(ret == 0);

  size_t slen = strlen(uts.sysname);
  size_t mlen = strlen(uts.machine);
  size_t need = reserve_tuple2() + String::reserve(slen) + String::reserve(mlen);
  runtime.heap.reserve(need);

  auto out = claim_tuple2(runtime.heap, String::claim(runtime.heap, uts.sysname, slen),
                          String::claim(runtime.heap, uts.machine, mlen));
  RETURN(out);
}

static PRIMTYPE(type_shell_str) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static PRIMFN(prim_shell_str) {
  EXPECT(1);
  STRING(str, 0);
  RETURN(String::alloc(runtime.heap, shell_escape(str->c_str())));
}

static PRIMTYPE(type_filter_term_codes) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static PRIMFN(prim_filter_term_codes) {
  EXPECT(1);
  STRING(str, 0);

  std::stringstream ss;
  TermInfoBuf tinfo(ss.rdbuf(), /*dumb*/ true);
  std::ostream out(&tinfo);
  out << str->as_str();

  RETURN(String::alloc(runtime.heap, ss.str()));
}

static PRIMTYPE(type_hash_str) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static void blake3(const std::string &str, uint64_t (*out)[4]) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, reinterpret_cast<const uint8_t *>(str.data()), str.size());
  blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t *>(*out), sizeof(*out));
}

static PRIMFN(prim_hash_str) {
  EXPECT(1);
  STRING(str, 0);

  uint64_t hash[4];
  blake3(str->as_str(), &hash);
  auto hex = wcl::to_hex(&hash);

  RETURN(String::alloc(runtime.heap, std::move(hex)));
}

static PRIMTYPE(type_to_upper) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static PRIMFN(prim_to_upper) {
  EXPECT(1);
  STRING(str, 0);

  const char *in = str->c_str();
  ssize_t in_size = str->length;
  std::string out;
  while (in_size > 0) {
    int32_t codepoint = 0;
    ssize_t size = utf8proc_iterate(reinterpret_cast<const uint8_t *>(in), in_size, &codepoint);
    in += size;
    in_size -= size;
    int32_t upper = utf8proc_toupper(codepoint);
    char buf[4] = {0};
    ssize_t bytes = utf8proc_encode_char(upper, reinterpret_cast<uint8_t *>(buf));
    out.insert(out.end(), buf, buf + bytes);
  }

  RETURN(String::alloc(runtime.heap, std::move(out)));
}

static PRIMTYPE(type_to_lower) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static PRIMFN(prim_to_lower) {
  EXPECT(1);
  STRING(str, 0);

  const char *in = str->c_str();
  ssize_t in_size = str->length;
  std::string out;
  while (in_size > 0) {
    int32_t codepoint = 0;
    ssize_t size = utf8proc_iterate(reinterpret_cast<const uint8_t *>(in), in_size, &codepoint);
    in += size;
    in_size -= size;
    int32_t upper = utf8proc_tolower(codepoint);
    char buf[4] = {0};
    ssize_t bytes = utf8proc_encode_char(upper, reinterpret_cast<uint8_t *>(buf));
    out.insert(out.end(), buf, buf + bytes);
  }

  RETURN(String::alloc(runtime.heap, std::move(out)));
}

void prim_register_string(PrimMap &pmap, StringInfo *info) {
  prim_register(pmap, "strlen", prim_strlen, type_strlen, PRIM_PURE);
  prim_register(pmap, "vcat", prim_vcat, type_vcat, PRIM_PURE);
  prim_register(pmap, "lcat", prim_lcat, type_lcat, PRIM_PURE);
  prim_register(pmap, "explode", prim_explode, type_explode, PRIM_PURE);
  prim_register(pmap, "getenv", prim_getenv, type_getenv, PRIM_PURE);
  prim_register(pmap, "format", prim_format, type_format, PRIM_PURE);
  prim_register(pmap, "version", prim_version, type_version, PRIM_PURE, (void *)info);
  prim_register(pmap, "level", prim_level, type_level, PRIM_PURE, (void *)info);
  prim_register(pmap, "cwd", prim_cwd, type_cwd, PRIM_PURE, (void *)info);
  prim_register(pmap, "cmdline", prim_cmdline, type_cmdline, PRIM_PURE, (void *)info);
  prim_register(pmap, "scmp", prim_scmp, type_scmp, PRIM_PURE);
  prim_register(pmap, "sNFC", prim_sNFC, type_normalize, PRIM_PURE);
  prim_register(pmap, "sNFKC", prim_sNFKC, type_normalize, PRIM_PURE);
  prim_register(pmap, "code2str", prim_code2str, type_code2str, PRIM_PURE);
  prim_register(pmap, "bin2str", prim_bin2str, type_code2str, PRIM_PURE);
  prim_register(pmap, "str2code", prim_str2code, type_str2code, PRIM_PURE);
  prim_register(pmap, "str2bin", prim_str2bin, type_str2code, PRIM_PURE);
  prim_register(pmap, "uname", prim_uname, type_uname, PRIM_PURE);
  prim_register(pmap, "shell_str", prim_shell_str, type_shell_str, PRIM_PURE);
  prim_register(pmap, "filter_term_codes", prim_filter_term_codes, type_filter_term_codes,
                PRIM_PURE);
  prim_register(pmap, "hash_str", prim_hash_str, type_hash_str, PRIM_PURE);
  prim_register(pmap, "print", prim_print, type_print, PRIM_IMPURE);
  prim_register(pmap, "mkdir", prim_mkdir, type_mkdir, PRIM_IMPURE);
  prim_register(pmap, "unlink", prim_unlink, type_unlink, PRIM_IMPURE);
  prim_register(pmap, "write", prim_write, type_write, PRIM_IMPURE);
  prim_register(pmap, "read", prim_read, type_read, PRIM_ORDERED);
  prim_register(pmap, "breadcrumb", prim_breadcrumb, type_breadcrumb, PRIM_IMPURE);
  prim_register(pmap, "toupper", prim_to_upper, type_to_upper, PRIM_PURE);
  prim_register(pmap, "tolower", prim_to_lower, type_to_lower, PRIM_PURE);
  prim_register(pmap, "stat", prim_stat, type_stat, PRIM_ORDERED);
  prim_register(pmap, "str2bytes", prim_str2bytes, type_str2bytes, PRIM_PURE);
}
