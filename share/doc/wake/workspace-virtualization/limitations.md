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
