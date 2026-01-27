# Content-Addressable Storage (CAS) Architecture

## Overview

This document describes the Content-Addressable Storage (CAS) system integrated into Wake. The CAS provides content-addressable storage for build outputs with a **CAS-first staging** architecture.

### Architecture: Separation of Concerns

The CAS system uses a **split architecture** for performance and scalability:

1. **FUSE Daemon** (`fuse-waked`): Lightweight, single-threaded
   - Intercepts filesystem operations
   - Writes to staging directory (`.cas/staging/`)
   - Tracks metadata (permissions, timestamps)
   - Outputs staging paths + metadata in JSON
   - Does **NOT** hash files or store in CAS

2. **Wakebox Client** (`wakebox`): Per-job, parallel
   - Receives staging paths and metadata from FUSE daemon
   - Hashes staged files (can run in parallel across jobs)
   - Stores content in CAS (`.cas/blobs/`)
   - Materializes outputs to workspace
   - Applies metadata (permissions, timestamps)

This separation keeps the FUSE daemon fast and responsive while allowing expensive operations (hashing, I/O) to run in parallel across jobs.

## Goals

1. **Content Deduplication**: Store build outputs by their content hash, eliminating duplicate storage
2. **Efficient Caching**: Enable cache hit recovery even when output files are deleted
3. **Concurrent Build Isolation**: Enable multiple concurrent Wake invocations with isolated views of the workspace
4. **Simple Design**: No Merkle trees - keep the implementation straightforward

## Why CAS-First Staging for Concurrent Builds

Traditional build systems have a fundamental problem with concurrent builds: **file conflicts**. When two Wake invocations run simultaneously, they may:

1. **Read-Write Conflicts**: Job A reads a file while Job B is writing to it
2. **Write-Write Conflicts**: Both jobs write to the same output file
3. **Stale Reads**: Job A reads an output that Job B is about to overwrite
4. **Partial Writes**: Job A sees a partially-written file from Job B

### The CAS-First Solution

CAS-first staging solves these problems by **isolating job outputs until completion**:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    CONCURRENT BUILD ISOLATION                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  Wake Invocation 1          Wake Invocation 2                       │
│  ┌─────────────┐            ┌─────────────┐                         │
│  │   Job A     │            │   Job B     │                         │
│  │  (running)  │            │  (running)  │                         │
│  └──────┬──────┘            └──────┬──────┘                         │
│         │                          │                                 │
│         ▼                          ▼                                 │
│  ┌─────────────┐            ┌─────────────┐                         │
│  │  Staging A  │            │  Staging B  │   ◄── Isolated writes   │
│  │ (temp files)│            │ (temp files)│                         │
│  └──────┬──────┘            └──────┬──────┘                         │
│         │ close()                  │ close()                        │
│         ▼                          ▼                                 │
│  ┌─────────────────────────────────────────┐                        │
│  │              CAS STORE                   │   ◄── Content-addressed│
│  │         .cas/blobs/{hash}                │       (deduplicated)   │
│  └─────────────────────────────────────────┘                        │
│         │ job complete             │ job complete                   │
│         ▼                          ▼                                 │
│  ┌─────────────────────────────────────────┐                        │
│  │              WORKSPACE                   │   ◄── Final outputs    │
│  │    (materialized from CAS at end)        │       (atomic)         │
│  └─────────────────────────────────────────┘                        │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

**Key Benefits**:

1. **No Read-Write Conflicts**: Jobs read from stable workspace; writes go to isolated staging
2. **No Partial Writes Visible**: Other jobs never see incomplete files
3. **Atomic Completion**: Outputs appear in workspace only when job succeeds
4. **Content Deduplication**: If two jobs produce identical files, CAS stores only one copy
5. **Cache Sharing**: Multiple Wake invocations can share cached outputs via CAS

### How It Works

1. **Job writes file** → FUSE daemon writes to `.cas/staging/{unique_id}` (not workspace)
2. **Job closes file** → Staging file remains; FUSE tracks path and metadata
3. **Job reads its own output** → FUSE serves content from staging file
4. **Job completes** → FUSE outputs staging paths + metadata in JSON
5. **Wakebox processes outputs**:
   - Hashes each staging file
   - Stores in CAS (`.cas/blobs/`)
   - Materializes to workspace with correct permissions/timestamps
   - Cleans up staging files
6. **Other jobs** → See stable workspace, not in-progress writes

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Data Flow (Split Architecture)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Job Process                FUSE Daemon                 Wakebox              │
│  ───────────                ───────────                 ───────              │
│                                                                              │
│  open("out.txt")  ────────► create staging file                             │
│                             .cas/staging/1                                   │
│                             track metadata                                   │
│                                                                              │
│  write(data)      ────────► write to staging                                │
│                                                                              │
│  chmod(0755)      ────────► track mode: 0755                                │
│                                                                              │
│  utimens(ts)      ────────► track timestamp: ts                             │
│                                                                              │
│  close()          ────────► keep staging file                               │
│                             (NO hashing here!)                              │
│                                                                              │
│  [job exits]                                                                 │
│                                                                              │
│                   ────────► output JSON:           ────────►                │
│                             {                                                │
│                               "staging_files": {                            │
│                                 "out.txt": {                                │
│                                   "staging_path": ".cas/staging/1",         │
│                                   "mode": 493,                              │
│                                   "mtime_sec": 1234567890,                  │
│                                   "mtime_nsec": 123456789                   │
│                                 }                                           │
│                               }                                             │
│                             }                                               │
│                                                          │                  │
│                                                          ▼                  │
│                                                   hash staging file         │
│                                                   store in CAS              │
│                                                   materialize to workspace  │
│                                                   apply mode & timestamps   │
│                                                   delete staging file       │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Design Principles

- **CAS-First Staging**: Outputs are written to staging, NOT directly to workspace
- **Deferred Processing**: Hashing and CAS storage happen in wakebox (parallel)
- **Deferred Materialization**: Workspace files created only at job completion
- **Metadata Preservation**: Permissions and timestamps tracked and applied
- **Isolated Views via FUSE**: Each job sees its own outputs through staging
- **Reflink-based Storage**: Files are stored using reflinks (copy-on-write) when supported
- **No Merkle Trees**: Individual files are stored by content hash only

## Directory Structure

```
workspace/
├── .cas/                    # CAS store root
│   ├── blobs/               # Content-addressed file storage
│   │   ├── a1/              # First 2 hex chars of hash (sharding)
│   │   │   └── b2c3d4...    # Remaining 62 hex chars (content)
│   │   └── ...
│   └── staging/             # Temporary staging for in-progress writes
│       └── {counter}        # Unique staging files (kept until wakebox processes)
├── .fuse/                   # FUSE mount point (for job isolation)
├── wake.db                  # Wake database (unchanged)
└── <outputs>                # Build outputs (materialized by wakebox at job end)
```

## Core Components

### 1. Content Hash (`src/cas/cas.h`)

256-bit BLAKE2b content hash with:
- `ContentHash::from_file(path)` - Hash a file's contents
- `ContentHash::from_string(data)` - Hash arbitrary data
- `to_hex()` / `from_hex()` - Hex string conversion
- `prefix()` / `suffix()` - For directory sharding (first 2 chars / rest)

### 2. CASStore (`src/cas/cas_store.h`)

Simplified storage interface (blob-only, no trees):

```cpp
class CASStore {
  // Blob operations
  wcl::result<ContentHash, CASError> store_blob_from_file(const std::string& path);
  wcl::result<ContentHash, CASError> store_blob(const std::string& data);
  bool has_blob(const ContentHash& hash) const;
  wcl::result<bool, CASError> materialize_blob(hash, dest_path, mode) const;

  // Read blob contents
  wcl::result<std::string, CASError> read_blob(const ContentHash& hash) const;

  // Get path to blob in store
  std::string blob_path(const ContentHash& hash) const;
};
```

### 3. File Operations (`src/cas/file_ops.h`)

Efficient file copying with fallback chain:

```
reflink (FICLONE) → full copy
```

- **Reflink**: Copy-on-write clone, shares disk blocks (fastest, preferred)
- **Full copy**: Traditional copy with correct mode (slowest, but always works)

Note: Hardlinks are not used for CAS materialization because they share the same inode, preventing different files from having different permissions.

## CAS-First Staging Architecture

### FUSE Daemon (`tools/fuse-waked/main.cpp`)

The FUSE daemon handles staging and metadata tracking (lightweight operations only):

#### Key Data Structures

```cpp
// Track files being written to staging
struct StagedFile {
  std::string staging_path;  // Path in .cas/staging/{id}
  std::string dest_path;     // Final destination (relative to workspace)
  std::string job_id;        // Job that owns this file
  mode_t mode;               // File mode (permissions)
  struct timespec mtime;     // Modification timestamp
  struct timespec atime;     // Access timestamp
  int open_count;            // Reference count for open FDs
};

// Per-job tracking (for staged files, not CAS hashes)
struct Job {
  std::set<std::string> files_visible;   // Files job can see
  std::set<std::string> files_read;      // Files job has read
  std::set<std::string> files_wrote;     // Files job has written
  std::map<std::string, StagedFile*> staged_files;  // path -> staged file info
  // NOTE: No file_hashes - hashing moved to wakebox
};
```

#### Write Path (Staging Only)

1. **`create()` / `open(O_CREAT)`**: File is created in `.cas/staging/{counter}`
2. **`write()`**: Data is written to the staging file
3. **`chmod()`**: Mode is tracked in `StagedFile.mode`
4. **`utimens()`**: Timestamps are tracked in `StagedFile.mtime/atime`
5. **`release()` (close)**:
   - Staging file remains on disk (NOT deleted)
   - Metadata continues to be tracked
   - **No hashing or CAS storage** (deferred to wakebox)

#### Read Path (From Staging)

When a job reads a file it previously wrote:
1. **`getattr()`**: If file is staged, stat info comes from staging file
2. **`open()`**: Staging file is opened
3. **`read()`**: Content is read from staging file

This allows jobs to read their own outputs before wakebox processes them.

#### Job Completion (Output JSON)

When a job completes, FUSE daemon outputs staging file metadata:
```json
{
  "inputs": ["src/foo.cpp", "include/bar.h"],
  "outputs": ["build/foo.o"],
  "staging_files": {
    "build/foo.o": {
      "staging_path": ".cas/staging/1",
      "mode": 493,
      "mtime_sec": 1234567890,
      "mtime_nsec": 123456789
    }
  }
}
```

### Wakebox Client (`src/wakefs/fuse.cpp`)

Wakebox receives the JSON from FUSE and performs expensive operations:

#### Processing Staged Files

```cpp
void process_staging_files(const JAST& staging_files) {
  for (const auto& entry : staging_files.children) {
    const std::string& dest_path = entry.first;
    const std::string& staging_path = entry.second.get("staging_path").value;
    mode_t mode = std::stoul(entry.second.get("mode").value);
    time_t mtime_sec = std::stol(entry.second.get("mtime_sec").value);
    long mtime_nsec = std::stol(entry.second.get("mtime_nsec").value);

    // 1. Hash the staging file
    auto hash = cas::ContentHash::from_file(staging_path);

    // 2. Store in CAS (idempotent - deduplicates automatically)
    g_cas_store->store_blob_from_file(staging_path);

    // 3. Materialize to workspace
    g_cas_store->materialize_blob(hash, dest_path, mode);

    // 4. Apply timestamp
    struct timespec times[2] = {{0, UTIME_OMIT}, {mtime_sec, mtime_nsec}};
    utimensat(AT_FDCWD, dest_path.c_str(), times, 0);

    // 5. Clean up staging file
    unlink(staging_path.c_str());
  }
}
```

### Special Case Handling

#### chmod After Close

Some tools (like ccache) call `chmod()` after closing a file:
```cpp
static int wakefuse_chmod(const char *path, mode_t mode) {
  // Update mode in staged file tracking
  auto staged_it = staged_files.find(path);
  if (staged_it != staged_files.end()) {
    staged_it->second->mode = mode & ~S_IFMT;  // Strip type bits
  }
  // chmod on staging file directly
}
```

#### Rename of Staged Files

When a tool renames a file that's staged:
```cpp
static int wakefuse_rename(const char *from, const char *to) {
  // Move staging file to new path
  auto staged_it = staged_files.find(from);
  if (staged_it != staged_files.end()) {
    staged_files[to] = staged_it->second;
    staged_files[to]->dest_path = to;
    staged_files.erase(from);
  }
}
```

#### Unlink of Staged Files

When a tool deletes a staged file:
```cpp
static int wakefuse_unlink(const char *path) {
  auto staged_it = staged_files.find(path);
  if (staged_it != staged_files.end()) {
    unlink(staged_it->second->staging_path.c_str());
    staged_files.erase(staged_it);
  }
}
```

## Job Integration

### Job Output Storage

With the split architecture, outputs are processed at job completion:

1. **During execution**: Files written to staging, metadata tracked
2. **At job exit**: FUSE outputs JSON with staging paths + metadata
3. **Wakebox processes**: Hashes, stores in CAS, materializes to workspace
4. **Job database**: Hashes recorded for future cache hits

### Cache Hit Recovery

When a job is cached but outputs are missing, they can be recovered from CAS:

```cpp
// On cache hit, materialize missing outputs from CAS
for (const auto &file : cached_files) {
  if (access(file.path.c_str(), R_OK) != 0 && !file.hash.empty()) {
    auto hash = ContentHash::from_hex(file.hash);
    if (cas_store->has_blob(hash)) {
      cas_store->materialize_blob(hash, file.path, file.mode);
    }
  }
}
```

This enables:
- Cache hits even when output files were deleted
- Content deduplication across builds
- Efficient storage via reflinks
- **Concurrent builds without file conflicts**

### Wake Primitives (`src/runtime/cas_prim.cpp`)

Runtime primitives exposed to the Wake language (simplified, no tree operations):

| Primitive | Description |
|-----------|-------------|
| `cas_store_file` | Store a file, return content hash |
| `cas_has_blob` | Check if blob exists |
| `cas_materialize_file` | Materialize file from CAS to path |
| `cas_store_path` | Get CAS store path |

### Wake Language API (`share/wake/lib/system/cas.wake`)

```wake
# Store a file, get its hash
export def casStoreFile (path: String): Result String Error

# Check if blob exists
export def casHasBlob (hash: String): Boolean

# Materialize file from CAS to a path
export def casMaterializeFile (hash: String) (destPath: String) (mode: Integer): Result Unit Error

# Get the CAS store path
export def casStorePath: String
```

## Initialization Flow

```
main.cpp
    │
    ├─► CASContext cas_ctx;
    │   cas::CASStore* cas_store = cas_ctx.get_store(workspace);
    │       │
    │       └─► Creates .cas/blobs/ directory
    │
    ├─► JobTable jobtable(&db, cas_store, ...);
    │       │
    │       └─► Stores cas_store pointer for job finish handling
    │
    └─► prim_register_all(&info, &jobtable, &cas_ctx);
            │
            └─► Registers CAS primitives with Wake runtime
```

## Files

### Modified Files

| File | Change |
|------|--------|
| `Makefile` | Added `src/cas` to `WAKE_DIRS`, CAS_OBJS for wakebox |
| `src/runtime/job.h` | Added `cas::CASStore*` to JobTable constructor |
| `src/runtime/job.cpp` | Materialize from CAS on cache hit |
| `src/runtime/prim.h` | Added CASContext, prim_register_cas |
| `src/runtime/prim.cpp` | Added CAS primitive registration |
| `tools/wake/main.cpp` | Initialize CASContext, pass to JobTable |
| `tools/fuse-waked/main.cpp` | Staging directory, metadata tracking, output JSON (no CAS) |
| `src/wakefs/fuse.cpp` | CAS initialization, hashing, storage, materialization |

### CAS Core Files

| File | Purpose |
|------|---------|
| `src/cas/cas.h` | ContentHash class (simplified, no Tree) |
| `src/cas/cas.cpp` | Implementation of content hashing |
| `src/cas/cas_store.h` | CASStore class interface (blob-only) |
| `src/cas/cas_store.cpp` | CASStore implementation |
| `src/cas/file_ops.h` | File operation utilities interface |
| `src/cas/file_ops.cpp` | Reflink/hardlink/copy implementation |

## Current Behavior (Split Architecture)

1. **On file create/open**: FUSE creates file in `.cas/staging/` (not workspace)
2. **On file write**: Data written directly to staging file
3. **On chmod/utimens**: Metadata tracked in `StagedFile` struct
4. **On file close**: Staging file remains; metadata preserved
5. **During job execution**: Job sees its outputs via FUSE (reads from staging)
6. **On job exit**: FUSE outputs JSON with staging paths + metadata
7. **Wakebox processes**:
   - Hashes each staging file
   - Stores in CAS (`.cas/blobs/`)
   - Materializes to workspace with permissions/timestamps
   - Cleans up staging files
8. **Deduplication**: Identical files share storage in CAS
9. **Cache recovery**: On cache hit with missing files, materialize from CAS
10. **Concurrent builds**: No file conflicts - each job writes to isolated staging

## FUSE Daemon Details

The fuse-waked daemon handles staging and metadata tracking only:

### fuse-waked Initialization (`tools/fuse-waked/main.cpp`)

On daemon startup:
```cpp
// Initialize staging directory (no CAS store needed in FUSE daemon)
g_staging_dir = ".cas/staging";
mkdir(g_staging_dir.c_str(), 0755);
```

### Write Path (Staging)

```cpp
// create() - File goes to staging
static int wakefuse_create(const char *path, mode_t mode, fuse_file_info *fi) {
  std::string staging_path = g_staging_dir + "/" + std::to_string(g_staging_counter++);
  int fd = open(staging_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);

  // Track the staged file with metadata
  StagedFile staged{staging_path, dest_path, job_id, mode, {0, 0}, {0, 0}, 1};
  g_staged_files[{job_id, dest_path}] = staged;
  g_fd_to_staged[fd] = &g_staged_files[{job_id, dest_path}];

  fi->fh = fd;
  return 0;
}

// release() - Keep staging file for wakebox to process
static int wakefuse_release(const char *path, fuse_file_info *fi) {
  if (staged_file) {
    close(fi->fh);
    staged_file->open_count--;
    // Staging file remains on disk - wakebox will process it
    // No hashing or CAS storage here!
  }
}
```

### Metadata Tracking

```cpp
// chmod() - Track mode in staged file
static int wakefuse_chmod(const char *path, mode_t mode) {
  auto staged_it = g_staged_files.find({job_id, dest_path});
  if (staged_it != g_staged_files.end()) {
    staged_it->second.mode = mode & 07777;  // Strip type bits
  }
  // Also chmod the staging file itself
  fchmod(staged_fd, mode & 07777);
}

// utimens() - Track timestamps in staged file
static int wakefuse_utimens(const char *path, const struct timespec ts[2]) {
  auto staged_it = g_staged_files.find({job_id, dest_path});
  if (staged_it != g_staged_files.end()) {
    staged_it->second.atime = ts[0];
    staged_it->second.mtime = ts[1];
  }
}
```

### JSON Output Format

The fuse-waked daemon outputs staging file metadata (no hashes):

```json
{
  "ibytes": 1234,
  "obytes": 5678,
  "inputs": ["src/foo.cpp", "include/bar.h"],
  "outputs": ["build/foo.o", "build/bar.o"],
  "staging_files": {
    "build/foo.o": {
      "staging_path": ".cas/staging/1",
      "mode": 493,
      "mtime_sec": 1234567890,
      "mtime_nsec": 123456789
    },
    "build/bar.o": {
      "staging_path": ".cas/staging/2",
      "mode": 420,
      "mtime_sec": 1234567891,
      "mtime_nsec": 0
    }
  }
}
```

## Wakebox Integration

Wakebox receives FUSE output and performs CAS operations:

### Wakebox Processing (`src/wakefs/fuse.cpp`)

```cpp
// After FUSE daemon exits, process staged files
void process_fuse_output(const JAST& fuse_json) {
  // Initialize CAS store (if not already)
  auto cas_store = cas::CASStore::open(".cas");

  // Process each staged file
  for (const auto& entry : fuse_json.get("staging_files").children) {
    const std::string& dest_path = entry.first;
    const auto& info = entry.second;

    std::string staging_path = info.get("staging_path").value;
    mode_t mode = std::stoul(info.get("mode").value);
    time_t mtime_sec = std::stol(info.get("mtime_sec").value);
    long mtime_nsec = std::stol(info.get("mtime_nsec").value);

    // Hash and store in CAS
    auto hash = cas_store->store_blob_from_file(staging_path);

    // Materialize to workspace
    cas_store->materialize_blob(*hash, dest_path, mode);

    // Apply timestamp
    struct timespec times[2] = {{0, UTIME_OMIT}, {mtime_sec, mtime_nsec}};
    utimensat(AT_FDCWD, dest_path.c_str(), times, 0);

    // Clean up staging file
    unlink(staging_path.c_str());
  }
}
```

## Concurrent Build Scenarios

### Scenario 1: Two Independent Jobs

```
Wake 1: Job A (compiling foo.cpp → foo.o)
Wake 2: Job B (compiling bar.cpp → bar.o)

Timeline:
  t1: FUSE: A creates staging/1, B creates staging/2
  t2: FUSE: A writes to staging/1, B writes to staging/2
  t3: FUSE: A closes (keeps staging/1), B closes (keeps staging/2)
  t4: A exits → wakebox A: hash, store, materialize foo.o
  t5: B exits → wakebox B: hash, store, materialize bar.o (parallel!)

Result: No conflicts, both outputs correctly in workspace
        Wakebox processes run in parallel across jobs
```

### Scenario 2: Same Output File

```
Wake 1: Job A (building libfoo.a)
Wake 2: Job B (building libfoo.a with different inputs)

Timeline:
  t1: FUSE: A creates staging/1, B creates staging/2
  t2: FUSE: A writes to staging/1, B writes to staging/2
  t3: FUSE: A closes, B closes (both staging files exist)
  t4: A exits → wakebox A: hash, store, materialize libfoo.a
  t5: B exits → wakebox B: hash, store, materialize libfoo.a (overwrites)

Result: Last completion wins, but no partial files visible
        Both hashes stored in CAS (content-addressed)
```

### Scenario 3: Job Reads Another's Output

```
Wake 1: Job A (compiling foo.cpp → foo.o)
Wake 2: Job B (linking foo.o → foo.exe, depends on Job A)

Timeline:
  t1: FUSE: A writes to staging (foo.o not in workspace)
  t2: A exits → wakebox A: hash, store, materialize foo.o
  t3: B starts, reads foo.o from workspace (stable, complete)
  t4: FUSE: B writes foo.exe to staging
  t5: B exits → wakebox B: hash, store, materialize foo.exe

Result: B sees complete foo.o, not partial/in-progress
        Metadata (mode, timestamps) preserved through the flow
```

## Future Work

1. **Remote CAS**: Support remote content-addressable storage backends
2. **Garbage Collection**: Clean up unreferenced CAS blobs
3. **Hash-based Job Caching**: Use content hashes for finer-grained cache invalidation
4. **Parallel Wakebox Processing**: Multiple wakebox instances process outputs concurrently
5. **Shared CAS Across Workspaces**: Enable cross-workspace content sharing
6. **Timestamp Preservation**: Apply timestamps during materialization for incremental compilation tools
