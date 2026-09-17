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

#ifndef GC_H
#define GC_H

#include <stdint.h>

#include <iostream>
#include <memory>
#include <ostream>
#include <typeinfo>
#include <unordered_map>
#ifdef DEBUG_GC
#include <cassert>
#endif
#include "util/hash.h"

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8)
#define alignas(x)
#endif

struct Heap;
struct HeapObject;
struct HeapPointerBase;
struct DestroyableObject;
struct PadObject;
struct FormatState;
struct Promise;
template <typename T>
struct HeapPointer;
template <typename T>
struct RootPointer;

struct Placement {
  HeapObject *obj;
  PadObject *free;
  Placement(HeapObject *obj_, void *free_) : obj(obj_), free(static_cast<PadObject *>(free_)) {}
};

struct HeapStep {
  Promise *broken;  // non-zero if there is an unfulfilled Promise
  HeapObject **found;
};

struct HeapAgeTracker {
  static void initTracker(bool enable) { heapAgeTracker = enable; }

  static void setAge(const HeapObject *obj, uint32_t age) {
    if (!heapAgeTracker) return;
    age_map[obj] = age;
  }

  static uint32_t getAge(const HeapObject *obj) {
    if (!heapAgeTracker) return 0;
    auto it = age_map.find(obj);
    if (it == age_map.end()) {
      return 0;
    }
    return it->second;
  }

  static void removeAge(const HeapObject *obj) {
    if (!heapAgeTracker) return;
    age_map.erase(obj);
  }

 private:
  // Don't want any instances of this
  HeapAgeTracker() = delete;
  static bool heapAgeTracker;
  static std::unordered_map<const HeapObject *, uint32_t> age_map;
};

enum Category { VALUE, WORK };

struct HeapObject {
  virtual Placement moveto(PadObject *free) = 0;
  virtual Placement descend(PadObject *free) = 0;
  virtual HeapStep explore(HeapStep step) = 0;
  virtual const char *type() const = 0;
  virtual void format(std::ostream &os, FormatState &state) const = 0;
  virtual Category category() const = 0;
  virtual ~HeapObject();

  static void format(std::ostream &os, const HeapObject *value, bool detailed = false,
                     int indent = -1);
  std::string to_str() const;

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    return arg;
  }

  // this overload causes non-placement 'new' to become illegal (which we want)
  void *operator new(size_t size, void *free) { return free; }
};

inline std::ostream &operator<<(std::ostream &os, const HeapObject *value) {
  HeapObject::format(os, value);
  return os;
}

struct RootRing {
  HeapObject *root;
  RootRing *prev;
  RootRing *next;

  explicit RootRing(HeapObject *root_ = nullptr) : root(root_), prev(this), next(this) {}
  RootRing(const RootRing &ring)
      : root(ring.root), prev(ring.prev), next(const_cast<RootRing *>(&ring)) {
    prev->next = this;
    next->prev = this;
  }

  RootRing &operator=(const RootRing &) = delete;

  RootRing(RootRing &&x) {
    root = x.root;
    x.root = nullptr;
    if (x.next == &x) {
      prev = this;
      next = this;
    } else {
      prev = x.prev;
      next = x.next;
      prev->next = this;
      next->prev = this;
      x.prev = &x;
      x.next = &x;
    }
  }

  RootRing(RootRing &x, HeapObject *root_) : root(root_), prev(&x), next(x.next) {
    next->prev = this;
    x.next = this;
  }

  ~RootRing() {
    prev->next = next;
    next->prev = prev;
  }
};

template <typename T>
struct RootPointer {
  // construct using heap.root(ptr)
  RootPointer(RootRing &o, HeapObject *obj) : ring(o, obj) {}
  template <typename Y>
  RootPointer(RootPointer<Y> &&r) : ring(std::move(r.ring)) {}

  explicit operator bool() const { return ring.root; }
  void reset() { ring.root = nullptr; }

  T *get() const { return static_cast<T *>(ring.root); }
  T *operator->() const { return get(); }
  T &operator*() const { return *get(); }

  template <typename Y>
  RootPointer &operator=(HeapPointer<Y> x);
  template <typename Y>
  RootPointer &operator=(const RootPointer<Y> &x) {
    ring.root = static_cast<T *>(x.get());
    return *this;
  }
  RootPointer &operator=(T *x) {
    ring.root = x;
    return *this;
  }

 private:
  RootRing ring;
  template <typename Y>
  friend struct RootPointer;
};

struct HeapPointerBase {
  HeapPointerBase(HeapObject *obj_) : obj(obj_) {}
  PadObject *moveto(PadObject *free);
  HeapStep explore(HeapStep step);

 protected:
  HeapObject *obj;
};

inline PadObject *HeapPointerBase::moveto(PadObject *free) {
  if (!obj) return free;
  Placement out = obj->moveto(free);
  obj = out.obj;
  return out.free;
}

inline HeapStep HeapPointerBase::explore(HeapStep step) {
  if (obj) *step.found++ = obj;
  return step;
}

template <typename T>
struct HeapPointer : public HeapPointerBase {
  template <typename Y>
  HeapPointer(HeapPointer<Y> x) : HeapPointerBase(static_cast<T *>(x.get())) {}
  template <typename Y>
  HeapPointer(const RootPointer<Y> &x) : HeapPointerBase(static_cast<T *>(x.get())) {}
  HeapPointer(T *x = nullptr) : HeapPointerBase(x) {}

  explicit operator bool() const { return obj; }
  void reset() { obj = nullptr; }

  T *get() const { return static_cast<T *>(obj); }
  T *operator->() const { return get(); }
  T &operator*() const { return *get(); }

  template <typename Y>
  HeapPointer &operator=(HeapPointer<Y> x) {
    obj = static_cast<T *>(x.get());
    return *this;
  }
  template <typename Y>
  HeapPointer &operator=(const RootPointer<Y> &x) {
    obj = static_cast<T *>(x.get());
    return *this;
  }
  HeapPointer &operator=(T *x) {
    obj = x;
    return *this;
  }
};

template <typename T>
template <typename Y>
RootPointer<T> &RootPointer<T>::operator=(HeapPointer<Y> x) {
  ring.root = static_cast<T *>(x.get());
  return *this;
}

struct PadObject final : public HeapObject {
  Placement moveto(PadObject *free) override;
  Placement descend(PadObject *free) override;
  HeapStep explore(HeapStep step) override;
  const char *type() const override;
  void format(std::ostream &os, FormatState &state) const override;
  Category category() const override;
  static PadObject *place(PadObject *free) {
    new (free) PadObject();
    return free + 1;
  }
};

struct alignas(PadObject) MovedObject final : public HeapObject {
  MovedObject(HeapObject *to_) : to(to_) {}
  HeapObject *to;
  Placement moveto(PadObject *free) override;
  Placement descend(PadObject *free) override;
  HeapStep explore(HeapStep step) override;
  const char *type() const override;
  void format(std::ostream &os, FormatState &state) const override;
  Category category() const override;
};

struct GCNeededException {
  size_t needed;
  GCNeededException(size_t needed_) : needed(needed_) {}
};

struct Heap {
  Heap(int profile_heap_, double heap_factor_, double heap_pivot_mb_);
  ~Heap();

  // Call this from main loop (no pointers on stack) when GCNeededException
  void GC(size_t requested_pads);
  // Report max heap usage
  void report() const;

  // Reserve enough space for a sequence of allocations
  void reserve(size_t requested_pads) {
    if (static_cast<size_t>(end - free) < requested_pads) throw GCNeededException(requested_pads);
#ifdef DEBUG_GC
    limit = requested_pads;
#endif
  }

  // This invalidates all non-RootPointers. Beware!
  void guarantee(size_t requested_pads) {
    if (static_cast<size_t>(end - free) < requested_pads) GC(requested_pads);
#ifdef DEBUG_GC
    limit = requested_pads;
#endif
  }

  // Claim the space previously prepared by 'reserve'
  PadObject *claim(size_t requested_pads) {
    PadObject *out = free;
    free += requested_pads;
#ifdef DEBUG_GC
    assert(requested_pads <= limit);
    limit -= requested_pads;
#endif
    return out;
  }

  // Allocate memory for a single request
  PadObject *alloc(size_t requested_pads) {
    reserve(requested_pads);
    return claim(requested_pads);
  }

  size_t used() const;
  size_t alloc() const;
  size_t avail() const;

  // Grab a large temporary buffer from the GC's unused space
  void *scratch(size_t bytes);

  template <typename T>
  RootPointer<T> root(T *obj) {
    return RootPointer<T>(roots, obj);
  }
  template <typename T>
  RootPointer<T> root(HeapPointer<T> x) {
    return RootPointer<T>(roots, x.get());
  }

 private:
  struct Imp;
  std::unique_ptr<Imp> imp;
  RootRing roots;
  PadObject *free;
  PadObject *end;
#ifdef DEBUG_GC
  size_t limit;
#endif

  friend struct DestroyableObject;
};

template <typename T, typename B>
struct alignas(PadObject) GCObject : public B {
  template <typename... ARGS>
  GCObject(ARGS &&...args) : B(std::forward<ARGS>(args)...) {
    static_assert(sizeof(MovedObject) <= sizeof(T), "HeapObject is too small");
    static_assert(sizeof(PadObject) == alignof(T), "HeapObject alignment wrong");
  }

  T *self() { return static_cast<T *>(this); }
  const T *self() const { return static_cast<const T *>(this); }

  const void *data() const { return self() + 1; }
  void *data() { return self() + 1; }

  // Implement heap virtual methods using the type-specific 'recurse' method
  Placement moveto(PadObject *free) final override;
  Placement descend(PadObject *free) final override;
  HeapStep explore(HeapStep step) final override;
  // Can be further specialized
  const char *type() const override;

  // redefine these if 'data' extends past sizeof(T)
  PadObject *objend() { return static_cast<PadObject *>(static_cast<HeapObject *>(self() + 1)); }
  static size_t reserve();
  template <typename... ARGS>
  static T *claim(Heap &h, ARGS &&...args);  // require prior h.reserve
  template <typename... ARGS>
  static T *alloc(Heap &h, ARGS &&...args);
};

template <typename T, typename B>
size_t GCObject<T, B>::reserve() {
  return sizeof(T) / sizeof(PadObject);
}

template <typename T, typename B>
template <typename... ARGS>
T *GCObject<T, B>::claim(Heap &h, ARGS &&...args) {
  T *obj = new (h.claim(sizeof(T) / sizeof(PadObject))) T(std::forward<ARGS>(args)...);
  HeapAgeTracker::setAge(obj, 0);
  return obj;
}

template <typename T, typename B>
template <typename... ARGS>
T *GCObject<T, B>::alloc(Heap &h, ARGS &&...args) {
  T *obj = new (h.alloc(sizeof(T) / sizeof(PadObject))) T(std::forward<ARGS>(args)...);
  HeapAgeTracker::setAge(obj, 0);
  return obj;
}

template <typename T, typename B>
Placement GCObject<T, B>::moveto(PadObject *free) {
  if (alignof(T) > alignof(PadObject))
    while (((uintptr_t)free & (alignof(T) - 1))) free = PadObject::place(free);
  T *from = self();

  uint32_t old_age = HeapAgeTracker::getAge(from);
  T *to = new (free) T(std::move(*from));

  HeapAgeTracker::setAge(to, old_age + 1);
  HeapAgeTracker::removeAge(from);

  from->~T();
  new (from) MovedObject(to);
  return Placement(to, to->objend());
}

template <typename T, typename B>
Placement GCObject<T, B>::descend(PadObject *free) {
  return Placement(self()->objend(),
                   self()->template recurse<PadObject *, &HeapPointerBase::moveto>(free));
}

template <typename T, typename B>
HeapStep GCObject<T, B>::explore(HeapStep step) {
  return self()->template recurse<HeapStep, &HeapPointerBase::explore>(step);
}

template <typename T, typename B>
const char *GCObject<T, B>::type() const {
  const char *out;
  for (out = typeid(T).name(); *out >= '0' && *out <= '9'; ++out) {
  }
  return out;
}

struct Value : public HeapObject {
  Category category() const override;
  // Shallow inspection of this object (including type)
  virtual Hash shallow_hash() const = 0;
  // These will assert fail if the Values contain broken Promises
  bool deep_equal(const Value &x, Heap &heap);
  Hash deep_hash(Heap &heap);
};

struct DestroyableObject : public Value {
  DestroyableObject(Heap &h);
  DestroyableObject(DestroyableObject &&d) = default;
  HeapObject *next;
};

#endif
