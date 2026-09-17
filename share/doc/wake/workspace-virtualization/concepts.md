# Key Concepts

_Part of [Workspace Virtualization and Multiple Wake Invocations](../workspace-virtualization-and-multi-wake.md)._

## Workspace virtualization

Sandboxed runners (that utilize FUSE) do not write directly through to your
workspace. Instead:

1. While a job runs, the data it writes lands in a staging directory under
   `.build/cas/staging/`.
2. When the job completes, its outputs are ingested into the CAS.
3. Wake materializes those outputs into their destination paths using
   **reflinks** (copy-on-write).

Materializing through reflinks means identical content is stored only once on
disk, and runs do not interfere with one another. This also means that changes that a user makes to the workspace don't propogate back to invalidate the CAS.

## Content-addressed storage (CAS)

All job outputs are stored by content hash under `.build/cas/blobs/`. Wake materializes outputs into your workspace *from* the
CAS rather than writing them directly. Because content is keyed by its hash,
identical files across jobs, targets, or runs share a single underlying blob.

> **The CAS is owned by wake.** Files under `.build/cas/` must never be modified
> by hand. Doing so results in undefined behavior — wake assumes it is the sole
> owner of everything in this space.

## The workspace as a projection of wake state

With workspace virtualization, the database and CAS are the source of truth, and
the workspace is a materialization of your jobs' outputs.

A direct consequence: **deleting your build directory does not reclaim the
underlying disk footprint**, because the outputs still live in the CAS. If you
delete materialized outputs and later re-run that job, wake can re-materialize
the outputs from the CAS without re-executing the work.

This also affects a debugging technique sometimes utilized — editing a file in the workspace
and re-running a `wakebox` spec. Wakebox refers to the CAS, not the workspace,
so edits made directly in the workspace are not picked up.

---

Next: [Running Multiple Wake Invocations Concurrently](concurrent-invocations.md)
