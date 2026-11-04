# OpenTelemetry support

Wake is an opt-in OpenTelemetry client. After a normal build completes, it
reads that run's committed records from `wake.db` and exports one trace using
OTLP over HTTP with protobuf. Export happens after the run is marked complete,
and exporter failures do not affect the build's exit status.

## Enabling export

Setting either standard endpoint variable enables export automatically:

```sh
OTEL_EXPORTER_OTLP_ENDPOINT=http://collector:4318 wake build
OTEL_EXPORTER_OTLP_TRACES_ENDPOINT=https://collector.example/v1/traces wake build
```

With `OTEL_EXPORTER_OTLP_ENDPOINT`, the exporter appends `/v1/traces`. The
signal-specific `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` is used as-is. `wake
--otel build` also enables export and uses the standard default endpoint when
neither variable is present.

Wake and the Rust OpenTelemetry SDK honor these standard variables:

- `OTEL_EXPORTER_OTLP_ENDPOINT`
- `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`
- `OTEL_EXPORTER_OTLP_HEADERS` and `OTEL_EXPORTER_OTLP_TRACES_HEADERS`
- `OTEL_EXPORTER_OTLP_TIMEOUT` and `OTEL_EXPORTER_OTLP_TRACES_TIMEOUT`
- `OTEL_SERVICE_NAME` and `OTEL_RESOURCE_ATTRIBUTES`
- `OTEL_TRACES_SAMPLER` and `OTEL_TRACES_SAMPLER_ARG`
- `OTEL_SDK_DISABLED`

An orchestrator can make `wake.run` a child of an existing distributed trace
by passing its W3C context in `WAKE_OTEL_PARENT_TRACEPARENT` and, when present,
`WAKE_OTEL_PARENT_TRACESTATE`. Wake does not write these variables itself.

The client uses OTLP/HTTP protobuf. HTTPS endpoints use the host's trusted root
certificates. If no service name is configured, Wake uses `wake`.

## Trace model

Every exported Wake trace segment has a `wake.run` span covering the recorded
run start and end timestamps. It is a root span unless an orchestrator supplies
the W3C parent context described above. It contains:

- `wake.run.id`
- `wake.run.exit_code`
- `wake.jobs.used`
- `wake.jobs.executed`
- `wake.jobs.cached`

Jobs actually executed by that invocation are child spans named after their
Wake labels. Each child includes:

- `wake.job.id`, `wake.job.label`, and `wake.job.exit_code`
- `wake.job.runtime_seconds` and `wake.job.cpu_seconds`
- `wake.job.memory_bytes`
- `wake.job.io_read_bytes` and `wake.job.io_write_bytes`

Cached jobs contribute to the root counters but do not become child spans,
because they have no execution interval in the current run. Failed jobs and
failed runs set the OpenTelemetry span status to error.

Wake deliberately omits command lines, environment variables, standard output,
standard error, artifact paths, and file contents from telemetry.
