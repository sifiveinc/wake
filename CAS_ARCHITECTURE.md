# Content-Addressable Storage (CAS) Architecture

## Overview

This document describes the Content-Addressable Storage (CAS) system integrated into Wake. The CAS provides content-addressable storage for build outputs with a **CAS-first staging** architecture.

### Architecture: Three-Component Split

The CAS system uses a **split architecture** for performance, scalability, and remote execution compatibility:

1. **FUSE Daemon** (`fuse-waked`): Lightweight, single-threaded
   - Intercepts filesystem operations
   - Writes to staging directory (`.cas/staging/`)
   - Tracks metadata (permissions, timestamps)
   - Outputs staging paths + metadata in JSON
   - Does **NOT** hash files or store in CAS

2. **Wakebox Client** (`wakebox`): Per-job, parallel, runs locally OR remotely
   - Receives staging paths and metadata from FUSE daemon
   - Hashes staged files (can run in parallel across jobs)
   - **Stores files in CAS** (`.cas/blobs/{prefix}/{suffix}`)
   - Deletes staging files after CAS storage (no longer needed)
   - Outputs hash in JSON (no staging_path for files - already in CAS)
   - Does **NOT** materialize to workspace (Wake does this)

3. **Wake** (main process): Centralized, consistent
   - Receives JSON from wakebox (with hash included)
   - For remote execution: receives CAS blobs via rsync (not staging files)
   - **Materializes outputs from CAS to workspace** (no CAS storage needed)
   - Applies metadata (permissions, timestamps)
   - Creates symlinks and directories

This separation ensures:
- FUSE daemon stays fast and responsive
- Hashing and CAS storage run in parallel per-job (in wakebox)
- Materialization runs locally (in Wake)
- **Same data flow for local and remote execution**
- **Less data transferred for remote execution** (CAS blobs are deduplicated)

## Goals

1. **Content Deduplication**: Store build outputs by their content hash, eliminating duplicate storage
2. **Efficient Caching**: Enable cache hit recovery even when output files are deleted
3. **Concurrent Build Isolation**: Enable multiple concurrent Wake invocations with isolated views of the workspace
4. **CAS-Based Reads**: Jobs read input files directly from CAS (by hash), not workspace, ensuring content correctness
5. **Simple Design**: No Merkle trees - keep the implementation straightforward

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

1. **No Read-Write Conflicts**: Jobs read from CAS by hash; writes go to isolated staging
2. **No Partial Writes Visible**: Other jobs never see incomplete files
3. **Atomic Completion**: Outputs appear in workspace only when job succeeds
4. **Content Deduplication**: If two jobs produce identical files, CAS stores only one copy
5. **Cache Sharing**: Multiple Wake invocations can share cached outputs via CAS
6. **Content Correctness**: Jobs always read the exact content they were promised (by hash)

### How It Works

1. **Job writes file** → FUSE daemon writes to `.cas/staging/{unique_id}` (not workspace)
2. **Job closes file** → Staging file remains; FUSE tracks path and metadata
3. **Job reads its own output** → FUSE serves content from staging file
4. **Job reads input file** → FUSE looks up hash in `visible_hashes`, reads from CAS blob
5. **Job completes** → FUSE outputs staging paths + metadata in JSON
6. **Wakebox processes outputs**:
   - Hashes each staging file
   - **Stores in CAS** (`.cas/blobs/{prefix}/{suffix}`)
   - **Deletes staging file** (no longer needed)
   - Adds hash to JSON output (no staging_path - file is in CAS)
7. **For remote execution**: rsync transfers **CAS blobs** + JSON to local (not staging files)
8. **Wake processes outputs** (in runner post-processing):
   - **Materializes from CAS** to workspace (reflink)
   - Applies permissions/timestamps
   - Creates symlinks and directories
9. **Other jobs** → Read from CAS by hash, not in-progress workspace writes

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                  Data Flow (Three-Component Split Architecture)              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Job Process       FUSE Daemon          Wakebox               Wake          │
│  ───────────       ───────────          ───────               ────          │
│                                                                              │
│  open("out.txt") ──► create staging                                         │
│                      .cas/staging/1                                          │
│                      track metadata                                          │
│                                                                              │
│  write(data)     ──► write to staging                                       │
│                                                                              │
│  chmod(0755)     ──► track mode: 0755                                       │
│                                                                              │
│  utimens(ts)     ──► track timestamp                                        │
│                                                                              │
│  close()         ──► keep staging file                                      │
│                      (NO hashing!)                                          │
│                                                                              │
│  [job exits]     ──► output JSON:    ──► receive JSON                       │
│                      {                   hash files                          │
│                        "staging_files":  STORE IN CAS  ──► receive JSON     │
│                        {...}             delete staging    materialize      │
│                      }                   output hash       apply meta       │
│                                                                              │
│                                          [REMOTE: rsync  ──►                │
│                                           .cas/blobs/ + JSON]               │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Remote Execution Data Flow

For remote execution (e.g., Slurm runner), the data flow adds an rsync step:

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                         REMOTE EXECUTION FLOW                                 │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  REMOTE MACHINE                                      LOCAL MACHINE            │
│  ──────────────                                      ─────────────            │
│                                                                               │
│  ┌─────────────────────────────────────┐                                      │
│  │ Job → FUSE → staging/               │                                      │
│  │                                     │                                      │
│  │ Wakebox: hash files                 │                                      │
│  │          STORE IN CAS               │  ◄── New: CAS storage in wakebox    │
│  │          delete staging             │                                      │
│  │          output JSON with hash      │                                      │
│  └─────────────────────────────────────┘                                      │
│                    │                                                          │
│                    │ rsync .cas/blobs/{new_hashes} + result.json              │
│                    ▼        ◄── Only new blobs, not staging files!            │
│                                              ┌────────────────────────────────┐│
│                                              │ Wake: parse JSON               ││
│                                              │       materialize from CAS     ││
│                                              │       apply mode + timestamps  ││
│                                              │       (no CAS storage needed)  ││
│                                              └────────────────────────────────┘│
│                                                                               │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Benefits of Wakebox CAS Storage**:
- **Less data transferred**: Only CAS blobs (deduplicated), not staging files
- **Immediate cleanup**: Staging files deleted right after CAS storage
- **Simpler Wake logic**: Just materialize, no CAS storage needed

## CAS-Based Reads

To solve the race condition where concurrent Wake invocations may materialize different content to the same workspace path, jobs read input files directly from CAS by content hash instead of from the workspace.

### The Problem

```
Process A: Job A1 produces build/foo.o (hash H1)
           → stages → CAS stores → materializes to workspace

Process B: Job B1 produces build/foo.o (hash H2)
           → stages → CAS stores → materializes to workspace (OVERWRITES!)

Process A: Job A2 reads build/foo.o
           → Gets H2 content instead of expected H1 ❌
```

### The Solution

Jobs receive both the **path** and **content hash** for each visible input file. When reading, the FUSE daemon looks up the file by hash in CAS instead of reading from the workspace.

```
Wake (Path with Name + Hash)
    │
    ▼
JSON spec: {"visible": [{"path": "build/foo.o", "hash": "abc123..."}], "cas-blobs-dir": ".cas/blobs"}
    │
    ▼
Wakebox parses → visible_file{path, hash}
    │
    ▼
daemon_client.connect() writes to visibles_path
    │
    ▼
FUSE daemon Job::parse() → visible_hashes["build/foo.o"] = "abc123..."
    │
    ▼
wakefuse_open("build/foo.o")
    │
    ├─► Has hash? → open(".cas/blobs/ab/c123...") [CAS-based read]
    │
    └─► No hash? → openat(rootfd, "build/foo.o") [workspace fallback]
```

### Visible Files Serialization

The `Path` tuple in Wake contains both `Name` and `Hash`. When serializing visible files for jobs:

```wake
# Helper to serialize a Path as {path, hash} for CAS-based reads
def mkVisibleJson (p: Path): JValue =
    JObject (
        "path" :-> JString p.getPathName,
        "hash" :-> JString p.getPathHash,
        Nil
    )

# In runner JSON spec:
"visible" :-> visible | map mkVisibleJson | JArray,
"cas-blobs-dir" :-> JString ".cas/blobs",
```

### FUSE Daemon Hash Lookup

```cpp
struct Job {
  std::set<std::string> files_visible;
  std::map<std::string, std::string> visible_hashes;  // path -> hash for CAS-based reads
  // ...
};

// In wakefuse_open():
auto hash_it = it->second.visible_hashes.find(key.second);
if (hash_it != it->second.visible_hashes.end() && !hash_it->second.empty()) {
  std::string blob_path = cas_blob_path(hash_it->second);
  int fd = open(blob_path.c_str(), O_RDONLY);
  if (fd != -1) {
    fi->fh = fd;
    return 0;
  }
  // Fall through to workspace if CAS blob not found
}
```

### Backward Compatibility

The implementation supports both formats:
- **New format**: `{"path": "file.o", "hash": "abc123..."}` - reads from CAS
- **Legacy format**: `"file.o"` (just a string) - reads from workspace

## Design Principles

- **CAS-First Staging**: Outputs are written to staging, NOT directly to workspace
- **CAS-Based Reads**: Inputs are read from CAS by hash, NOT from workspace
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
// Unified structure to track all staged items (files, symlinks, directories)
// Uses a type discriminator field instead of separate structures
struct StagedFile {
  std::string type;          // "file", "symlink", or "directory"
  std::string staging_path;  // Path in .cas/staging/{id} (files only)
  std::string dest_path;     // Final destination (relative to workspace)
  std::string target;        // Symlink target (symlinks only)
  std::string job_id;        // Job that owns this item
  mode_t mode;               // File/directory mode (permissions)
  struct timespec mtime;     // Modification timestamp
  struct timespec atime;     // Access timestamp
  int open_count;            // Reference count for open FDs (files only)
};

// Global CAS blobs directory for hash-based reads
static std::string g_cas_blobs_dir;

// Per-job tracking
struct Job {
  std::set<std::string> files_visible;   // Files job can see
  std::map<std::string, std::string> visible_hashes;  // path -> hash for CAS-based reads
  std::set<std::string> files_read;      // Files job has read
  std::set<std::string> files_wrote;     // Files job has written
  std::set<std::string> staged_paths;    // Paths staged (for is_readable)
  // NOTE: No file_hashes - hashing moved to wakebox
};
```

#### Write Path (Unified Staging)

All output types (files, symlinks, directories) use the same `StagedFile` structure with a type discriminator:

**Files:**
1. **`create()` / `open(O_CREAT)`**: File is created in `.cas/staging/{counter}`, type="file"
2. **`write()`**: Data is written to the staging file
3. **`chmod()`**: Mode is tracked in `StagedFile.mode`
4. **`utimens()`**: Timestamps are tracked in `StagedFile.mtime/atime`
5. **`release()` (close)**:
   - Staging file remains on disk (NOT deleted)
   - Metadata continues to be tracked
   - **No hashing or CAS storage** (deferred to wakebox)

**Symlinks:**
1. **`symlink(target, path)`**: Creates `StagedFile` with type="symlink"
   - No actual symlink created in staging (deferred to post-processing)
   - `target` stored in `StagedFile.target`
   - Path added to `staged_paths` for visibility

**Directories:**
1. **`mkdir(path, mode)`**: Creates `StagedFile` with type="directory"
   - No actual directory created in workspace (deferred to post-processing)
   - Mode stored in `StagedFile.mode`
   - Path added to `staged_paths` for visibility

#### Read Path (CAS-Based and Staging)

When a job reads a file:

**1. Check if file is staged (job's own output):**
- If staged with type="file", read from staging file
- If staged with type="symlink", return symlink target via `readlink()`
- If staged with type="directory", return directory stat info

**2. Check if file has a known hash (CAS-based read):**
```cpp
auto hash_it = it->second.visible_hashes.find(key.second);
if (hash_it != it->second.visible_hashes.end() && !hash_it->second.empty()) {
  std::string blob_path = cas_blob_path(hash_it->second);
  int fd = open(blob_path.c_str(), O_RDONLY);
  if (fd != -1) {
    fi->fh = fd;  // Read from CAS blob
    return 0;
  }
}
```

**3. Fallback to workspace (legacy compatibility):**
```cpp
int fd = openat(context.rootfd, key.second.c_str(), fi->flags, 0);
```

#### Job Completion (Output JSON)

When a job completes, FUSE daemon outputs unified staging metadata with type field:
```json
{
  "inputs": ["src/foo.cpp", "include/bar.h"],
  "outputs": ["build/foo.o", "build/link.txt", "build/subdir"],
  "staging_files": {
    "build/foo.o": {
      "type": "file",
      "staging_path": ".cas/staging/1",
      "mode": 493,
      "mtime_sec": 1234567890,
      "mtime_nsec": 123456789
    },
    "build/link.txt": {
      "type": "symlink",
      "target": "../src/original.txt"
    },
    "build/subdir": {
      "type": "directory",
      "mode": 493
    }
  }
}
```

### Wakebox Client (`src/wakefs/fuse.cpp`)

Wakebox receives the JSON from FUSE and **only hashes files** (no CAS storage or materialization):

#### Processing Staged Files

```cpp
// Process staging files: hash each file and add hash to output JSON
// Wake will handle CAS storage and materialization
static bool process_staging_files(const JAST &staging_files, JAST &staging_files_with_hash) {
  for (const auto &entry : staging_files.children) {
    const std::string &dest_path = entry.first;
    const JAST &file_info = entry.second;

    std::string staging_path = file_info.get("staging_path").value;
    std::string mode_str = file_info.get("mode").value;
    std::string mtime_sec_str = file_info.get("mtime_sec").value;
    std::string mtime_nsec_str = file_info.get("mtime_nsec").value;

    // Only hash - no CAS storage or materialization
    auto hash_result = cas::ContentHash::from_file(staging_path);
    if (!hash_result) {
      std::cerr << "Failed to hash " << staging_path << std::endl;
      continue;
    }

    // Add file info with hash to output JSON
    JAST &file_entry = staging_files_with_hash.add(dest_path, JSON_OBJECT);
    file_entry.add("staging_path", staging_path);
    file_entry.add("mode", mode_str);
    file_entry.add("mtime_sec", mtime_sec_str);
    file_entry.add("mtime_nsec", mtime_nsec_str);
    file_entry.add("hash", hash_result->to_hex());  // Hash included!
  }
  return true;
}
```

#### Wakebox Output JSON (with hash and type)

```json
{
  "inputs": ["src/foo.cpp", "include/bar.h"],
  "outputs": ["build/foo.o", "build/link.txt", "build/subdir"],
  "staging_files": {
    "build/foo.o": {
      "type": "file",
      "staging_path": ".cas/staging/1",
      "mode": 493,
      "mtime_sec": 1234567890,
      "mtime_nsec": 123456789,
      "hash": "abc123def456..."
    },
    "build/link.txt": {
      "type": "symlink",
      "target": "../src/original.txt"
    },
    "build/subdir": {
      "type": "directory",
      "mode": 493
    }
  }
}
```

### Wake CAS Ingestion (`wake/wakebox/runners/rhel8-common.wake`)

Wake's runner post-processing receives the wakebox output JSON and performs CAS storage and materialization using a unified API that handles files, symlinks, and directories:

```wake
# Unified processing - dispatch based on type field
def processStagingItem (destPath: String) (itemInfo: JValue): Result Unit Error =
    require Some itemType = itemInfo // `type` | getJString
    else failWithError "Missing type for {destPath}"

    if itemType ==* "file" then processStagingFile destPath itemInfo
    else if itemType ==* "symlink" then processStagingSymlink destPath itemInfo
    else if itemType ==* "directory" then processStagingDirectory destPath itemInfo
    else failWithError "Unknown staging item type '{itemType}' for {destPath}"

# File: store in CAS, materialize, apply metadata, cleanup staging
def processStagingFile destPath itemInfo =
    require Some stagingPath = itemInfo // `staging_path` | getJString
    require Some hash = itemInfo // `hash` | getJString
    require Some mode = itemInfo // `mode` | getJInteger
    require Some mtimeSec = itemInfo // `mtime_sec` | getJInteger
    require Some mtimeNsec = itemInfo // `mtime_nsec` | getJInteger
    casIngestStagingFile destPath "file" stagingPath hash mode mtimeSec mtimeNsec

# Symlink: create symlink at destPath pointing to target
def processStagingSymlink destPath itemInfo =
    require Some linkTarget = itemInfo // `target` | getJString
    casIngestStagingFile destPath "symlink" linkTarget "" 0 0 0

# Directory: create directory at destPath with mode
def processStagingDirectory destPath itemInfo =
    def mode = itemInfo // `mode` | getJInteger | getOrElse 493  # 0755 octal
    casIngestStagingFile destPath "directory" "" "" mode 0 0
```

### CAS Primitives (`src/runtime/cas_prim.cpp`)

Wake exposes CAS operations as primitives callable from the Wake language:

```cpp
// Unified ingestion primitive - handles files, symlinks, and directories
// prim "cas_ingest_staging_file" destPath itemType stagingPathOrTarget hash mode mtimeSec mtimeNsec
static PRIMFN(prim_cas_ingest_staging_file) {
  // Dispatch based on itemType:
  //
  // type="file":
  //   1. Store file in CAS using provided hash
  //   2. Materialize to workspace with mode (reflink from CAS)
  //   3. Apply timestamps
  //   4. Delete staging file
  //
  // type="symlink":
  //   1. Create parent directories if needed
  //   2. Create symlink at destPath pointing to stagingPathOrTarget
  //
  // type="directory":
  //   1. Create parent directories if needed
  //   2. Create directory at destPath with mode
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

Runtime primitives exposed to the Wake language:

| Primitive | Description |
|-----------|-------------|
| `cas_ingest_staging_file` | Unified: ingest file/symlink/directory from staging |
| `cas_store_file` | Store a file, return content hash |
| `cas_has_blob` | Check if blob exists |
| `cas_materialize_file` | Materialize file from CAS to path |
| `cas_store_path` | Get CAS store path |

### Wake Language API (`share/wake/lib/system/cas.wake`)

```wake
# Ingest a staging item into the workspace (atomic operation).
# Handles files, symlinks, and directories based on the type parameter.
# - type="file": stagingPathOrTarget = staging path, uses hash/mode/mtime
# - type="symlink": stagingPathOrTarget = symlink target
# - type="directory": stagingPathOrTarget = "" (unused), uses mode
export def casIngestStagingFile (destPath: String) (itemType: String)
    (stagingPathOrTarget: String) (hash: String) (mode: Integer)
    (mtimeSec: Integer) (mtimeNsec: Integer): Result Unit Error
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
| `Makefile` | Added `src/cas` to `WAKE_DIRS`, CAS_OBJS for wakebox and wake |
| `src/runtime/cas_prim.h` | CASContext class, CAS primitive declarations |
| `src/runtime/cas_prim.cpp` | CAS primitive implementations (unified ingestion) |
| `src/runtime/prim.h` | Added CASContext forward declaration |
| `src/runtime/prim.cpp` | Added CAS primitive registration |
| `tools/wake/main.cpp` | Initialize CASContext, pass to prim_register_all |
| `tools/fuse-waked/main.cpp` | Staging, metadata tracking, visible_hashes, CAS-based reads |
| `src/wakefs/fuse.cpp` | Hashing only (no CAS storage or materialization) |
| `src/wakefs/fuse.h` | visible_file struct, cas_blobs_dir field |
| `src/wakefs/daemon_client.cpp` | Pass visible files with hashes to FUSE daemon |
| `share/wake/lib/system/cas.wake` | Unified casIngestStagingFile API |
| `wake/wakebox/runners/rhel8-common.wake` | Type-based staging dispatch |
| `wake/wakebox/runners/rhel8-bindmount-runner.wake` | Visible files serialized with hashes |
| `wake/wakebox/runners/squashfs-runner.wake` | Visible files serialized with hashes |
| `share/wake/lib/system/runner.wake` | Visible files serialized with hashes |

### CAS Core Files

| File | Purpose |
|------|---------|
| `src/cas/cas.h` | ContentHash class (simplified, no Tree) |
| `src/cas/cas.cpp` | Implementation of content hashing |
| `src/cas/cas_store.h` | CASStore class interface (blob-only) |
| `src/cas/cas_store.cpp` | CASStore implementation |
| `src/cas/file_ops.h` | File operation utilities interface |
| `src/cas/file_ops.cpp` | Reflink/hardlink/copy implementation |

## Current Behavior (Three-Component Split Architecture)

### Write Path (Outputs)
1. **On file create/open**: FUSE creates file in `.cas/staging/` with type="file"
2. **On file write**: Data written directly to staging file
3. **On symlink**: FUSE tracks symlink with type="symlink" (deferred creation)
4. **On mkdir**: FUSE tracks directory with type="directory" (deferred creation)
5. **On chmod/utimens**: Metadata tracked in `StagedFile` struct
6. **On file close**: Staging file remains; metadata preserved
7. **During job execution**: Job sees its outputs via FUSE (reads from staging)
8. **On job exit**: FUSE outputs unified JSON with staging paths, types, and metadata

### Read Path (Inputs)
1. **Job reads staged file**: FUSE serves from staging (job's own output)
2. **Job reads visible file with hash**: FUSE reads from CAS blob by hash
3. **Job reads visible file without hash**: FUSE reads from workspace (legacy)

### Post-Processing
1. **Wakebox processes** (runs locally or remotely):
   - Hashes each staging file (type="file" only)
   - Adds hash to JSON output
   - Does NOT store in CAS or materialize
2. **For remote execution**: rsync transfers staging files + JSON to local
3. **Wake processes** (in runner post-processing):
   - Dispatches based on type field
   - Files: stores in CAS, materializes with reflink, applies metadata, cleans up
   - Symlinks: creates symlink at destPath
   - Directories: creates directory with mode

### Benefits
1. **Deduplication**: Identical files share storage in CAS
2. **Cache recovery**: On cache hit with missing files, materialize from CAS
3. **Concurrent builds**: No file conflicts - each job writes to isolated staging
4. **Content correctness**: Jobs read exact content by hash, not stale workspace

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

## Wakebox Integration (Hashing Only)

Wakebox receives FUSE output and **only hashes files** (CAS storage and materialization happen in Wake):

### Wakebox Processing (`src/wakefs/fuse.cpp`)

```cpp
// After FUSE daemon exits, hash staged files and add hash to output JSON
// Wake will handle CAS storage and materialization
static bool process_staging_files(const JAST &staging_files, JAST &staging_files_with_hash) {
  for (const auto &entry : staging_files.children) {
    const std::string &dest_path = entry.first;
    const JAST &file_info = entry.second;

    std::string staging_path = file_info.get("staging_path").value;
    // ... other metadata parsing ...

    // Only hash - Wake will handle CAS storage and materialization
    auto hash_result = cas::ContentHash::from_file(staging_path);

    // Add file info WITH HASH to output JSON
    JAST &file_entry = staging_files_with_hash.add(dest_path, JSON_OBJECT);
    file_entry.add("staging_path", staging_path);
    // ... other metadata ...
    file_entry.add("hash", hash_result->to_hex());  // Hash included!
  }
  return true;
}
```

### Wake CAS Ingestion (`share/wake/lib/system/cas.wake`)

```wake
# Ingest a staging item into the workspace (atomic operation).
# Handles files, symlinks, and directories based on the type parameter.
# - type="file": stagingPathOrTarget = staging path, uses hash/mode/mtime
# - type="symlink": stagingPathOrTarget = symlink target
# - type="directory": stagingPathOrTarget = "" (unused), uses mode
export def casIngestStagingFile (destPath: String) (itemType: String)
    (stagingPathOrTarget: String) (hash: String) (mode: Integer)
    (mtimeSec: Integer) (mtimeNsec: Integer): Result Unit Error =
  prim "cas_ingest_staging_file"
```

### Runner Post-Processing (`wake/wakebox/runners/rhel8-common.wake`)

```wake
# In commonWakeboxPost, process staging files from wakebox output:
# Dispatch based on type field for files, symlinks, and directories
def processStagingItem destPath itemInfo =
    require Some itemType = itemInfo // `type` | getJString

    if itemType ==* "file" then
        casIngestStagingFile destPath "file" stagingPath hash mode mtimeSec mtimeNsec
    else if itemType ==* "symlink" then
        casIngestStagingFile destPath "symlink" linkTarget "" 0 0 0
    else if itemType ==* "directory" then
        casIngestStagingFile destPath "directory" "" "" mode 0 0
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
  t3: B starts, visible_hashes["foo.o"] = H1
  t4: B reads foo.o → FUSE opens .cas/blobs/{H1} (CAS-based read)
  t5: FUSE: B writes foo.exe to staging
  t6: B exits → wakebox B: hash, store, materialize foo.exe

Result: B reads from CAS by hash, not workspace
        Even if workspace is overwritten, B reads correct content
```

### Scenario 4: Concurrent Builds with Same Input (CAS-Based Reads)

```
Wake 1: Job A1 produces build/foo.o (hash H1)
        → stages → CAS stores → materializes to workspace

Wake 2: Job B1 produces build/foo.o (hash H2)
        → stages → CAS stores → materializes to workspace (OVERWRITES!)

Wake 1: Job A2 reads build/foo.o (visible_hashes["build/foo.o"] = H1)

Timeline:
  t1: A1 materializes foo.o with hash H1
  t2: B1 materializes foo.o with hash H2 (overwrites workspace!)
  t3: A2 starts, visible_hashes["foo.o"] = H1
  t4: A2 reads foo.o → FUSE opens .cas/blobs/{H1} ← Correct hash!

Result: A2 reads H1 content (from CAS), not H2 (from workspace)
        CAS-based reads solve the race condition
```

## RSC (Remote Source Cache) Compatibility

RSC is a distributed caching system that stores job outputs on a remote server. This section analyzes its compatibility with the CAS architecture.

### Current RSC Behavior

RSC has two paths:

**Cache Miss (Upload)**:
1. Job runs locally via `baseDoIt` (uses CAS architecture normally)
2. Outputs are stored in CAS, materialized to workspace
3. RSC uploads output files from workspace to RSC server via `rscApiPostFileBlob`
4. RSC uploads stdout/stderr as blobs
5. RSC posts job metadata with blob IDs

**Cache Hit (Download/Rehydrate)**:
1. RSC downloads blobs from server via `rscApiGetFileBlob`
2. **Writes files directly to workspace** (using hardlink + atomic mv)
3. Creates symlinks and directories directly in workspace
4. Calls `primJobVirtual` to mark job as complete

### Compatibility Issues

| Aspect | Current RSC Behavior | CAS Architecture Expectation | Compatible? |
|--------|---------------------|------------------------------|-------------|
| **Upload (cache miss)** | Reads from workspace after job completes | Files are in workspace (materialized from CAS) | ✅ Yes |
| **Download (cache hit)** | Writes directly to workspace | Should go through CAS staging first | ❌ **Bypasses CAS** |
| **Hash tracking** | RSC has its own `content_hash` in blobs | CAS uses BLAKE2b hash in Path tuple | ⚠️ Duplicate hashing |
| **Concurrent builds** | Downloads to workspace directly | CAS-based reads expect files in `.cas/blobs/` | ❌ **Race condition not solved** |

### The Core Problem

When RSC rehydrates a cached job (cache hit), it downloads directly to the workspace:

```wake
# remote_cache_runner.wake:148
def doDownload (CacheSearchOutputFile path mode blob) = rscApiGetFileBlob blob path mode
```

And `rscApiGetFileBlob` writes directly to the workspace:

```wake
# remote_cache_api.wake:696-710
def fixupScript =
    """
    ...
    cp -l %{blobPath.getPathName} '%{path}.rsctmp'
    chmod %{mode | strOctal} '%{path}.rsctmp'
    mv '%{path}.rsctmp' '%{path}'   # ← Direct write to workspace!
    """
```

This means:

1. **No CAS storage**: The file isn't stored in `.cas/blobs/`
2. **No hash in Path**: The returned `Path` won't have the correct hash for CAS-based reads
3. **Race condition persists**: If two Wake processes both get cache hits for different versions of the same file, they'll overwrite each other in the workspace

### What `avoid-unproductive-localRunner` Branch Does (And Doesn't Do)

The `avoid-unproductive-localRunner` branch changes some operations to use `fuseRunner` instead of `localRunner`:

| Change | File | Purpose |
|--------|------|---------|
| `makeRequest` → `fuseRunner` | `http.wake` | Avoid cyclic value errors in defaultRunner |
| `makeBinaryRequest` → `fuseRunner` | `http.wake` | Same - used for RSC blob fetches |
| `generateID` → `fuseRunner` | `remote_cache_api.wake` | Avoid loop through makeRemoteCacheAPI |
| `mkdir`, `symlink`, `readlink` → still `localRunner` | `remote_cache_runner.wake` | Need full workspace visibility |

**This does NOT solve CAS compatibility.** The changes are about fixing runner dependency cycles, not CAS integration. RSC cache hits still:
- Bypass the CAS store
- Write directly to workspace
- Not provide hashes for CAS-based reads
- Remain vulnerable to race conditions in concurrent builds

### Changes Needed for Full Compatibility

To make RSC fully CAS-compatible, the rehydration path needs to:

1. **Download to staging**: Instead of downloading to `{path}.rsctmp`, download to `.cas/staging/{id}`

2. **Store in CAS**: Call `casIngestStagingFile` to:
   - Store the blob in `.cas/blobs/{prefix}/{suffix}`
   - Materialize to workspace via reflink
   - Return a proper `Path` with the hash

3. **Use existing content hash**: RSC already has `content_hash` in its blob metadata - this could be used directly instead of re-hashing

Example of what the change would look like:

```wake
# Instead of direct download in remote_cache_runner.wake:
def doDownload (CacheSearchOutputFile path mode (CacheSearchBlob _ uri contentHash)) =
    # 1. Download to temp/staging
    require Pass tempPath = downloadToStaging uri

    # 2. Ingest through CAS (store + materialize)
    # Use RSC's content_hash directly (no re-hashing needed)
    require Pass _ = casIngestStagingFile path "file" tempPath contentHash mode 0 0

    Pass path
```

### Current State: Partial Compatibility

RSC is **partially compatible** with the CAS architecture:

- ✅ **Cache miss path works**: Jobs run through CAS, outputs are stored in CAS, then RSC uploads from workspace
- ❌ **Cache hit path bypasses CAS**: Downloads go directly to workspace, missing CAS benefits

This means:
- Single-process builds with RSC cache hits work fine
- **Multi-process concurrent builds with RSC cache hits could still have race conditions**

## Future Work

1. **Remote CAS**: Support remote content-addressable storage backends (S3, NFS)
2. **Garbage Collection**: Clean up unreferenced CAS blobs
3. **Hash-based Job Caching**: Use content hashes for finer-grained cache invalidation
4. **Shared CAS Across Workspaces**: Enable cross-workspace content sharing
5. **CAS Verification**: Verify file integrity using stored hashes
6. **RSC Integration**: Modify RSC rehydration to go through CAS for full concurrent build safety

## Design Decisions

### Why Three-Component Split?

**Problem**: Remote execution (Slurm runner) doesn't have access to local CAS store.

**Solution**: Split responsibilities so that:
1. FUSE daemon: lightweight, runs on any machine
2. Wakebox: hashes files, runs on any machine (local or remote)
3. Wake: CAS storage/materialization, runs only on local machine

This ensures the same data flow for both local and remote execution.

### Why Hash in Wakebox?

1. **Parallelism**: Each wakebox hashes its own job's outputs concurrently
2. **Efficiency**: Hash is computed once, not re-computed by Wake
3. **Remote-friendly**: Hashing works on remote machines without CAS access

### Why CAS Storage in Wake?

1. **Local Access**: Wake runs on the machine with the CAS store
2. **Consistency**: Single point of CAS management
3. **Cache Integration**: Wake manages job cache which references CAS blobs

### Why Not Store in CAS on Remote Machine?

1. Remote machine's CAS would be discarded after job completes
2. Rsync would need to transfer entire CAS (wasteful)
3. CAS blobs need to be on the machine running Wake for cache hits

### Why Unified Type-Based Staging?

Previously, files and symlinks used separate tracking structures (`g_staged_files` for files, `g_staged_symlinks` for symlinks) and separate JSON fields. This caused:

1. **Code duplication**: Similar logic for handling different output types
2. **Inconsistent JSON format**: Different structures for different types
3. **Complex post-processing**: Multiple code paths in runners

The unified approach uses a single `StagedFile` struct with a `type` discriminator ("file", "symlink", "directory"):

1. **Simpler code**: One structure, one JSON format, one code path
2. **Extensible**: Easy to add new types in the future
3. **Consistent API**: `casIngestStagingFile` handles all types with dispatch on `itemType`

### Why CAS-Based Reads?

When multiple Wake invocations run concurrently, they may materialize different content to the same workspace path. Without CAS-based reads:

1. **Race condition**: Job A2 reads workspace after Job B1 overwrites → wrong content
2. **Non-determinism**: Same job can read different content depending on timing
3. **Silent failures**: No error, but build produces incorrect results

With CAS-based reads:

1. **Content correctness**: Job reads by hash, always gets expected content
2. **Determinism**: Same inputs → same outputs regardless of concurrent builds
3. **Workspace as cache**: Workspace is just a convenience; CAS is source of truth

The implementation passes both path and hash for visible files:
```json
{"visible": [{"path": "build/foo.o", "hash": "abc123..."}]}
```

FUSE daemon stores hashes in `visible_hashes` map and reads from CAS when available.
