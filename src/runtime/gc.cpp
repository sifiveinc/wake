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

#include "gc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

#include "status.h"

#define INITIAL_HEAP_SIZE 1024

bool HeapAgeTracker::heapAgeTracker = false;
std::unordered_map<const HeapObject *, uint32_t> HeapAgeTracker::age_map;
static std::ofstream g_csv;

HeapObject::~HeapObject() {}

Placement PadObject::moveto(PadObject *free) {
  assert(0 /* unreachable */);
  return Placement(0, 0);
}

Placement PadObject::descend(PadObject *free) { return Placement(this + 1, free); }

HeapStep PadObject::explore(HeapStep step) {
  assert(0 /* unreachable */);
  return step;
}

const char *PadObject::type() const { return "PadObject"; }

void PadObject::format(std::ostream &os, FormatState &state) const { os << "PadObject"; }

Category PadObject::category() const {
  assert(0 /* unreachable */);
  return VALUE;
}

Placement MovedObject::moveto(PadObject *free) { return Placement(to, free); }

Placement MovedObject::descend(PadObject *free) {
  assert(0 /* unreachable */);
  return Placement(0, 0);
}

const char *MovedObject::type() const { return "MovedObject"; }

HeapStep MovedObject::explore(HeapStep step) { return to->explore(step); }

void MovedObject::format(std::ostream &os, FormatState &state) const { to->format(os, state); }

Category MovedObject::category() const {
  // invoked by ~Target
  return to->category();
}

struct HeapStats {
  const char *type;
  size_t objects, pads;
  HeapStats() : type(nullptr), objects(0), pads(0) {}
};

struct Space {
  size_t size;
  size_t alloc;
  PadObject *array;

  Space(size_t size_ = INITIAL_HEAP_SIZE);
  ~Space();

  void resize(size_t size_);
};

Space::Space(size_t size_)
    : size(size_),
      alloc(size_),
      array(static_cast<PadObject *>(::malloc(sizeof(PadObject) * size))) {
  assert(array);
}

Space::~Space() { ::free(array); }

void Space::resize(size_t size_) {
  if (alloc < size_ || 3 * size_ < alloc) {
    alloc = size_ + (size_ >> 1);
    void *tmp = ::realloc(static_cast<void *>(array), sizeof(PadObject) * alloc);
    assert(tmp);
    array = static_cast<PadObject *>(tmp);
  }
  size = size_;
}

struct Heap::Imp {
  int profile_heap;
  double heap_factor;
  size_t heap_pivot_pads;
  Space spaces[2];
  int space;
  size_t last_pads;
  size_t most_pads;
  HeapStats peak[10];
  size_t peak_alloc;
  size_t previous_alloc;
  HeapObject *finalize;

  size_t gc_count;
  size_t total_gc_time;

  Imp(int profile_heap_, double heap_factor_, size_t heap_pivot_pads_)
      : profile_heap(profile_heap_),
        heap_factor(heap_factor_),
        heap_pivot_pads(heap_pivot_pads_),
        spaces(),
        space(0),
        last_pads(0),
        most_pads(0),
        peak(),
        peak_alloc(0),
        previous_alloc(1),
        finalize(nullptr),
        gc_count(0),
        total_gc_time(0) {}

  size_t desired_size(size_t live_pads, size_t requested_pads) const {
    size_t linear = static_cast<size_t>(heap_factor * live_pads);
    size_t sqrt_desired =
        live_pads +
        static_cast<size_t>(heap_factor * std::sqrt(static_cast<double>(live_pads) *
                                                    static_cast<double>(heap_pivot_pads)));
    return std::min(linear, sqrt_desired) + requested_pads;
  }
};

static size_t mb_to_pads(double mb) {
  return static_cast<size_t>(mb * 1024.0 * 1024.0 / sizeof(PadObject));
}

Heap::Heap(int profile_heap_, double heap_factor_, double heap_pivot_mb_)
    : imp(new Imp(profile_heap_, heap_factor_, mb_to_pads(heap_pivot_mb_))),
      roots(),
      free(imp->spaces[imp->space].array),
      end(free + imp->spaces[imp->space].size) {}

Heap::~Heap() {
  GC(0);
  assert(free == imp->spaces[imp->space].array);
}

size_t Heap::used() const { return (free - imp->spaces[imp->space].array) * sizeof(PadObject); }

size_t Heap::alloc() const {
  return (imp->spaces[0].alloc + imp->spaces[1].alloc) * sizeof(PadObject);
}

size_t Heap::avail() const { return (end - free) * sizeof(PadObject); }

void *Heap::scratch(size_t bytes) {
  size_t size = (bytes + sizeof(PadObject) - 1) / sizeof(PadObject);
  Space &idle = imp->spaces[imp->space ^ 1];
  if (idle.alloc < size) idle.resize(size);
  if (imp->peak_alloc < alloc()) imp->peak_alloc = alloc();
  return idle.array;
}

void Heap::report() const {
  if (imp->profile_heap) {
    std::stringstream s;
    s << "------------------------------------------" << std::endl;
    s << "Peak live heap " << (imp->most_pads * 8) << " bytes" << std::endl;
    s << "Peak System Alloc: " << imp->peak_alloc << std::endl;
    s << "------------------------------------------" << std::endl;
    s << "  Object type          Objects       Bytes" << std::endl;
    s << "  ----------------------------------------" << std::endl;
    for (size_t i = 0; i < sizeof(imp->peak) / sizeof(imp->peak[0]); ++i) {
      const HeapStats &x = imp->peak[i];
      if (!x.type) continue;
      s << "  " << std::setw(20) << std::left << x.type << std::setw(8) << std::right << x.objects
        << std::setw(12) << std::right << (x.pads * sizeof(PadObject)) << std::endl;
    }
    s << "------------------------------------------" << std::endl;
    // TODO: Add that this is coming from profile
    status_get_generic_stream(STREAM_REPORT) << s.str() << std::endl;
  }
}

struct ObjectStats {
  size_t objects;
  size_t pads;
  ObjectStats() : objects(0), pads(0) {}
};

struct StatOrder {
  typedef std::pair<const char *, ObjectStats> Kind;
  bool operator()(Kind a, Kind b) { return a.second.pads > b.second.pads; }
};

void Heap::GC(size_t requested_pads) {
  auto gc_start = std::chrono::system_clock::now();
  std::time_t current_t = std::chrono::system_clock::to_time_t(gc_start);
  std::tm *local_tm = std::localtime(&current_t);
  std::ostringstream curent_time_string;

  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(gc_start.time_since_epoch()) % 1000;

  curent_time_string << std::put_time(local_tm, "%H:%M:%S") << '.' << std::setfill('0')
                     << std::setw(3) << ms.count();
  imp->gc_count += 1;

  Space &from = imp->spaces[imp->space];
  size_t no_gc_overrun = (free - from.array) + requested_pads;
  size_t estimate_desired_size = imp->desired_size(imp->last_pads, requested_pads);
  size_t elems = std::max(no_gc_overrun, estimate_desired_size);

  // Resize the to space based on the above "calculation"
  imp->space ^= 1;
  Space &to = imp->spaces[imp->space];
  to.resize(elems);
  if (imp->peak_alloc < alloc()) imp->peak_alloc = alloc();

  Placement progress(to.array, to.array);
  std::map<const char *, ObjectStats> stats;

  // Move and compact all root objects over to the new space
  for (RootRing *root = roots.next; root != &roots; root = root->next) {
    if (!root->root) continue;
    auto out = root->root->moveto(progress.free);
    progress.free = out.free;
    root->root = out.obj;
  }

  int profile = imp->profile_heap;
  size_t total_objs = 0;
  size_t young_objects = 0;
  size_t mid_objects = 0;
  size_t old_objects = 0;
  // Iterating through the root objects we just moved and collect stats
  while (progress.obj != progress.free) {
    auto next = progress.obj->descend(progress.free);
    if (profile) {
      ObjectStats &s = stats[progress.obj->type()];
      ++s.objects;
      total_objs++;
      s.pads += (static_cast<PadObject *>(next.obj) - static_cast<PadObject *>(progress.obj));

      HeapObject *ho = static_cast<HeapObject *>(progress.obj);
      auto obj_age = HeapAgeTracker::getAge(ho);
      if (obj_age < 2) {
        ++young_objects;
      } else if (obj_age < 5) {
        ++mid_objects;
      } else {
        ++old_objects;
      }
    }
    progress = next;
  }

  DestroyableObject *tail = nullptr;
  HeapObject *next;
  size_t deleted_objs = 0;
  // Iterate through objects in the (from space) and delete those which weren't moved
  for (HeapObject *obj = imp->finalize; obj; obj = next) {
    // If we moved this object to the to space update its pointers to the to heap
    if (typeid(*obj) == typeid(MovedObject)) {
      MovedObject *mo = static_cast<MovedObject *>(obj);
      DestroyableObject *keep = static_cast<DestroyableObject *>(mo->to);
      next = keep->next;
      keep->next = tail;
      tail = keep;
    } else {  // if we did not move it destroy it
      next = static_cast<DestroyableObject *>(obj)->next;
      obj->~HeapObject();
      deleted_objs++;
    }
  }
  // Update to the last object in the to space
  imp->finalize = tail;

  end = to.array + elems;            // elems doesn't include the extra 50% from resize
  free = progress.free;              // The place to append new things on the heap
  imp->last_pads = free - to.array;  // how many bytes were copied to the to space
  // Contain heap growth due to no_gc_overrun pessimism
  size_t desired_sized = imp->desired_size(imp->last_pads, requested_pads);
  if (desired_sized < elems) {
    end =
        to.array + desired_sized;  // Update the end to be smaller if we don't need that much space
  }

  double actual_growth = alloc() / (double)imp->previous_alloc;
  imp->previous_alloc = alloc();

  auto gc_end = std::chrono::system_clock::now();
  std::chrono::duration<double> elapsed = gc_end - gc_start;
  double gc_duration_ms = elapsed.count() * 1000.0;
  imp->total_gc_time += gc_duration_ms;

  if (imp->profile_heap) {
    std::stringstream s;
    StatOrder order;
    std::vector<StatOrder::Kind> top(stats.begin(), stats.end());
    std::sort(top.begin(), top.end(), order);

    if (imp->gc_count == 1) {
      g_csv.open("heap_log.csv", std::ios::out | std::ios::trunc);
      g_csv << "GC#, Current Time Stamp, GC Cycle Duration (ms), Heap Factor, Actual Growth "
               "Factor, Total allocated (bytes), Current Semisphere, Semisphere 0 allocated, "
               "Semisphere 1 allocated, Live Heap (bytes), Free Space in Semi, "
            << "Percentage used of Semi, Percentage used of Alloc, Requested Space, Deleted, Young "
               "Objects (<2), Mid Objects (<5), Old Objects (>5), Total Objects\n";
    }

    if (imp->profile_heap > 1 && !top.empty()) {
      double free_space = (end - free) * sizeof(PadObject);
      double used_space = (imp->last_pads) * sizeof(PadObject);
      double total_space = free_space + used_space;
      double percentage_used_semi = (used_space / total_space) * 100;
      double percentage_used_allocated = (used_space / alloc()) * 100;

      s << "------------------------------------------" << std::endl;
      s << std::fixed << std::setprecision(2);
      s << "GC Number: " << imp->gc_count << std::endl;
      s << "Current Time Stamp: " << curent_time_string.str() << std::endl;
      s << "Current GC Duration: " << gc_duration_ms << " ms" << std::endl;
      s << "Total actual allocated: " << alloc() << std::endl;
      s << "Live heap: " << used_space << " bytes" << std::endl;
      s << "Free Space left in semisphere: " << free_space << " bytes" << std::endl;
      s << "Percentage used of semisphere: " << percentage_used_semi << std::endl;
      s << "Percentage used of total allocated: " << percentage_used_allocated << std::endl;

      s << "------------------------------------------" << std::endl;
      s << "  Object type          Objects       Bytes" << std::endl;
      s << "  ----------------------------------------" << std::endl;
      size_t max = top.size();
      if (max > 5) max = 5;
      for (size_t i = 0; i < max; ++i) {
        StatOrder::Kind &x = top[i];
        s << "  " << std::setw(20) << std::left << x.first << std::setw(8) << std::right
          << x.second.objects << std::setw(12) << std::right << (x.second.pads * sizeof(PadObject))
          << std::endl;
      }
      s << "------------------------------------------" << std::endl;
      g_csv << std::fixed << std::setprecision(2) << imp->gc_count << ", "
            << curent_time_string.str() << ", " << gc_duration_ms << ", " << imp->heap_factor
            << ", " << actual_growth << ", " << alloc() << ", " << imp->space << ", "
            << imp->spaces[0].alloc * sizeof(PadObject) << ", "
            << imp->spaces[1].alloc * sizeof(PadObject) << ", " << used_space << ", " << free_space
            << ", " << percentage_used_semi << ", " << percentage_used_allocated << ", "
            << requested_pads * sizeof(PadObject) << ", " << deleted_objs << ", " << young_objects
            << ", " << mid_objects << ", " << old_objects << ", " << total_objs << "\n";

      // TODO: Say that this is from profiling
      status_get_generic_stream(STREAM_REPORT) << s.str() << std::endl;
    }

    if (imp->last_pads > imp->most_pads) {
      imp->most_pads = imp->last_pads;
      size_t max = top.size();
      if (max > sizeof(imp->peak) / sizeof(imp->peak[0]))
        max = sizeof(imp->peak) / sizeof(imp->peak[0]);
      for (size_t i = 0; i < max; ++i) {
        StatOrder::Kind &x = top[i];
        HeapStats &hs = imp->peak[i];
        hs.type = x.first;
        hs.objects = x.second.objects;
        hs.pads = x.second.pads;
      }
    }
  }
}

Category Value::category() const { return VALUE; }

DestroyableObject::DestroyableObject(Heap &h) : next(h.imp->finalize) { h.imp->finalize = this; }
