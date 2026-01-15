# Content-Addressable Storage (CAS) Architecture

## Overview

This document describes the Content-Addressable Storage (CAS) system integrated into Wake. The CAS provides content-addressable storage for build outputs with a **CAS-first staging** architecture. Build outputs are written to a staging area, immediately stored in CAS when closed, and only materialized to the workspace at job completion. This enables content deduplication, efficient caching, cache hit recovery, and **concurrent Wake invocations with isolated views**.

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

1. **Job writes file** → Write goes to `.cas/staging/{unique_id}` (not workspace)
2. **Job closes file** → File is hashed and stored in `.cas/blobs/{hash}`
3. **Job reads its own output** → FUSE serves content from CAS (via file_hashes map)
4. **Job completes** → All outputs materialized from CAS to workspace paths
5. **Other jobs** → See stable workspace, not in-progress writes

## Design Principles

- **CAS-First Staging**: Outputs are written to staging, stored in CAS immediately on close
- **Deferred Materialization**: Workspace files created only at job completion
- **Isolated Views via FUSE**: Each job sees its own outputs through file_hashes tracking
- **Reflink-based Storage**: Files are stored using reflinks (copy-on-write) when supported
- **No Merkle Trees**: Individual files are stored by content hash only - no directory-level hashing

## Directory Structure

```
workspace/
├── .cas/                    # CAS store root
│   ├── blobs/               # Content-addressed file storage
│   │   ├── a1/              # First 2 hex chars of hash (sharding)
│   │   │   └── b2c3d4...    # Remaining 62 hex chars (content)
│   │   └── ...
│   └── staging/             # Temporary staging for in-progress writes
│       └── {counter}        # Unique staging files (cleaned up on close)
├── .fuse/                   # FUSE mount point (for job isolation)
├── wake.db                  # Wake database (unchanged)
└── <outputs>                # Build outputs (materialized from CAS at job end)
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

### FUSE Daemon Integration (`tools/fuse-waked/main.cpp`)

The FUSE daemon implements CAS-first staging to enable isolated job execution:

#### Key Data Structures

```cpp
// Track files being written to staging
struct StagedFile {
  std::string staging_path;  // Path in .cas/staging/{id}
  std::string dest_path;     // Final destination (relative to workspace)
  std::string job_id;        // Job that owns this file
  mode_t mode;               // File mode (permissions)
  int open_count;            // Reference count for open FDs
};

// Per-job tracking
struct Job {
  std::set<std::string> files_visible;   // Files job can see
  std::set<std::string> files_read;      // Files job has read
  std::set<std::string> files_wrote;     // Files job has written
  std::map<std::string, std::string> file_hashes;  // path -> content hash
  std::map<std::string, mode_t> file_modes;        // path -> file mode
  // ...
};
```

#### Write Path (CAS-First)

1. **`create()` / `open(O_CREAT)`**: File is created in `.cas/staging/{counter}`
2. **`write()`**: Data is written to the staging file
3. **`release()` (close)**:
   - File is hashed using BLAKE2b
   - Content is stored in `.cas/blobs/{prefix}/{suffix}`
   - Hash and mode are recorded in job's `file_hashes` and `file_modes`
   - Staging file is deleted
   - File is NOT yet in workspace!

#### Read Path (Virtual)

When a job reads a file it previously wrote:
1. **`getattr()`**: If file has a hash, stat info is synthesized from CAS blob
2. **`open()`**: Blob is opened from CAS store
3. **`read()`**: Content is read from CAS blob

This allows jobs to read their own outputs even though they're not in the workspace.

#### Job Completion (Materialization)

When a job completes successfully:
1. **`materialize_outputs_from_cas()`** is called
2. For each entry in `file_hashes`:
   - Create parent directories as needed
   - Materialize file from CAS to workspace path
   - Apply correct file mode from `file_modes`
3. Files now appear in workspace for other jobs/builds to see

### Special Case Handling

#### chmod After Close

Some tools (like ccache) call `chmod()` after closing a file:
```cpp
static int wakefuse_chmod(const char *path, mode_t mode) {
  // Update our mode tracking even if file is in CAS
  auto hash_it = it->second.file_hashes.find(key.second);
  if (hash_it != it->second.file_hashes.end()) {
    it->second.file_modes[key.second] = mode & ~S_IFMT;  // Strip type bits
  }
  // chmod on physical file may fail with ENOENT (expected)
}
```

#### Rename of CAS-Staged Files

When a tool renames a file that's already in CAS:
```cpp
static int wakefuse_rename(const char *from, const char *to) {
  // If source is in CAS and rename fails with ENOENT, that's expected
  if (source_in_cas && errno == ENOENT) {
    // Just move the hash to the new name
    file_hashes[new_name] = file_hashes[old_name];
    file_hashes.erase(old_name);
  }
}
```

#### Unlink of CAS-Staged Files

When a tool deletes a file that's in CAS:
```cpp
static int wakefuse_unlink(const char *path) {
  // If file is in CAS and unlink fails with ENOENT, that's expected
  if (file_in_cas && errno == ENOENT) {
    // Remove from tracking
    file_hashes.erase(path);
    file_modes.erase(path);
  }
}
```

## Job Integration

### Job Output Storage

With CAS-first staging, outputs are stored incrementally during job execution:

1. **During execution**: Each file close stores content in CAS
2. **At job finish**: `materialize_outputs_from_cas()` creates workspace files
3. **Job database**: Hashes are recorded for future cache hits

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
| `Makefile` | Added `src/cas` to `WAKE_DIRS`, CAS_OBJS for fuse-waked |
| `src/runtime/job.h` | Added `cas::CASStore*` to JobTable constructor |
| `src/runtime/job.cpp` | Store outputs in CAS on job finish, materialize on cache hit |
| `src/runtime/prim.h` | Added CASContext, prim_register_cas |
| `src/runtime/prim.cpp` | Added CAS primitive registration |
| `tools/wake/main.cpp` | Initialize CASContext, pass to JobTable |
| `tools/fuse-waked/main.cpp` | CAS store initialization, store outputs in CAS, output_hashes in JSON |
| `tools/fuse-waked/fuse-waked.wake` | Added casLib dependency |

### CAS Core Files

| File | Purpose |
|------|---------|
| `src/cas/cas.h` | ContentHash class (simplified, no Tree) |
| `src/cas/cas.cpp` | Implementation of content hashing |
| `src/cas/cas_store.h` | CASStore class interface (blob-only) |
| `src/cas/cas_store.cpp` | CASStore implementation |
| `src/cas/file_ops.h` | File operation utilities interface |
| `src/cas/file_ops.cpp` | Reflink/hardlink/copy implementation |

## Current Behavior (CAS-First Staging)

1. **On file create/open**: File is created in `.cas/staging/` (not workspace)
2. **On file close**: Content is hashed, stored in `.cas/blobs/`, staging file deleted
3. **During job execution**: Job sees its outputs via FUSE (reads from CAS)
4. **On job finish**: All outputs materialized from CAS to workspace
5. **Deduplication**: Identical files share storage in CAS
6. **Cache recovery**: On cache hit with missing files, materialize from CAS
7. **Concurrent builds**: No file conflicts - each job writes to isolated staging

## FUSE Integration

The fuse-waked daemon implements CAS-first staging:

### fuse-waked Initialization (`tools/fuse-waked/main.cpp`)

On daemon startup:
```cpp
// Initialize CAS store
auto cas_result = cas::CASStore::open(".cas");
if (cas_result) {
  g_cas_store = std::make_unique<cas::CASStore>(std::move(*cas_result));
  g_staging_dir = ".cas/staging";
  mkdir(g_staging_dir.c_str(), 0755);
}
```

### Write Path (Staging)

```cpp
// create() - File goes to staging
static int wakefuse_create(const char *path, mode_t mode, fuse_file_info *fi) {
  std::string staging_path = g_staging_dir + "/" + std::to_string(g_staging_counter++);
  int fd = open(staging_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);

  // Track the staged file
  StagedFile staged{staging_path, dest_path, job_id, mode, 1};
  g_staged_files[{job_id, dest_path}] = staged;
  g_fd_to_staged[fd] = &g_staged_files[{job_id, dest_path}];

  fi->fh = fd;
  return 0;
}

// release() - Store in CAS, remove staging file
static int wakefuse_release(const char *path, fuse_file_info *fi) {
  if (staged_file) {
    // Hash and store in CAS
    auto hash = g_cas_store->store_blob_from_file(staging_path);
    job.file_hashes[dest_path] = hash.to_hex();
    job.file_modes[dest_path] = staged_file->mode;

    // Remove staging file
    unlink(staging_path.c_str());
  }
}
```

### Job Completion (Materialization)

```cpp
void materialize_outputs_from_cas(Job &job) {
  for (const auto &[path, hash_hex] : job.file_hashes) {
    auto hash = cas::ContentHash::from_hex(hash_hex);
    mode_t mode = job.file_modes.count(path) ? job.file_modes[path] : 0644;

    // Create parent directories
    mkdir_parents(dirname(path));

    // Materialize from CAS to workspace
    cas::materialize_file(*g_cas_store, hash, path, mode);
  }
}
```

### JSON Output Format

The fuse-waked daemon outputs content hashes in a separate `output_hashes` field:

```json
{
  "ibytes": 1234,
  "obytes": 5678,
  "inputs": ["src/foo.cpp", "include/bar.h"],
  "outputs": ["build/foo.o", "build/bar.o"],
  "output_hashes": {
    "build/foo.o": "a1b2c3d4e5f6...",
    "build/bar.o": "f6e5d4c3b2a1..."
  },
  "output_modes": {
    "build/foo.o": 420,
    "build/bar.o": 493
  }
}
```

This format is backward compatible - `outputs` remains an array of strings.

### Build Configuration

The fuse-waked build includes CAS as a dependency (`tools/fuse-waked/fuse-waked.wake`):
```wake
target buildFuseDaemon variant: Result (List Path) Error = match variant
    _ -> tool @here Nil variant "lib/wake/fuse-waked" (json, fuse, util, casLib, Nil) Nil Nil
```

## Concurrent Build Scenarios

### Scenario 1: Two Independent Jobs

```
Wake 1: Job A (compiling foo.cpp → foo.o)
Wake 2: Job B (compiling bar.cpp → bar.o)

Timeline:
  t1: A creates staging/1, B creates staging/2
  t2: A writes to staging/1, B writes to staging/2
  t3: A closes → blob/{hash_a}, B closes → blob/{hash_b}
  t4: A completes → materialize foo.o, B completes → materialize bar.o

Result: No conflicts, both outputs correctly in workspace
```

### Scenario 2: Same Output File

```
Wake 1: Job A (building libfoo.a)
Wake 2: Job B (building libfoo.a with different inputs)

Timeline:
  t1: A creates staging/1, B creates staging/2
  t2: A writes to staging/1, B writes to staging/2
  t3: A closes → blob/{hash_a}, B closes → blob/{hash_b}
  t4: A completes → materialize libfoo.a (from hash_a)
  t5: B completes → materialize libfoo.a (from hash_b, overwrites)

Result: Last completion wins, but no partial files visible
```

### Scenario 3: Job Reads Another's Output

```
Wake 1: Job A (compiling foo.cpp → foo.o)
Wake 2: Job B (linking foo.o → foo.exe, depends on Job A)

Timeline:
  t1: A writes to staging, B waits (foo.o not visible)
  t2: A completes → materialize foo.o
  t3: B starts, reads foo.o from workspace (stable)
  t4: B writes foo.exe to staging
  t5: B completes → materialize foo.exe

Result: B sees complete foo.o, not partial/in-progress
```

## Future Work

1. **Remote CAS**: Support remote content-addressable storage backends
2. **Garbage Collection**: Clean up unreferenced CAS blobs
3. **Hash-based Job Caching**: Use content hashes for finer-grained cache invalidation
4. **Parallel Materialization**: Materialize multiple outputs concurrently
5. **Shared CAS Across Workspaces**: Enable cross-workspace content sharing
