use crate::db::{TelemetryJob, TelemetryRun};
use anyhow::{anyhow, Context as _, Result};
use opentelemetry::trace::{Span, SpanKind, Status, TraceContextExt, Tracer, TracerProvider as _};
use opentelemetry::{Context, KeyValue};
use opentelemetry_otlp::{Protocol, SpanExporter as OtlpSpanExporter, WithExportConfig};
use opentelemetry_sdk::error::OTelSdkResult;
use opentelemetry_sdk::trace::{SdkTracerProvider, SpanData};
use opentelemetry_sdk::Resource;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

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
    let provider = SdkTracerProvider::builder()
        .with_resource(resource(wake_version))
        .with_batch_exporter(exporter)
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
        .start(&tracer);
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
    let shutdown = provider.shutdown();
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
}
