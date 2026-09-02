# Tunnel Vision

Tunnel Vision is Wake's federated triage flow for work that fans out to local,
Slurm, Ray, or other remote runners. It keeps two concerns separate:

- OpenTelemetry is the trace plane. A shared W3C parent makes every Wake run a
  sibling in one distributed trace, and stable triage/source attributes make
  the trace searchable.
- `wake-mcp` is the read-only data plane. The TUI keeps a session to the MCP
  process beside each destination `wake.db` and asks that process for database
  records or bounded artifact previews. Databases and artifacts are not copied
  to the coordinator.

This separation is intentional: telemetry stays small and does not contain
commands, logs, paths, or file content, while detailed data remains on the host
that owns it.

## Configure the virtual workspace

Create `.wake/tunnel-vision.json`. Paths on local sources are resolved relative
to the directory containing the config. Paths on remote sources are passed to
the remote MCP process unchanged.

```json
{
  "version": 1,
  "triage_id": "change-1842-attempt-3",
  "sources": [
    {
      "id": "local",
      "label": "coordinator",
      "runner": "local",
      "database": "../wake.db",
      "artifact_root": "..",
      "transport": "local"
    },
    {
      "id": "slurm-gpu-a",
      "label": "Slurm GPU partition",
      "runner": "slurm",
      "execution_host": "gpu-a.internal",
      "database": "/scratch/run-1842/wake.db",
      "artifact_root": "/scratch/run-1842",
      "timeout_seconds": 20,
      "transport": "ssh",
      "host": "gpu-a.internal",
      "ssh_args": ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"],
      "executable": "/opt/wake/lib/wake/wake-mcp"
    },
    {
      "id": "ray-worker-b",
      "label": "Ray worker B",
      "runner": "ray",
      "execution_host": "ray-b.internal",
      "database": "/mnt/ray/session/wake.db",
      "artifact_root": "/mnt/ray/session",
      "transport": "command",
      "command": ["ray-mcp-gateway", "--worker", "ray-b"]
    }
  ]
}
```

For `ssh`, Tunnel Vision safely quotes the remote executable and generated
arguments. For `command`, it executes the declared argv directly and appends:

```text
--database DB --artifact-root ROOT --source-id SOURCE
```

The command must expose `wake-mcp` protocol-compatible stdin/stdout. This makes
site-specific `srun`, container, and Ray gateways possible without teaching
the TUI scheduler-specific control operations.

Start the view from the workspace or any child directory:

```sh
wake tunnel-vision
```

The config may also be selected explicitly when invoking the companion binary:

```sh
wake-tui --tunnel-config /path/to/tunnel-vision.json
```

## Correlate distributed runs

The launcher that creates a triage should create one orchestration span and
propagate its W3C context plus the Tunnel Vision identity to every Wake
invocation. A Slurm export looks like this conceptually:

```sh
export WAKE_OTEL_PARENT_TRACEPARENT="$TRIAGE_TRACEPARENT"
export WAKE_OTEL_PARENT_TRACESTATE="$TRIAGE_TRACESTATE"
export WAKE_TUNNEL_TRIAGE_ID=change-1842-attempt-3
export WAKE_TUNNEL_SOURCE_ID=slurm-gpu-a
export WAKE_RUNNER_KIND=slurm
export WAKE_RUNNER_HOST=gpu-a.internal
srun --export=ALL wake --otel build
```

Ray should put the same values in each task's runtime environment, changing
only source and host. Every `wake.run` is a child of the orchestration span;
executed jobs remain children of their owning run. Runs or jobs that overlap in
wall-clock time are siblings. Clock order alone never creates a causal edge.

The exported run attributes are:

| Attribute | Meaning |
| --- | --- |
| `wake.triage.id` | Stable triage/search identity; matches the config |
| `wake.source.id` | Globally unique source within the triage |
| `wake.run.id` | Database-local run ID |
| `wake.run.coordinate` | Source-qualified run identity, such as `slurm-gpu-a:7` |
| `wake.runner.kind` | `local`, `slurm`, `ray`, or a site-defined runner |
| `wake.runner.host` | Execution host or stable worker identity |

Job IDs are also database-local. The TUI therefore renders `source#job`, never
a bare remote job ID.

## Triage and parallel presentation

The dashboard shows one execution lane per configured source, including its
runner, host, run count, running count, and failures. A source that is
temporarily unavailable reports its error without removing jobs returned by
healthy sources. The triage queue mixes active jobs and failures across lanes,
ordered by recorded start time. Peak parallelism is calculated from all job
start/end intervals using half-open intervals; it is not the sum of per-run
durations. Cross-host overlap is observational and assumes reasonably
synchronized clocks (NTP/PTP). Clock order never creates a causal edge; the W3C
parent context supplies causality even when host clocks drift.

Use `d` to enter the job view, `j`/`k` to move between source-qualified jobs,
`[`/`]` to choose an output artifact, and `a` or Enter to request a preview.
Artifacts use virtual URIs:

```text
wake://SOURCE/relative/artifact/path
```

The URI is logical; the owning source resolves it against its configured
artifact root.

## Read-only and failure boundaries

- All database connections use SQLite read-only mode.
- MCP advertises only read-only, idempotent tools. There are no write, delete,
  launch, signal, or shell-execution artifact tools.
- Artifact paths must be non-empty and relative. Canonical resolution must
  remain beneath the configured root, including through symlinks.
- The remote service only accepts non-deleted output paths recorded in its
  `wake.db` (or descendants of a recorded output directory).
- A read returns at most 1 MiB at the protocol layer; the TUI requests 64 KiB.
  Directory listings return at most 2,000 sorted entries.
- File bytes are shown as lossy UTF-8 for triage. Binary download and mutation
  are deliberately outside this flow.
- Remote requests time out, reconnect once, and then mark that source
  unavailable for the refresh. Configure SSH batch mode and connection
  timeouts so authentication cannot block the TUI.

Treat access to `wake-mcp` as access to the configured database metadata,
logs, and artifact contents. Use normal SSH authentication and authorization;
the stdio protocol itself does not add credentials.

## Delivery plan

The first end-to-end slice includes OTEL correlation attributes, W3C sibling
run semantics, config-driven local/remote federation, source-qualified
identity, parallel source lanes, resilient remote sessions, and confined
artifact previews. Natural follow-on work is collector-backed source discovery
(so config can be generated from trace attributes), binary-safe ranged export,
and scheduler adapters that publish worker endpoints automatically. Those can
extend the same trace/data-plane contracts without changing the virtual URI or
TUI identity model.
