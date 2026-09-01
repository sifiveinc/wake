# Inspecting a Live Job

_Part of [Workspace Virtualization and Multiple Wake Invocations](../workspace-virtualization-and-multi-wake.md)._

Use `wake --ps` to find a running job ID:

```
$ wake --ps
Run 12: wake -x 'buildEverything Unit'
  JOB     ELAPSED     LABEL
  341     [1m12s]     compile core/main.cpp
  342     [1m04s]     compile core/util.cpp
  348     [queued]    link core
```

Then attach a shell to job `341`'s live FUSE-sandboxed workspace:

```
wake --attach 341
```

The attached shell is read/write: changes to the workspace affect the running
job and can change its build result. Its banner also prints the host-visible
FUSE root for the job's live workspace. Use a host-installed external tool
against a workspace-relative live file below that root, for example:

```
fuse_root=/scratch/user/workspace/.fuse/uid.gid/job-pid
vim -R "$fuse_root/build/output"
```

The FUSE path presents live staging data under its real workspace-relative
names. It remains valid only while the job is running. It is the same writable
view used by the job, so do not modify it: writes can affect the build result.
