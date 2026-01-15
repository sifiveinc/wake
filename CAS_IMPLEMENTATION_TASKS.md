# CAS Implementation Tasks

## Overview

Implementing Content-Addressable Storage (CAS) in Wake with **CAS-first staging** architecture. Build outputs are written to a staging area, immediately stored in CAS when closed, and only materialized to the workspace at job completion. This enables:

- **Content Deduplication**: Identical files share storage
- **Concurrent Build Isolation**: Multiple Wake invocations can run without file conflicts
- **Cache Recovery**: Missing outputs can be materialized from CAS on cache hit

## Why CAS-First Staging for Concurrent Builds

Traditional build systems have file conflict problems with concurrent builds:
- Job A reads a file while Job B is writing to it (read-write conflict)
- Both jobs write to the same output file (write-write conflict)
- Job A sees a partially-written file from Job B (partial write visibility)

**CAS-first staging solves this** by:
1. Writing job outputs to isolated staging directory (`.cas/staging/`)
2. Storing content in CAS immediately when files are closed
3. Jobs see their own outputs via FUSE (reads from CAS)
4. Only materializing to workspace when job completes successfully

This means:
- No partial writes visible to other jobs
- No file conflicts between concurrent builds
- Atomic job completion (all outputs appear at once)
- Content deduplication across all builds

## Design

### Key Features
- **CAS-First Staging**: Outputs written to staging, stored in CAS on close, materialized at job end
- **No Merkle Trees**: Store individual files by hash only, no directory-level hashing
- **CAS in FUSE Daemon**: The CAS staging is implemented in fuse-waked for isolation
- **Reflink Storage**: Files are stored using copy-on-write reflinks when supported
- **Mode Preservation**: File permissions tracked and applied during materialization

## Tasks

### Phase 1: Core Data Structures (COMPLETE)
- [x] Create `src/cas/cas.h` - ContentHash class only (simplified, no Tree/TreeEntry)
- [x] Create `src/cas/cas.cpp` - Implementation of content hashing
- [x] Remove Tree/TreeEntry from cas.h
- [x] Create `src/cas/cas_store.h` - CAS store class header (blob-only)
- [x] Create `src/cas/cas_store.cpp` - CAS store implementation
- [x] Remove tree operations from CASStore

### Phase 2: File Operations (COMPLETE)
- [x] Create `src/cas/file_ops.h` - File operation utilities header
- [x] Create `src/cas/file_ops.cpp` - Reflink/copy with fallback (hardlinks disabled for CAS)

### Phase 3: Job Integration (COMPLETE)
- [x] Integrate CAS store into JobTable (`src/runtime/job.cpp`)
- [x] Store outputs in CAS on job finish (`prim_job_finish`)
- [x] Materialize missing outputs from CAS on cache hit (`prim_job_cache`)
- [x] Pass CASStore to JobTable in main.cpp

### Phase 4: Runtime Integration (COMPLETE)
- [x] Create `src/runtime/cas_prim.h` - CASContext class
- [x] Create `src/runtime/cas_prim.cpp` - CAS primitives implementation
- [x] Update `tools/wake/main.cpp` - Initialize CASContext
- [x] Update `Makefile` - Add src/cas to WAKE_DIRS

### Phase 5: FUSE Integration (COMPLETE)
- [x] Integrate CAS store into fuse-waked daemon
- [x] Store outputs in CAS when job completes (in `Job::dump()`)
- [x] Add `output_hashes` field to JSON output with content hashes
- [x] Add `casLib` dependency to fuse-waked.wake build

### Phase 6: Wake API (COMPLETE)
- [x] Create `share/wake/lib/system/cas.wake` - Wake CAS API
- [x] Simplified API - removed tree/directory functions
- [x] Final API: `casStoreFile`, `casHasBlob`, `casMaterializeFile`, `casStorePath`

### Phase 7: CAS-First Staging (COMPLETE)
- [x] Add StagedFile struct for tracking in-progress writes
- [x] Add g_staged_files and g_fd_to_staged maps for staging tracking
- [x] Modify wakefuse_create() to write to staging directory
- [x] Modify wakefuse_open() to redirect writes to staging
- [x] Modify wakefuse_release() to hash and store in CAS on close
- [x] Add file_hashes map to Job struct for tracking content hashes
- [x] Add file_modes map to Job struct for tracking file permissions
- [x] Implement materialize_outputs_from_cas() for job completion
- [x] Handle chmod after close (update file_modes)
- [x] Handle rename of CAS-staged files (move hash to new name)
- [x] Handle unlink of CAS-staged files (remove from tracking)
- [x] Disable hardlinks in copy_file() for correct mode handling
- [x] Add output_modes to JSON output for mode preservation

## File Structure

```
workspace/
├── .cas/                    # CAS store root
│   ├── blobs/               # Content-addressed file storage
│   │   ├── a1/              # First 2 hex chars (sharding)
│   │   │   └── b2c3d4...    # Content blob
│   │   └── ...
│   └── staging/             # Temporary staging for in-progress writes
│       └── {counter}        # Unique staging files (cleaned up on close)
├── .fuse/                   # FUSE mount point (for job isolation)
├── wake.db                  # Wake database
└── <outputs>                # Build outputs (materialized from CAS at job end)

src/cas/
├── cas.h              # Core types: ContentHash only (simplified)
├── cas.cpp            # Implementation of content hashing
├── cas_store.h        # CASStore class for blob storage (no trees)
├── cas_store.cpp      # CASStore implementation
├── cas_job_cache.h    # CAS job cache utilities header
├── cas_job_cache.cpp  # CAS job cache utilities implementation
├── file_ops.h         # File copy utilities (reflink, copy - no hardlinks)
├── file_ops.cpp       # File operations implementation
└── cas.wake           # Wake build configuration

src/runtime/
├── cas_prim.h         # CASContext class and primitive declarations
└── cas_prim.cpp       # CAS primitive implementations

tools/fuse-waked/
└── main.cpp           # FUSE daemon with CAS-first staging
```

## Current Progress

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 1: Core Types | COMPLETE | ContentHash only, Tree/TreeEntry removed |
| Phase 2: File Ops | COMPLETE | Reflink/copy working (hardlinks disabled for mode handling) |
| Phase 3: Job Integration | COMPLETE | Store outputs in CAS, materialize on cache hit |
| Phase 4: Runtime Integration | COMPLETE | CASContext, primitives, main.cpp |
| Phase 5: FUSE Integration | COMPLETE | fuse-waked stores outputs in CAS, provides hashes in JSON |
| Phase 6: Wake API | COMPLETE | Blob-only API, tree functions removed |
| Phase 7: CAS-First Staging | COMPLETE | Staging, deferred materialization, mode handling |

## Current Components

### Core CAS
- `ContentHash` - BLAKE2b 256-bit content hashing
- `CASStore` - Content-addressable blob storage (blob-only)
- File operations with reflink/copy fallback (no hardlinks)

### CAS Job Cache
- `store_output_file()` - Store a single file in CAS
- `store_output_files()` - Store multiple files with modes
- `materialize_file()` - Materialize file from CAS to path with mode

### CAS Primitives
- `prim "cas_store_file"` - Store a file in CAS, returns content hash
- `prim "cas_has_blob"` - Check if blob exists in CAS
- `prim "cas_materialize_file"` - Materialize file from CAS to filesystem
- `prim "cas_store_path"` - Get CAS store path

### FUSE Daemon (CAS-First Staging)
- `StagedFile` struct - Track files being written to staging
- `g_staged_files` map - Map (job_id, dest_path) → StagedFile
- `g_fd_to_staged` map - Map file descriptor → StagedFile
- `file_hashes` in Job - Map path → content hash (hex)
- `file_modes` in Job - Map path → file mode
- `materialize_outputs_from_cas()` - Materialize all outputs at job end

## Output Flow (CAS-First Staging)

```
Job Execution:
  1. create()/open(O_CREAT): File created in .cas/staging/{counter}
  2. write(): Data written to staging file
  3. release():
     a. Hash staging file content (BLAKE2b)
     b. Store in .cas/blobs/{prefix}/{suffix}
     c. Record hash in job.file_hashes[path]
     d. Record mode in job.file_modes[path]
     e. Delete staging file
  4. Job reads its own output: FUSE serves from CAS blob

Job Completion:
  5. materialize_outputs_from_cas():
     a. For each (path, hash) in file_hashes:
        - Create parent directories
        - Materialize from CAS to workspace path
        - Apply mode from file_modes

Cache Hit (prim_job_cache):
  6. Check if outputs exist at their paths
  7. If missing but hash known:
     a. Verify blob exists in CAS
     b. Materialize: CAS blob → output path with correct mode
  8. Job reused if all outputs available
```

## JSON Output Format

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

## Completed

1. ✓ **Core CAS types** - ContentHash, CASStore (blob-only)
2. ✓ **File operations** - Reflink/copy with correct mode handling
3. ✓ **Job integration** - Store outputs on finish, materialize on cache hit
4. ✓ **Wake API** - Blob-only primitives and Wake functions
5. ✓ **CAS-first staging** - Isolated writes, deferred materialization
6. ✓ **Mode preservation** - Track and apply file permissions
7. ✓ **Special case handling** - chmod/rename/unlink of staged files

## Future Work

1. **Garbage Collection** - Clean up unreferenced CAS blobs
2. **Remote CAS** - Support remote content-addressable storage backends
3. **Parallel Materialization** - Materialize multiple outputs concurrently
4. **Shared CAS Across Workspaces** - Enable cross-workspace content sharing
5. **Job-Level Cache Keys** - Use content hashes for finer-grained cache invalidation
