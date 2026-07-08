# Running Multiple Wake Invocations Concurrently

_Part of [Workspace Virtualization and Multiple Wake Invocations](../workspace-virtualization-and-multi-wake.md)._

Multiple wake processes may run concurrently in the same workspace. They
coordinate through the shared workspace database and CAS, and a run will reuse
jobs that other runs have already completed.

For non-concurrent use, behavior is designed to be identical to a single wake
invocation. Concurrency simply allows additional processes to participate in,
and benefit from, the same shared state.

## Observing active runs

Some command-line options let you inspect what is happening across all active
builds:

| Option        | Description                                                          |
| ------------- | -------------------------------------------------------------------- |
| `--history`   | Report the command-line history of all wake commands, including completed and in-flight runs. |
| `--ps`        | Show jobs currently running in active wake builds, grouped by run. Each job lists its id, elapsed time (or `[queued]` if not yet started), and label. |

### Example: watching jobs across concurrent runs

Start a long build in one terminal:

```
# terminal 1
wake -x 'buildEverything Unit'
```

Kick off a second, independent run in another terminal:

```
# terminal 2
wake -x 'runTests Unit'
```

Then, in a third terminal, watch the jobs active across *both* runs update in
real time. Because `wake --ps` reports jobs from every active build in the
workspace, the output reflects work from both terminals at once:

```
# terminal 3
watch -n0.5 wake --ps

Run 12: wake -x 'buildEverything Unit'
  JOB     ELAPSED     LABEL
  341     [1m12s]     compile core/main.cpp
  342     [1m04s]     compile core/util.cpp
  348     [queued]    link core

Run 13: wake -x 'runTests Unit'
  JOB     ELAPSED     LABEL
  360     [3s]        run test suite Unit
  361     [queued]    run test suite Integration
```

---

Next: [Managing Workspace Disk Usage with CAS](managing-disk-usage.md)
