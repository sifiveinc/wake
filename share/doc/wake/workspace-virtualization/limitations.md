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

- **Inspecting long-running jobs.** With workspace virtualization, a job's
  outputs are not ingested into the CAS or materialized into your workspace
  until the job *completes*. While a job runs, its data lives in a staging
  directory under `.build/cas/staging/`. Along with the fact that these files should not be modified, these staging files are stored under generated names rather than their real output paths. This makes
  it difficult to observe the in-progress state of a long-running (or
  potentially hung) job. You can still introspect running jobs through wake:

  - `wake --ps` lists jobs currently running across all active builds, grouped
    by run, with each job's id, elapsed time (or `[queued]`), and label. Use it
    to find the id of the job you want to inspect.
  - `wake --job <jobID> -v` describes a specific job in verbose form, including
    its command-line, environment, working directory, and any stdout/stderr
    streamed so far.

  These options let you watch a job's command and streamed output, but they do
  *not* expose the job's not-yet-completed output files, since those are not
  materialized until the job finishes. Inspecting the in-progress build outputs
  of a long-running job is something the Wake team is actively looking into.
