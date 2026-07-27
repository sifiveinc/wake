# Known Limitations and Good to Know

_Part of [Workspace Virtualization and Multiple Wake Invocations](../workspace-virtualization-and-multi-wake.md)._

- **Redundant work across runs.** If two invocations start at the
  same time, work that neither has cached yet may be performed by both, since
  only *completed* jobs are considered for reuse. A simple mitigation is to let
  one run progress far enough to populate the cache before fanning out into
  parallel invocations. Correctness is unaffected; only resource usage is.

- **CAS file ownership.** Blobs are owned by the user and group running wake.
  Workflows that switch the effective user/group for specific jobs will see
  those changes apply within the job, but the content written to the CAS is
  owned by the user/group that launched wake. If specific at-rest ownership must
  be preserved, set it in your shell before launching wake.

- **Never hand-edit the CAS.** As noted above, any modification to files under
  `.build/cas/` results in undefined behavior. This space is owned entirely by
  wake.

- **Disk usage tools over-report reflinked space.** Tools like `du` do not
  account for deduplicated reflinks, so they may report inflated usage (e.g.
  roughly 2x) for content shared between the workspace and the CAS. We are
  investigating how to address this.

- **Inspecting a running job's in-progress outputs.** A job's outputs aren't
  materialized into the workspace at their real paths until it *completes*.
  `wake --job JOB --attach` gives a live, read-only shell inside a still-running
  job's own FUSE sandbox, so its in-progress outputs are visible at their real
  workspace-relative paths (writes fail with a read-only-filesystem error).
  Any user may attach to any job, not just their own -- the read-only guarantee
  and cross-user access both come from a narrow privileged helper
  (`lib/wake/wake-attach-helper`) that drops all privilege before handing you a
  shell. Package installs (`.deb`/`.rpm`) grant it the required file
  capabilities automatically; if you installed from source (`make install`),
  run `setcap 'cap_sys_admin,cap_sys_ptrace,cap_setpcap+ep' lib/wake/wake-attach-helper`
  yourself, or `--attach` will fail. Currently this only works on Linux and
  only for jobs run under the FUSE runner; it gives a filesystem view only, not
  process interaction (`ps`, signals) inside the sandbox.
