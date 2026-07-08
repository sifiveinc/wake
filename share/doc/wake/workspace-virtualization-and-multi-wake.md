# Workspace Virtualization and Multiple Wake Invocations

This document describes two closely related capabilities in wake:

- **Workspace virtualization** through a Content-Addressable Storage (CAS)
  system, and
- Support for **multiple concurrent wake invocations** in a single workspace.

Both are built on the same foundation: wake stores everything jobs produce in a
content-addressed store and treats your workspace as a *projection* of that
state, rather than as the source of truth. This shift is what
concurrent builds possible, naturally deduplicates storage, and lays the
groundwork for richer workspace management.

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

- Run wake interactively — lint, format-check, tests — while one or
  more long builds are already in progress.
- Run IDE or editor integrations (such as language servers)
  without waiting for other wake activity to finish (e.g. Scala Metals).
- Kick off multiple independent builds in the same workspace without maintaining separate checkouts.

## Contents

- [Key Concepts](workspace-virtualization/concepts.md) — workspace
  virtualization, content-addressed storage (CAS), and the workspace as a
  projection of wake state.
- [Running Multiple Wake Invocations Concurrently](workspace-virtualization/concurrent-invocations.md)
  — how concurrent runs coordinate, and options for observing active runs.
- [Managing Workspace Disk Usage with CAS](workspace-virtualization/managing-disk-usage.md)
  — reclaiming space with `--prune`, `--rm`, and staging cleanup.
- [Known Limitations and Good to Know](workspace-virtualization/limitations.md).
