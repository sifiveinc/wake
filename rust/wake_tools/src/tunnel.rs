use crate::artifact::{ArtifactInspection, ArtifactRoot, DEFAULT_READ_LIMIT};
use crate::db::{JobDetail, JobFilter, JobState, JobSummary, RunSummary, WakeDb};
use anyhow::{anyhow, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashSet;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const CONFIG_VERSION: u32 = 1;
const REMOTE_QUERY_LIMIT: usize = 200;

#[derive(Clone, Debug, Deserialize)]
pub struct TunnelVisionConfig {
    pub version: u32,
    pub triage_id: String,
    pub sources: Vec<SourceConfig>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct SourceConfig {
    pub id: String,
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub runner: String,
    #[serde(default)]
    pub execution_host: String,
    pub database: String,
    pub artifact_root: String,
    #[serde(default = "default_timeout_seconds")]
    pub timeout_seconds: u64,
    #[serde(flatten)]
    pub transport: TransportConfig,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(tag = "transport", rename_all = "snake_case")]
pub enum TransportConfig {
    Local,
    Ssh {
        host: String,
        #[serde(default)]
        ssh_args: Vec<String>,
        #[serde(default = "default_remote_executable")]
        executable: String,
    },
    /// Execute an arbitrary argv prefix and append the wake-mcp read-only service arguments.
    /// This supports launchers such as `srun`, container exec, and site-specific Ray gateways.
    Command {
        command: Vec<String>,
    },
}

fn default_timeout_seconds() -> u64 {
    15
}

fn default_remote_executable() -> String {
    "wake-mcp".to_owned()
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SourceInfo {
    pub id: String,
    pub label: String,
    pub runner: String,
    pub host: String,
    pub remote: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct TunnelJob {
    pub source: SourceInfo,
    pub summary: JobSummary,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct TunnelJobDetail {
    pub source: SourceInfo,
    pub detail: JobDetail,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct TunnelSourceSummary {
    pub source: Option<SourceInfo>,
    pub jobs: usize,
    pub runs: usize,
    pub failed: usize,
    pub running: usize,
    pub error: Option<String>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct TunnelSnapshot {
    pub triage_id: String,
    pub jobs: Vec<TunnelJob>,
    pub sources: Vec<TunnelSourceSummary>,
    pub peak_parallelism: usize,
    pub starttime: Option<i64>,
    pub endtime: Option<i64>,
}

pub struct TunnelVision {
    triage_id: String,
    sources: Vec<TunnelSource>,
}

struct TunnelSource {
    info: SourceInfo,
    access: SourceAccess,
}

enum SourceAccess {
    Local { db: WakeDb, artifacts: ArtifactRoot },
    Remote(RemoteMcp),
}

struct RemoteMcp {
    command: Vec<String>,
    timeout: Duration,
    process: Option<McpProcess>,
    request_id: u64,
}

struct McpProcess {
    child: Child,
    stdin: ChildStdin,
    responses: Receiver<std::result::Result<String, String>>,
}

impl Drop for RemoteMcp {
    fn drop(&mut self) {
        self.stop();
    }
}

impl RemoteMcp {
    fn new(command: Vec<String>, timeout: Duration) -> Result<Self> {
        if command.is_empty() || command[0].is_empty() {
            return Err(anyhow!("remote MCP command cannot be empty"));
        }
        Ok(Self {
            command,
            timeout,
            process: None,
            request_id: 0,
        })
    }

    fn stop(&mut self) {
        if let Some(mut process) = self.process.take() {
            let _ = process.child.kill();
            let _ = process.child.wait();
        }
    }

    fn start(&mut self) -> Result<()> {
        let mut command = Command::new(&self.command[0]);
        command
            .args(&self.command[1..])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        let mut child = command
            .spawn()
            .with_context(|| format!("starting {}", self.command.join(" ")))?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| anyhow!("opening MCP stdin"))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| anyhow!("opening MCP stdout"))?;
        let (sender, responses) = mpsc::channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let line = line.map_err(|error| error.to_string());
                let failed = line.is_err();
                if sender.send(line).is_err() || failed {
                    return;
                }
            }
            let _ = sender.send(Err("remote MCP service closed its output".to_owned()));
        });
        self.process = Some(McpProcess {
            child,
            stdin,
            responses,
        });
        Ok(())
    }

    fn call_once(&mut self, tool: &str, arguments: Value) -> Result<Value> {
        if self.process.is_none() {
            self.start()?;
        }
        self.request_id = self.request_id.saturating_add(1);
        let request_id = self.request_id;
        let request = json!({
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": { "name": tool, "arguments": arguments }
        });
        let process = self.process.as_mut().unwrap();
        serde_json::to_writer(&mut process.stdin, &request)?;
        process.stdin.write_all(b"\n")?;
        process.stdin.flush()?;
        let line = process
            .responses
            .recv_timeout(self.timeout)
            .map_err(|error| anyhow!("remote MCP response: {error}"))?
            .map_err(|error| anyhow!(error))?;
        let response: Value =
            serde_json::from_str(&line).context("decoding remote MCP response")?;
        if response.get("id").and_then(Value::as_u64) != Some(request_id) {
            return Err(anyhow!("remote MCP response ID did not match request"));
        }
        if let Some(error) = response.pointer("/error/message").and_then(Value::as_str) {
            return Err(anyhow!("remote MCP error: {error}"));
        }
        let result = response
            .get("result")
            .ok_or_else(|| anyhow!("remote MCP response did not contain a result"))?;
        if result.get("isError").and_then(Value::as_bool) == Some(true) {
            let message = result
                .pointer("/content/0/text")
                .and_then(Value::as_str)
                .unwrap_or("remote Wake tool failed");
            return Err(anyhow!(message.to_owned()));
        }
        result
            .get("structuredContent")
            .cloned()
            .ok_or_else(|| anyhow!("remote Wake tool omitted structured content"))
    }

    fn call(&mut self, tool: &str, arguments: Value) -> Result<Value> {
        match self.call_once(tool, arguments.clone()) {
            Ok(value) => Ok(value),
            Err(first) => {
                self.stop();
                self.call_once(tool, arguments)
                    .with_context(|| format!("remote query failed after reconnect ({first})"))
            }
        }
    }
}

fn valid_identity(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\"'\"'"))
}

fn remote_arguments(source: &SourceConfig) -> Vec<String> {
    vec![
        "--database".to_owned(),
        source.database.clone(),
        "--artifact-root".to_owned(),
        source.artifact_root.clone(),
        "--source-id".to_owned(),
        source.id.clone(),
    ]
}

fn source_command(source: &SourceConfig) -> Result<Vec<String>> {
    let arguments = remote_arguments(source);
    match &source.transport {
        TransportConfig::Local => Err(anyhow!("local sources do not have a remote command")),
        TransportConfig::Command { command } => {
            if command.is_empty() {
                return Err(anyhow!("source {} has an empty command", source.id));
            }
            let mut result = command.clone();
            result.extend(arguments);
            Ok(result)
        }
        TransportConfig::Ssh {
            host,
            ssh_args,
            executable,
        } => {
            if host.is_empty() || executable.is_empty() {
                return Err(anyhow!("source {} has incomplete SSH settings", source.id));
            }
            let mut result = vec!["ssh".to_owned()];
            result.extend(ssh_args.clone());
            result.push("--".to_owned());
            result.push(host.clone());
            let remote_command = std::iter::once(executable.as_str())
                .chain(arguments.iter().map(String::as_str))
                .map(shell_quote)
                .collect::<Vec<_>>()
                .join(" ");
            result.push(remote_command);
            Ok(result)
        }
    }
}

fn resolve_local(base: &Path, value: &str) -> PathBuf {
    let path = Path::new(value);
    if path.is_absolute() {
        path.to_owned()
    } else {
        base.join(path)
    }
}

fn filter_arguments(filter: &JobFilter, offset: usize, limit: usize) -> Value {
    let state = match filter.state {
        JobState::All => "all",
        JobState::Failed => "failed",
        JobState::Passed => "passed",
        JobState::Running => "running",
    };
    let mut arguments = json!({
        "state": state,
        "include_noise": !filter.hide_noise,
        "offset": offset,
        "limit": limit.min(REMOTE_QUERY_LIMIT)
    });
    let object = arguments.as_object_mut().unwrap();
    if let Some(value) = &filter.query {
        object.insert("query".to_owned(), json!(value));
    }
    if let Some(value) = filter.run {
        object.insert("run_id".to_owned(), json!(value));
    }
    if let Some(value) = &filter.command {
        object.insert("command".to_owned(), json!(value));
    }
    if let Some(value) = &filter.artifact {
        object.insert("artifact".to_owned(), json!(value));
    }
    if let Some(value) = filter.min_runtime {
        object.insert("min_runtime".to_owned(), json!(value));
    }
    arguments
}

impl TunnelSource {
    fn jobs(&mut self, filter: &JobFilter, limit: usize) -> Result<Vec<JobSummary>> {
        match &mut self.access {
            SourceAccess::Local { db, .. } => db.filtered_jobs(filter, limit),
            SourceAccess::Remote(remote) => {
                let mut jobs = Vec::new();
                while jobs.len() < limit {
                    let page_limit = (limit - jobs.len()).min(REMOTE_QUERY_LIMIT);
                    let value = remote.call(
                        "list_wake_jobs",
                        filter_arguments(filter, jobs.len(), page_limit),
                    )?;
                    let page: Vec<JobSummary> = serde_json::from_value(value["jobs"].clone())
                        .context("decoding remote Wake jobs")?;
                    let page_len = page.len();
                    jobs.extend(page);
                    if page_len < page_limit {
                        break;
                    }
                }
                Ok(jobs)
            }
        }
    }

    fn job(&mut self, job_id: i64) -> Result<Option<JobDetail>> {
        match &mut self.access {
            SourceAccess::Local { db, .. } => db.job(job_id),
            SourceAccess::Remote(remote) => {
                let value = remote.call("get_wake_job", json!({ "job_id": job_id }))?;
                Ok(Some(
                    serde_json::from_value(value["job"].clone())
                        .context("decoding remote Wake job")?,
                ))
            }
        }
    }

    fn runs(&mut self, limit: usize) -> Result<Vec<RunSummary>> {
        match &mut self.access {
            SourceAccess::Local { db, .. } => db.runs(limit),
            SourceAccess::Remote(remote) => {
                let value = remote.call(
                    "list_wake_runs",
                    json!({ "limit": limit.min(REMOTE_QUERY_LIMIT) }),
                )?;
                serde_json::from_value(value["runs"].clone()).context("decoding remote Wake runs")
            }
        }
    }

    fn inspect(&mut self, path: &str, offset: u64, limit: usize) -> Result<ArtifactInspection> {
        match &mut self.access {
            SourceAccess::Local { artifacts, .. } => {
                artifacts.inspect(&self.info.id, path, offset, limit)
            }
            SourceAccess::Remote(remote) => {
                let value = remote.call(
                    "inspect_wake_artifact",
                    json!({ "path": path, "offset": offset, "limit": limit }),
                )?;
                serde_json::from_value(value["artifact"].clone())
                    .context("decoding remote Wake artifact")
            }
        }
    }
}

impl TunnelVision {
    pub fn discover(path: Option<PathBuf>) -> Result<Self> {
        if let Some(path) = path {
            return Self::load(path);
        }
        let mut directory = std::env::current_dir().context("finding current directory")?;
        loop {
            let candidate = directory.join(".wake/tunnel-vision.json");
            if candidate.is_file() {
                return Self::load(candidate);
            }
            if !directory.pop() {
                break;
            }
        }
        Err(anyhow!(
            "could not find .wake/tunnel-vision.json in this directory or its parents"
        ))
    }

    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let text = fs::read_to_string(path)
            .with_context(|| format!("reading Tunnel Vision config {}", path.display()))?;
        let config: TunnelVisionConfig = serde_json::from_str(&text)
            .with_context(|| format!("parsing Tunnel Vision config {}", path.display()))?;
        if config.version != CONFIG_VERSION {
            return Err(anyhow!(
                "unsupported Tunnel Vision config version {} (expected {CONFIG_VERSION})",
                config.version
            ));
        }
        if !valid_identity(&config.triage_id) {
            return Err(anyhow!(
                "triage_id must use only letters, digits, '.', '_', or '-'"
            ));
        }
        if config.sources.is_empty() {
            return Err(anyhow!(
                "Tunnel Vision config must contain at least one source"
            ));
        }
        let base = path.parent().unwrap_or_else(|| Path::new("."));
        let mut identities = HashSet::new();
        let mut sources = Vec::with_capacity(config.sources.len());
        for source in config.sources {
            if !valid_identity(&source.id) {
                return Err(anyhow!(
                    "source ID must use only letters, digits, '.', '_', or '-'"
                ));
            }
            if !identities.insert(source.id.clone()) {
                return Err(anyhow!("duplicate Tunnel Vision source ID: {}", source.id));
            }
            let remote = !matches!(source.transport, TransportConfig::Local);
            let label = if source.label.is_empty() {
                source.id.clone()
            } else {
                source.label.clone()
            };
            let runner = if source.runner.is_empty() {
                if remote { "remote" } else { "local" }.to_owned()
            } else {
                source.runner.clone()
            };
            let host = if !source.execution_host.is_empty() {
                source.execution_host.clone()
            } else if let TransportConfig::Ssh { host, .. } = &source.transport {
                host.clone()
            } else if remote {
                "remote".to_owned()
            } else {
                "localhost".to_owned()
            };
            let info = SourceInfo {
                id: source.id.clone(),
                label,
                runner,
                host,
                remote,
            };
            let access = match &source.transport {
                TransportConfig::Local => SourceAccess::Local {
                    db: WakeDb::new(resolve_local(base, &source.database))?,
                    artifacts: ArtifactRoot::new(resolve_local(base, &source.artifact_root))?,
                },
                _ => SourceAccess::Remote(RemoteMcp::new(
                    source_command(&source)?,
                    Duration::from_secs(source.timeout_seconds.clamp(1, 300)),
                )?),
            };
            sources.push(TunnelSource { info, access });
        }
        Ok(Self {
            triage_id: config.triage_id,
            sources,
        })
    }

    pub fn triage_id(&self) -> &str {
        &self.triage_id
    }

    pub fn snapshot(&mut self, filter: &JobFilter, limit: usize) -> TunnelSnapshot {
        let mut snapshot = TunnelSnapshot {
            triage_id: self.triage_id.clone(),
            ..TunnelSnapshot::default()
        };
        for source in &mut self.sources {
            let mut summary = TunnelSourceSummary {
                source: Some(source.info.clone()),
                ..TunnelSourceSummary::default()
            };
            match source.jobs(filter, limit) {
                Ok(jobs) => {
                    summary.jobs = jobs.len();
                    summary.failed = jobs
                        .iter()
                        .filter(|job| matches!(job.status, Some(status) if status != 0))
                        .count();
                    summary.running = jobs.iter().filter(|job| job.status.is_none()).count();
                    summary.runs = jobs.iter().map(|job| job.run).collect::<HashSet<_>>().len();
                    snapshot
                        .jobs
                        .extend(jobs.into_iter().map(|summary| TunnelJob {
                            source: source.info.clone(),
                            summary,
                        }));
                }
                Err(error) => summary.error = Some(error.to_string()),
            }
            snapshot.sources.push(summary);
        }
        snapshot.jobs.sort_by(|left, right| {
            right
                .summary
                .starttime
                .cmp(&left.summary.starttime)
                .then_with(|| left.source.id.cmp(&right.source.id))
                .then_with(|| right.summary.job.cmp(&left.summary.job))
        });
        let (peak, starttime, endtime) = parallelism(&snapshot.jobs);
        snapshot.peak_parallelism = peak;
        snapshot.starttime = starttime;
        snapshot.endtime = endtime;
        snapshot
    }

    pub fn job(&mut self, source_id: &str, job_id: i64) -> Result<Option<TunnelJobDetail>> {
        let source = self
            .sources
            .iter_mut()
            .find(|source| source.info.id == source_id)
            .ok_or_else(|| anyhow!("unknown Tunnel Vision source: {source_id}"))?;
        Ok(source.job(job_id)?.map(|detail| TunnelJobDetail {
            source: source.info.clone(),
            detail,
        }))
    }

    pub fn runs(&mut self, source_id: &str, limit: usize) -> Result<Vec<RunSummary>> {
        let source = self
            .sources
            .iter_mut()
            .find(|source| source.info.id == source_id)
            .ok_or_else(|| anyhow!("unknown Tunnel Vision source: {source_id}"))?;
        source.runs(limit)
    }

    pub fn inspect(
        &mut self,
        source_id: &str,
        path: &str,
        offset: u64,
        limit: usize,
    ) -> Result<ArtifactInspection> {
        let source = self
            .sources
            .iter_mut()
            .find(|source| source.info.id == source_id)
            .ok_or_else(|| anyhow!("unknown Tunnel Vision source: {source_id}"))?;
        source.inspect(path, offset, limit.clamp(1, DEFAULT_READ_LIMIT))
    }
}

fn parallelism(jobs: &[TunnelJob]) -> (usize, Option<i64>, Option<i64>) {
    let mut events = Vec::with_capacity(jobs.len() * 2);
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|duration| i64::try_from(duration.as_nanos()).ok())
        .unwrap_or(i64::MAX);
    for job in jobs {
        if job.summary.starttime <= 0 {
            continue;
        }
        let recorded_end = if job.summary.status.is_none() {
            now
        } else {
            job.summary.endtime
        };
        let endtime = recorded_end.max(job.summary.starttime.saturating_add(1));
        events.push((job.summary.starttime, 1_i64));
        events.push((endtime, -1_i64));
    }
    events.sort_by(|left, right| left.0.cmp(&right.0).then_with(|| left.1.cmp(&right.1)));
    let starttime = events.first().map(|event| event.0);
    let endtime = events.last().map(|event| event.0);
    let mut active = 0_i64;
    let mut peak = 0_i64;
    for (_, change) in events {
        active = (active + change).max(0);
        peak = peak.max(active);
    }
    (peak as usize, starttime, endtime)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::Connection;

    #[test]
    fn quotes_remote_arguments_without_shell_injection() {
        assert_eq!(shell_quote("a'b"), "'a'\"'\"'b'");
    }

    #[test]
    fn omits_unset_filters_from_remote_calls() {
        let arguments = filter_arguments(&JobFilter::default(), 200, 50);
        assert!(arguments.get("query").is_none());
        assert!(arguments.get("run_id").is_none());
        assert_eq!(arguments["offset"], 200);
        assert_eq!(arguments["limit"], 50);
    }

    #[test]
    fn builds_source_qualified_parallel_timeline() {
        let info = SourceInfo {
            id: "slurm".to_owned(),
            label: "Slurm".to_owned(),
            runner: "slurm".to_owned(),
            host: "gpu".to_owned(),
            remote: true,
        };
        let make_job = |job, starttime, endtime| TunnelJob {
            source: info.clone(),
            summary: JobSummary {
                job,
                run: 1,
                label: "job".to_owned(),
                directory: ".".to_owned(),
                commandline: Vec::new(),
                status: Some(0),
                runtime: None,
                cputime: None,
                membytes: None,
                ibytes: None,
                obytes: None,
                starttime,
                endtime,
                artifacts: Vec::new(),
                noise: None,
            },
        };
        let jobs = vec![
            make_job(1, 10, 30),
            make_job(2, 20, 40),
            make_job(3, 25, 35),
        ];
        assert_eq!(parallelism(&jobs), (3, Some(10), Some(40)));
    }

    #[test]
    fn loads_relative_local_source_and_reads_virtual_artifact() {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("wake-tunnel-config-{suffix}"));
        fs::create_dir_all(root.join(".wake")).unwrap();
        Connection::open(root.join("wake.db"))
            .unwrap()
            .execute_batch("PRAGMA user_version = 1")
            .unwrap();
        fs::write(root.join("artifact.txt"), "federated").unwrap();
        let config = root.join(".wake/tunnel-vision.json");
        fs::write(
            &config,
            r#"{
                "version": 1,
                "triage_id": "triage-7",
                "sources": [{
                    "id": "local-a",
                    "database": "../wake.db",
                    "artifact_root": "..",
                    "transport": "local"
                }]
            }"#,
        )
        .unwrap();
        let mut tunnel = TunnelVision::load(&config).unwrap();
        let artifact = tunnel
            .inspect("local-a", "artifact.txt", 0, DEFAULT_READ_LIMIT)
            .unwrap();
        assert_eq!(tunnel.triage_id(), "triage-7");
        assert_eq!(artifact.uri, "wake://local-a/artifact.txt");
        assert_eq!(artifact.content.as_deref(), Some("federated"));
        fs::remove_dir_all(root).unwrap();
    }
}
