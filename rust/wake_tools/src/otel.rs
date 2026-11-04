use crate::db::{TelemetryJob, TelemetryRun};
use anyhow::{anyhow, Context as _, Result};
use opentelemetry::propagation::{Extractor, TextMapPropagator};
use opentelemetry::trace::{Span, SpanKind, Status, TraceContextExt, Tracer, TracerProvider as _};
use opentelemetry::{Context, KeyValue};
use opentelemetry_otlp::{Protocol, SpanExporter as OtlpSpanExporter, WithExportConfig};
use opentelemetry_sdk::error::OTelSdkResult;
use opentelemetry_sdk::propagation::TraceContextPropagator;
use opentelemetry_sdk::trace::{
    BatchConfigBuilder, BatchSpanProcessor, SdkTracerProvider, SpanData,
};
use opentelemetry_sdk::Resource;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

struct ParentTraceContext {
    traceparent: String,
    tracestate: Option<String>,
}

impl Extractor for ParentTraceContext {
    fn get(&self, key: &str) -> Option<&str> {
        match key {
            "traceparent" => Some(self.traceparent.as_str()),
            "tracestate" => self.tracestate.as_deref(),
            _ => None,
        }
    }

    fn keys(&self) -> Vec<&str> {
        if self.tracestate.is_some() {
            vec!["traceparent", "tracestate"]
        } else {
            vec!["traceparent"]
        }
    }
}

fn parent_context() -> Context {
    let Ok(traceparent) = std::env::var("WAKE_OTEL_PARENT_TRACEPARENT") else {
        return Context::new();
    };
    TraceContextPropagator::new().extract(&ParentTraceContext {
        traceparent,
        tracestate: std::env::var("WAKE_OTEL_PARENT_TRACESTATE").ok(),
    })
}

#[derive(Debug)]
struct RecordingExporter {
    inner: OtlpSpanExporter,
    error: Arc<Mutex<Option<String>>>,
}

impl opentelemetry_sdk::trace::SpanExporter for RecordingExporter {
    async fn export(&self, batch: Vec<SpanData>) -> OTelSdkResult {
        let result = self.inner.export(batch).await;
        if let Err(error) = &result {
            if let Ok(mut recorded) = self.error.lock() {
                *recorded = Some(error.to_string());
            }
        }
        result
    }

    fn shutdown_with_timeout(&self, timeout: Duration) -> OTelSdkResult {
        self.inner.shutdown_with_timeout(timeout)
    }

    fn force_flush(&self) -> OTelSdkResult {
        self.inner.force_flush()
    }

    fn set_resource(&mut self, resource: &Resource) {
        self.inner.set_resource(resource);
    }
}

fn timestamp(nanoseconds: i64) -> Result<SystemTime> {
    let nanoseconds = u64::try_from(nanoseconds)
        .map_err(|_| anyhow!("negative Wake timestamp: {nanoseconds}"))?;
    Ok(UNIX_EPOCH + Duration::from_nanos(nanoseconds))
}

fn service_name_is_configured() -> bool {
    if std::env::var_os("OTEL_SERVICE_NAME").is_some() {
        return true;
    }
    std::env::var("OTEL_RESOURCE_ATTRIBUTES")
        .map(|attributes| {
            attributes
                .split(',')
                .any(|attribute| attribute.trim_start().starts_with("service.name="))
        })
        .unwrap_or(false)
}

fn resource(wake_version: &str) -> Resource {
    let builder = Resource::builder()
        .with_attribute(KeyValue::new("service.version", wake_version.to_owned()));
    if service_name_is_configured() {
        builder.build()
    } else {
        builder.with_service_name("wake").build()
    }
}

fn job_attributes(job: &TelemetryJob) -> Vec<KeyValue> {
    vec![
        KeyValue::new("wake.job.id", job.job),
        KeyValue::new("wake.job.label", job.label.clone()),
        KeyValue::new("wake.job.exit_code", job.status),
        KeyValue::new("wake.job.runtime_seconds", job.runtime),
        KeyValue::new("wake.job.cpu_seconds", job.cputime),
        KeyValue::new("wake.job.memory_bytes", job.membytes),
        KeyValue::new("wake.job.io_read_bytes", job.ibytes),
        KeyValue::new("wake.job.io_write_bytes", job.obytes),
    ]
}

pub fn export_run(run: &TelemetryRun, exit_code: i32, wake_version: &str) -> Result<()> {
    let exporter = OtlpSpanExporter::builder()
        .with_http()
        .with_protocol(Protocol::HttpBinary)
        .build()
        .context("building OTLP trace exporter")?;
    let export_error = Arc::new(Mutex::new(None));
    let exporter = RecordingExporter {
        inner: exporter,
        error: Arc::clone(&export_error),
    };
    let batch_processor = BatchSpanProcessor::builder(exporter)
        .with_batch_config(
            BatchConfigBuilder::default()
                .with_max_queue_size(run.jobs.len().saturating_add(1))
                .build(),
        )
        .build();
    let provider = SdkTracerProvider::builder()
        .with_resource(resource(wake_version))
        .with_span_processor(batch_processor)
        .build();
    let tracer = provider.tracer("wake");
    let executed_jobs = i64::try_from(run.jobs.len()).unwrap_or(i64::MAX);
    let cached_jobs = run.used_jobs.saturating_sub(executed_jobs);
    let run_span = tracer
        .span_builder("wake.run")
        .with_kind(SpanKind::Internal)
        .with_start_time(timestamp(run.starttime)?)
        .with_attributes(vec![
            KeyValue::new("wake.run.id", run.run),
            KeyValue::new("wake.run.exit_code", i64::from(exit_code)),
            KeyValue::new("wake.jobs.used", run.used_jobs),
            KeyValue::new("wake.jobs.executed", executed_jobs),
            KeyValue::new("wake.jobs.cached", cached_jobs),
        ])
        .start_with_context(&tracer, &parent_context());
    let run_context = Context::current_with_span(run_span);

    for job in &run.jobs {
        let mut span = tracer
            .span_builder(job.label.clone())
            .with_kind(SpanKind::Internal)
            .with_start_time(timestamp(job.starttime)?)
            .with_attributes(job_attributes(job))
            .start_with_context(&tracer, &run_context);
        if job.status != 0 {
            span.set_status(Status::error(format!("exit status {}", job.status)));
        }
        span.end_with_timestamp(timestamp(job.endtime)?);
    }

    if exit_code != 0 {
        run_context.span().set_status(Status::error(format!(
            "wake exited with status {exit_code}"
        )));
    }
    run_context
        .span()
        .end_with_timestamp(timestamp(run.endtime)?);
    drop(run_context);
    let shutdown = provider.shutdown_with_timeout(Duration::from_secs(60));
    let recorded_error = export_error
        .lock()
        .map_err(|_| anyhow!("OpenTelemetry exporter error lock was poisoned"))?
        .take();
    if let Some(error) = recorded_error {
        return Err(anyhow!("exporting OTLP traces: {error}"));
    }
    shutdown.context("flushing OTLP traces")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_negative_timestamps() {
        assert!(timestamp(-1).is_err());
    }

    #[test]
    fn creates_expected_job_attributes() {
        let attributes = job_attributes(&TelemetryJob {
            job: 4,
            label: "compile".to_owned(),
            status: 1,
            runtime: 2.0,
            cputime: 1.0,
            membytes: 3,
            ibytes: 5,
            obytes: 8,
            starttime: 10,
            endtime: 20,
        });
        assert_eq!(attributes.len(), 8);
        assert_eq!(attributes[0].key.as_str(), "wake.job.id");
    }

    #[test]
    fn extracts_parent_trace_context() {
        let context = TraceContextPropagator::new().extract(&ParentTraceContext {
            traceparent: "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01".to_owned(),
            tracestate: None,
        });
        let span_context = context.span().span_context().clone();
        assert!(span_context.is_valid());
        assert!(span_context.is_remote());
        assert_eq!(
            span_context.trace_id().to_string(),
            "4bf92f3577b34da6a3ce929d0e0e4736"
        );
    }
}
