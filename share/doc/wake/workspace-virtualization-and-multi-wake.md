# Workspace Virtualization and Multiple Wake Invocations

This document describes two closely related capabilities in wake:

- **Workspace virtualization** through a Content-Addressable Storage (CAS)
  system, and
- Support for **multiple concurrent wake invocations** in a single workspace.

Both are built on the same foundation: wake stores everything jobs produce in a
content-addressed store and treats your workspace as a *projection* of that
state, rather than as the source of truth. This shift is what
concurrent builds possible, naturally deduplicates storage, and lays the
groundwork for richer cache management.

## Overview

Traditionally, wake tracked — but did not own — the build outputs produced by
jobs. The build directory was primarily yours; wake simply recorded what it
last saw and used that to decide what to run next. The workspace was the source
of truth.

With workspace virtualization, the source of truth moves into wake's own state:
the database and, especially, the CAS. Your workspace becomes a materialization
of the outputs of your runs' jobs.

Because outputs are content-addressed and shared safely, multiple wake
processes can run at the same time in the same workspace, reusing completed
jobs from one another while preserving all of wake's existing correctness and
safety guarantees.

Some things this makes possible:

- Run wake interactively — lint, format-check, test a variant — while one or
  more long builds are already in progress.
- Use `bsp-wake` / Scala Metals without waiting for other wake activity to
  finish.
- Kick off multiple independent builds (different configs, different targets) in
  the same workspace without maintaining separate checkouts.

## Key concepts

### Workspace virtualization

Sandboxed runners (the default) no longer write directly through to your
workspace. Instead:

1. While a job runs, the data it writes lands in a staging directory under
   `.build/cas/staging/`.
2. When the job completes, its outputs are ingested into the CAS.
3. Wake materializes those outputs into their destination paths using
   **reflinks** (copy-on-write) rather than writing them directly.

Materializing through reflinks means identical content is stored only once on
disk, and runs do not interfere with one another.

### Content-addressed storage (CAS)

All job outputs are stored by content hash under `.build/cas/blobs/`, similar to
the git object store. Wake materializes outputs into your workspace *from* the
CAS rather than writing them directly. Because content is keyed by its hash,
identical files across jobs, targets, or runs share a single underlying blob.

> **The CAS is owned by wake.** Files under `.build/cas/` must never be modified
> by hand. Doing so results in undefined behavior — wake assumes it is the sole
> owner of everything in this space.

### The workspace as a projection of wake state

With workspace virtualization, the database and CAS are the source of truth, and
the workspace is a materialization of your jobs' outputs.

A direct consequence: **deleting your build directory does not reclaim the
underlying disk footprint**, because the outputs still live in the CAS. If you
delete materialized outputs and later re-run that job, wake can re-materialize
the outputs from the CAS without re-executing the work.

This is also why a common debugging technique — editing a file in the workspace
and re-running a `wakebox` spec — no longer behaves as it used to. Wakebox
refers to the CAS, not the workspace.

## Running multiple wake invocations concurrently

Multiple wake processes may run concurrently in the same workspace. They
coordinate through the shared workspace database and CAS, and a run will reuse
jobs that other runs have already completed.

For non-concurrent use, behavior is designed to be identical to a single wake
invocation. Concurrency simply allows additional processes to participate in,
and benefit from, the same shared state.

### Observing active runs

Several command-line options let you inspect what is happening across all active
builds:

| Option        | Description                                                          |
| ------------- | -------------------------------------------------------------------- |
| `--history`   | Report the command-line history of all wake commands, including completed and in-flight runs. |
| `--ps`        | Show jobs currently running in active wake builds.                   |
| `--active`    | Capture jobs currently running in active builds.                     |
| `--queued`    | Capture jobs queued but not yet launched in active builds.           |
| `--in-flight` | Capture jobs running or queued in active builds.                     |

## Managing workspace disk usage with CAS

Because the CAS is the source of truth, managing disk usage is about managing
the CAS and database — not about deleting files in your workspace.

### Why the build directory is not where space lives

Reflinked, content-addressed storage means:

- Identical outputs are stored once and shared, so the CAS is typically far
  smaller than the sum of all materialized copies would suggest.
- Removing materialized files (for example, `rm -rf` of a build directory) frees
  only the lightweight reflinked references, **not** the underlying blobs in the
  CAS. The footprint persists until the corresponding entries are removed from
  wake's state.

To actually reclaim space, you must remove entries from the database, which in
turn allows their backing CAS blobs to be collected.

### Removing entries for files that no longer exist: `--prune`

```
wake --prune
```

`--prune` removes database entries for jobs whose output files no longer exist
in the workspace, then garbage-collects any CAS blobs that are no longer
referenced. It reports how many jobs were pruned.

`--prune` first reaps dead runs and will refuse to run while builds are in
progress:

```
error: cannot prune while builds are in progress
hint: use 'wake --history' to see active runs
```

Use `wake --history` to confirm no runs are active before pruning.

### Safely deleting specific files: `--rm`

```
wake --rm <path> [<path> ...]
```

`--rm` removes the given files from the database and deletes their backing CAS
blobs. It is **safe with respect to shared content**: a CAS blob is only deleted
when *every* path that references it is included in the removal. If any other
path still references the same content, the blob is preserved.

`--rm` performs its work inside an exclusive transaction, so it cannot race with
concurrent builds that might still be using the files being deleted. Paths may
be given relative to the current directory, including from a subdirectory of the
workspace.

### Cleaning up staging leftovers

`.build/cas/staging/` can accumulate leftover files after a crash. If it grows
large, it is safe to delete its contents **while no wake processes are
running**.

## Known limitations and good to know

- **Redundant work across simultaneous runs.** If two invocations start at the
  same time, work that neither has cached yet may be performed by both, since
  only *completed* jobs are considered for reuse. A simple mitigation is to let
  one run progress far enough to populate the cache before fanning out into
  parallel invocations. Correctness is unaffected; only resource usage is.

- **Schema versioning.** The database schema may change between releases and may
  not always migrate forward automatically. After an upgrade you may need to run
  `wake --clean` to start fresh. Release notes will call this out when relevant.

- **CAS file ownership.** Blobs are owned by the user and group running wake.
  Some workflows switch the effective user/group for specific jobs; those
  changes apply within the job, but the content written to the CAS is owned by
  the user/group that launched wake. If specific at-rest ownership must be
  preserved, set it in your shell before launching wake.

- **Never hand-edit the CAS.** As noted above, any modification to files under
  `.build/cas/` results in undefined behavior. This space is owned entirely by
  wake.