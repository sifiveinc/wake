# Managing Workspace Disk Usage with CAS

_Part of [Workspace Virtualization and Multiple Wake Invocations](../workspace-virtualization-and-multi-wake.md)._

Because the CAS is the source of truth, managing disk usage is about managing
the CAS and database — not about deleting files in your workspace.

## Why the build directory is not where space lives

Reflinked, content-addressed storage means:

- Identical outputs are stored once and shared, so the CAS is typically far
  smaller than the sum of all materialized copies would suggest.
- Removing materialized files (for example, `rm -rf` of a build directory) frees
  only the lightweight reflinked references, *not* the underlying blobs in the
  CAS. The footprint persists until the corresponding entries are removed from
  wake's state.

To actually reclaim space, you must remove entries from the database, which in
turn allows their backing CAS blobs to be collected.

## Removing entries for files that no longer exist: `--prune`

```
wake --prune
```

`--prune` removes database entries for jobs whose output files no longer exist
in the workspace, then garbage-collects any CAS blobs that are no longer
referenced. It reports how many jobs were pruned.

This makes `--prune` a convenient way to resync the CAS with the current state
of your workspace after deleting outputs directly. For example:

```
rm -rf build/test
wake --prune
```

Wake detects that the outputs under `build/test` are gone, drops the
corresponding job entries from the database, and garbage-collects the CAS
blobs that are no longer referenced.

`--prune` first reaps dead runs and will refuse to run while builds are in
progress:

```
error: cannot prune while builds are in progress
hint: use 'wake --history' to see active runs
```

Use `wake --history` to confirm no runs are active before pruning.

## Safely deleting specific files/directories: `--rm`

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

By default, directories are skipped with a warning. To remove directories and
all of their contents, pass `-r`/`--recursive`:

```
wake --rm -r <path> [<path> ...]
```

With `--recursive`, every file beneath the given directories is removed at
arbitrary nesting depth, and the now-empty directories are removed deepest-first.
The same shared-content safety applies: a CAS blob is only deleted when every
path referencing it is part of the removal.

For example, to conveniently remove a whole build subtree from both the workspace and the
CAS in one step:

```
wake --rm -r build/test
```

Wake deletes every tracked file under `build/test`, removes their database
entries, drops the CAS blobs no longer referenced by any remaining path, and
removes the emptied directories.

## Cleaning up staging leftovers

`.build/cas/staging/` can accumulate leftover files after a crash. If it grows
large, it is safe to delete its contents **while no wake processes are
running**.

---

Next: [Known Limitations and Good to Know](limitations.md)
