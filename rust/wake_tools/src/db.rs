use anyhow::{anyhow, Context, Result};
use regex::Regex;
use rusqlite::{params, Connection, OpenFlags};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct WakeDb {
    path: PathBuf,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Artifact {
    pub path: String,
    pub kind: String,
    pub hash: String,
    pub mode: i64,
    pub deleted: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct JobSummary {
    pub job: i64,
    pub run: i64,
    pub label: String,
    pub directory: String,
    pub commandline: Vec<String>,
    pub status: Option<i64>,
    pub runtime: Option<f64>,
    pub cputime: Option<f64>,
    pub membytes: Option<i64>,
    pub ibytes: Option<i64>,
    pub obytes: Option<i64>,
    pub starttime: i64,
    pub endtime: i64,
    pub artifacts: Vec<Artifact>,
    /// Why this job is hidden from the actionable view, if applicable.
    pub noise: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct JobDetail {
    #[serde(flatten)]
    pub summary: JobSummary,
    pub environment: Vec<String>,
    pub stdin: String,
    pub stack: String,
    pub stdout: String,
    pub stderr: String,
    pub runner_output: String,
    pub runner_error: String,
    pub tags: Vec<Tag>,
    pub inputs: Vec<Artifact>,
    pub fanouts: Vec<FanoutConsumer>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct FanoutConsumer {
    pub artifact: String,
    pub job: i64,
    pub label: String,
    pub status: Option<i64>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ArtifactFanout {
    pub artifact: String,
    pub kind: String,
    pub producer_job: i64,
    pub producer_label: String,
    pub consumers: Vec<FanoutConsumer>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Tag {
    pub uri: String,
    pub content: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct RunSummary {
    pub run: i64,
    pub starttime: i64,
    pub endtime: Option<i64>,
    pub commandline: String,
}

#[derive(Clone, Debug)]
pub struct TelemetryRun {
    pub run: i64,
    pub starttime: i64,
    pub endtime: i64,
    pub used_jobs: i64,
    pub jobs: Vec<TelemetryJob>,
}

#[derive(Clone, Debug)]
pub struct TelemetryJob {
    pub job: i64,
    pub label: String,
    pub status: i64,
    pub runtime: f64,
    pub cputime: f64,
    pub membytes: i64,
    pub ibytes: i64,
    pub obytes: i64,
    pub starttime: i64,
    pub endtime: i64,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum JobState {
    #[default]
    All,
    Failed,
    Passed,
    Running,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct JobFilter {
    pub query: Option<String>,
    pub state: JobState,
    pub run: Option<i64>,
    pub command: Option<String>,
    pub artifact: Option<String>,
    pub min_runtime: Option<f64>,
    pub hide_noise: bool,
    /// Regular expression matched against the full command line when hiding noise.
    pub noise_regex: Option<String>,
}

impl Default for JobFilter {
    fn default() -> Self {
        Self {
            query: None,
            state: JobState::All,
            run: None,
            command: None,
            artifact: None,
            min_runtime: None,
            hide_noise: true,
            noise_regex: None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum GroupBy {
    #[default]
    Command,
    Label,
    Status,
    Artifact,
    Run,
}

impl GroupBy {
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "command" => Some(Self::Command),
            "label" => Some(Self::Label),
            "status" => Some(Self::Status),
            "artifact" => Some(Self::Artifact),
            "run" => Some(Self::Run),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Command => "command",
            Self::Label => "label",
            Self::Status => "status",
            Self::Artifact => "artifact",
            Self::Run => "run",
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct DashboardMetrics {
    pub jobs: usize,
    pub failed: usize,
    pub passed: usize,
    pub running: usize,
    pub hidden_noise: usize,
    pub commands: usize,
    pub artifacts: usize,
    pub fanout_edges: usize,
    pub total_runtime: f64,
    pub total_cputime: f64,
    pub io_bytes: i64,
    pub peak_memory_bytes: i64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct DashboardGroup {
    pub key: String,
    pub jobs: usize,
    pub failed: usize,
    pub artifacts: usize,
    pub runtime: f64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Dashboard {
    pub metrics: DashboardMetrics,
    pub group_by: String,
    pub groups: Vec<DashboardGroup>,
    pub failures: Vec<JobSummary>,
    pub fanouts: Vec<ArtifactFanout>,
}

fn split_blob(bytes: Vec<u8>) -> Vec<String> {
    bytes
        .split(|byte| *byte == 0)
        .filter(|part| !part.is_empty())
        .map(|part| String::from_utf8_lossy(part).into_owned())
        .collect()
}

fn contains_folded(value: &str, query: &str) -> bool {
    value.to_lowercase().contains(&query.to_lowercase())
}

fn command_name(job: &JobSummary) -> String {
    job.commandline
        .first()
        .map(|command| {
            Path::new(command)
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or(command)
                .to_owned()
        })
        .filter(|command| !command.is_empty())
        .unwrap_or_else(|| "(internal)".to_owned())
}

fn noise_reason(job: &JobSummary, noise_regex: Option<&Regex>) -> Option<String> {
    noise_regex
        .filter(|pattern| pattern.is_match(&job.commandline.join(" ")))
        .map(|pattern| format!("matches /{}/", pattern.as_str()))
}

fn matches_filter(job: &JobSummary, filter: &JobFilter) -> bool {
    if filter.hide_noise && job.noise.is_some() {
        return false;
    }
    if let Some(minimum) = filter.min_runtime {
        if job.runtime.unwrap_or_default() < minimum {
            return false;
        }
    }
    if let Some(command) = filter.command.as_deref() {
        if !contains_folded(&job.commandline.join(" "), command) {
            return false;
        }
    }
    if let Some(artifact) = filter.artifact.as_deref() {
        let matches = if artifact == "(no artifacts)" {
            job.artifacts.is_empty()
        } else {
            job.artifacts.iter().any(|output| {
                contains_folded(&output.path, artifact) || contains_folded(&output.kind, artifact)
            })
        };
        if !matches {
            return false;
        }
    }
    if let Some(query) = filter.query.as_deref() {
        let matches = contains_folded(&job.label, query)
            || job.job.to_string().contains(query)
            || contains_folded(&job.commandline.join(" "), query)
            || job
                .artifacts
                .iter()
                .any(|output| contains_folded(&output.path, query));
        if !matches {
            return false;
        }
    }
    true
}

impl WakeDb {
    pub fn discover(path: Option<PathBuf>) -> Result<Self> {
        if let Some(path) = path {
            return Self::new(path);
        }

        let mut directory = std::env::current_dir().context("finding current directory")?;
        loop {
            let candidate = directory.join("wake.db");
            if candidate.is_file() {
                return Self::new(candidate);
            }
            if !directory.pop() {
                break;
            }
        }
        Err(anyhow!(
            "could not find wake.db in this directory or its parents"
        ))
    }

    pub fn new(path: impl Into<PathBuf>) -> Result<Self> {
        let path = path.into();
        if !path.is_file() {
            return Err(anyhow!("Wake database does not exist: {}", path.display()));
        }
        let db = Self { path };
        let connection = db.open()?;
        let schema: i64 = connection
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .context("reading Wake database version")?;
        if schema == 0 {
            return Err(anyhow!(
                "{} is not an initialized Wake database",
                db.path.display()
            ));
        }
        Ok(db)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    fn open(&self) -> Result<Connection> {
        let connection = Connection::open_with_flags(
            &self.path,
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .with_context(|| format!("opening {}", self.path.display()))?;
        connection.busy_timeout(std::time::Duration::from_secs(5))?;
        Ok(connection)
    }

    pub fn jobs(
        &self,
        query: Option<&str>,
        state: JobState,
        limit: usize,
    ) -> Result<Vec<JobSummary>> {
        let filter = JobFilter {
            query: query.map(str::to_owned),
            state,
            hide_noise: false,
            ..JobFilter::default()
        };
        self.filtered_jobs(&filter, limit)
    }

    pub fn filtered_jobs(&self, filter: &JobFilter, limit: usize) -> Result<Vec<JobSummary>> {
        let noise_regex = filter
            .noise_regex
            .as_deref()
            .map(Regex::new)
            .transpose()
            .context("invalid exclusion regular expression")?;
        let connection = self.open()?;
        let state_filter = match filter.state {
            JobState::All => 0,
            JobState::Failed => 1,
            JobState::Passed => 2,
            JobState::Running => 3,
        };
        let mut statement = connection.prepare(
            "SELECT j.job_id, j.run_id, j.label, j.directory, j.commandline, s.status, \
                    s.runtime, s.cputime, s.membytes, s.ibytes, s.obytes, \
                    j.starttime, j.endtime \
             FROM jobs j LEFT JOIN stats s ON j.stat_id = s.stat_id \
             WHERE (?1 = 0 OR (?1 = 1 AND s.status IS NOT NULL AND s.status != 0) \
                            OR (?1 = 2 AND s.status = 0) \
                            OR (?1 = 3 AND s.status IS NULL)) \
               AND (?2 IS NULL OR EXISTS (SELECT 1 FROM run_jobs rj \
                                           WHERE rj.job_id = j.job_id AND rj.run_id = ?2)) \
             ORDER BY j.job_id DESC",
        )?;
        let rows = statement.query_map(params![state_filter, filter.run], |row| {
            Ok(JobSummary {
                job: row.get(0)?,
                run: row.get(1)?,
                label: row.get(2)?,
                directory: row.get(3)?,
                commandline: split_blob(row.get(4)?),
                status: row.get(5)?,
                runtime: row.get(6)?,
                cputime: row.get(7)?,
                membytes: row.get(8)?,
                ibytes: row.get(9)?,
                obytes: row.get(10)?,
                starttime: row.get(11)?,
                endtime: row.get(12)?,
                artifacts: Vec::new(),
                noise: None,
            })
        })?;
        let mut jobs: Vec<JobSummary> = rows.collect::<rusqlite::Result<_>>()?;
        let selected: HashSet<i64> = jobs.iter().map(|job| job.job).collect();
        let mut artifacts_by_job: HashMap<i64, Vec<Artifact>> = HashMap::new();
        let mut artifact_statement = connection.prepare(
            "SELECT ft.job_id, f.path, f.type, f.hash, f.mode, f.deleted \
             FROM filetree ft JOIN files f ON f.file_id = ft.file_id \
             WHERE ft.access = 2 ORDER BY ft.job_id, f.path",
        )?;
        let artifact_rows = artifact_statement.query_map([], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                Artifact {
                    path: row.get(1)?,
                    kind: row.get(2)?,
                    hash: row.get(3)?,
                    mode: row.get(4)?,
                    deleted: row.get::<_, i64>(5)? != 0,
                },
            ))
        })?;
        for row in artifact_rows {
            let (job_id, artifact) = row?;
            if selected.contains(&job_id) {
                artifacts_by_job.entry(job_id).or_default().push(artifact);
            }
        }
        for job in &mut jobs {
            job.artifacts = artifacts_by_job.remove(&job.job).unwrap_or_default();
            job.noise = noise_reason(job, noise_regex.as_ref());
        }
        jobs.retain(|job| matches_filter(job, filter));
        jobs.truncate(limit.clamp(1, 100_000));
        Ok(jobs)
    }

    pub fn filtered_jobs_page(
        &self,
        filter: &JobFilter,
        offset: usize,
        limit: usize,
    ) -> Result<Vec<JobSummary>> {
        let end = offset.saturating_add(limit).clamp(1, 100_000);
        let mut jobs = self.filtered_jobs(filter, end)?;
        if offset >= jobs.len() {
            return Ok(Vec::new());
        }
        jobs.drain(..offset);
        jobs.truncate(limit.clamp(1, 100_000));
        Ok(jobs)
    }

    pub fn job(&self, job_id: i64) -> Result<Option<JobDetail>> {
        let connection = self.open()?;
        let result = connection.query_row(
            "SELECT j.job_id, j.run_id, j.label, j.directory, j.commandline, s.status, \
                    s.runtime, s.cputime, s.membytes, s.ibytes, s.obytes, \
                    j.starttime, j.endtime, j.environment, j.stdin, j.stack \
             FROM jobs j LEFT JOIN stats s ON j.stat_id = s.stat_id WHERE j.job_id = ?1",
            [job_id],
            |row| {
                Ok(JobDetail {
                    summary: JobSummary {
                        job: row.get(0)?,
                        run: row.get(1)?,
                        label: row.get(2)?,
                        directory: row.get(3)?,
                        commandline: split_blob(row.get(4)?),
                        status: row.get(5)?,
                        runtime: row.get(6)?,
                        cputime: row.get(7)?,
                        membytes: row.get(8)?,
                        ibytes: row.get(9)?,
                        obytes: row.get(10)?,
                        starttime: row.get(11)?,
                        endtime: row.get(12)?,
                        artifacts: Vec::new(),
                        noise: None,
                    },
                    environment: split_blob(row.get(13)?),
                    stdin: row.get(14)?,
                    stack: String::from_utf8_lossy(&row.get::<_, Vec<u8>>(15)?).into_owned(),
                    stdout: String::new(),
                    stderr: String::new(),
                    runner_output: String::new(),
                    runner_error: String::new(),
                    tags: Vec::new(),
                    inputs: Vec::new(),
                    fanouts: Vec::new(),
                })
            },
        );
        let mut detail = match result {
            Ok(detail) => detail,
            Err(rusqlite::Error::QueryReturnedNoRows) => return Ok(None),
            Err(error) => return Err(error.into()),
        };

        let mut artifacts = connection.prepare(
            "SELECT f.path, f.type, f.hash, f.mode, f.deleted FROM filetree ft \
             JOIN files f ON f.file_id = ft.file_id \
             WHERE ft.job_id = ?1 AND ft.access = 2 ORDER BY f.path",
        )?;
        detail.summary.artifacts = artifacts
            .query_map([job_id], |row| {
                Ok(Artifact {
                    path: row.get(0)?,
                    kind: row.get(1)?,
                    hash: row.get(2)?,
                    mode: row.get(3)?,
                    deleted: row.get::<_, i64>(4)? != 0,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;
        detail.summary.noise = None;

        let mut inputs = connection.prepare(
            "SELECT f.path, f.type, f.hash, f.mode, f.deleted FROM filetree ft \
             JOIN files f ON f.file_id = ft.file_id \
             WHERE ft.job_id = ?1 AND ft.access = 1 ORDER BY f.path",
        )?;
        detail.inputs = inputs
            .query_map([job_id], |row| {
                Ok(Artifact {
                    path: row.get(0)?,
                    kind: row.get(1)?,
                    hash: row.get(2)?,
                    mode: row.get(3)?,
                    deleted: row.get::<_, i64>(4)? != 0,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;

        let mut fanouts = connection.prepare(
            "SELECT DISTINCT f.path, consumer.job_id, consumer.label, cs.status \
             FROM filetree output \
             JOIN files f ON f.file_id = output.file_id \
             JOIN filetree input ON input.file_id = output.file_id AND input.access = 1 \
             JOIN jobs consumer ON consumer.job_id = input.job_id \
             LEFT JOIN stats cs ON cs.stat_id = consumer.stat_id \
             WHERE output.job_id = ?1 AND output.access = 2 AND consumer.job_id != ?1 \
             ORDER BY f.path, consumer.job_id",
        )?;
        detail.fanouts = fanouts
            .query_map([job_id], |row| {
                Ok(FanoutConsumer {
                    artifact: row.get(0)?,
                    job: row.get(1)?,
                    label: row.get(2)?,
                    status: row.get(3)?,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;

        let mut logs = connection
            .prepare("SELECT descriptor, output FROM log WHERE job_id = ?1 ORDER BY log_id")?;
        for row in logs.query_map([job_id], |row| {
            Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
        })? {
            let (descriptor, output) = row?;
            match descriptor {
                1 => detail.stdout.push_str(&output),
                2 => detail.stderr.push_str(&output),
                3 => detail.runner_output.push_str(&output),
                4 => detail.runner_error.push_str(&output),
                _ => {}
            }
        }

        let mut tags = connection.prepare(
            "SELECT COALESCE(uri, ''), COALESCE(content, '') FROM tags WHERE job_id = ?1 ORDER BY uri",
        )?;
        detail.tags = tags
            .query_map([job_id], |row| {
                Ok(Tag {
                    uri: row.get(0)?,
                    content: row.get(1)?,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;
        Ok(Some(detail))
    }

    /// Whether a relative path is a recorded, non-deleted output artifact or is
    /// contained by a recorded output directory.
    pub fn owns_artifact_path(&self, path: &str) -> Result<bool> {
        let connection = self.open()?;
        let exists = connection.query_row(
            "SELECT EXISTS (
                 SELECT 1 FROM filetree ft JOIN files f ON f.file_id = ft.file_id
                 WHERE ft.access = 2 AND f.deleted = 0
                   AND (f.path = ?1 OR (f.type = 'directory'
                                        AND substr(?1, 1, length(f.path) + 1) = f.path || '/'))
             )",
            [path],
            |row| row.get(0),
        )?;
        Ok(exists)
    }

    pub fn fanouts(&self, filter: &JobFilter, limit: usize) -> Result<Vec<ArtifactFanout>> {
        let jobs = self.filtered_jobs(filter, 100_000)?;
        self.fanouts_for_jobs(&jobs, limit)
    }

    fn fanouts_for_jobs(&self, jobs: &[JobSummary], limit: usize) -> Result<Vec<ArtifactFanout>> {
        let selected: HashSet<i64> = jobs.iter().map(|job| job.job).collect();
        if selected.is_empty() {
            return Ok(Vec::new());
        }
        let connection = self.open()?;
        let mut statement = connection.prepare(
            "SELECT DISTINCT f.path, f.type, producer.job_id, producer.label, \
                    consumer.job_id, consumer.label, cs.status \
             FROM filetree output \
             JOIN files f ON f.file_id = output.file_id \
             JOIN jobs producer ON producer.job_id = output.job_id \
             JOIN filetree input ON input.file_id = output.file_id AND input.access = 1 \
             JOIN jobs consumer ON consumer.job_id = input.job_id \
             LEFT JOIN stats cs ON cs.stat_id = consumer.stat_id \
             WHERE output.access = 2 AND consumer.job_id != producer.job_id \
             ORDER BY producer.job_id DESC, f.path, consumer.job_id",
        )?;
        let rows = statement.query_map([], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, i64>(2)?,
                row.get::<_, String>(3)?,
                FanoutConsumer {
                    artifact: row.get(0)?,
                    job: row.get(4)?,
                    label: row.get(5)?,
                    status: row.get(6)?,
                },
            ))
        })?;
        let mut grouped: HashMap<(i64, String, String), ArtifactFanout> = HashMap::new();
        for row in rows {
            let (artifact, kind, producer_job, producer_label, consumer) = row?;
            if !selected.contains(&producer_job) {
                continue;
            }
            grouped
                .entry((producer_job, artifact.clone(), kind.clone()))
                .or_insert_with(|| ArtifactFanout {
                    artifact,
                    kind,
                    producer_job,
                    producer_label,
                    consumers: Vec::new(),
                })
                .consumers
                .push(consumer);
        }
        let mut fanouts: Vec<_> = grouped.into_values().collect();
        fanouts.sort_by(|left, right| {
            right
                .consumers
                .len()
                .cmp(&left.consumers.len())
                .then_with(|| right.producer_job.cmp(&left.producer_job))
                .then_with(|| left.artifact.cmp(&right.artifact))
        });
        fanouts.truncate(limit.clamp(1, 100_000));
        Ok(fanouts)
    }

    pub fn dashboard(
        &self,
        filter: &JobFilter,
        group_by: GroupBy,
        limit: usize,
    ) -> Result<Dashboard> {
        let mut including_noise = filter.clone();
        including_noise.hide_noise = false;
        let all_jobs = self.filtered_jobs(&including_noise, 100_000)?;
        let hidden_noise = if filter.hide_noise {
            all_jobs.iter().filter(|job| job.noise.is_some()).count()
        } else {
            0
        };
        let jobs: Vec<_> = all_jobs
            .into_iter()
            .filter(|job| !filter.hide_noise || job.noise.is_none())
            .collect();
        let all_fanouts = self.fanouts_for_jobs(&jobs, 100_000)?;
        let commands: HashSet<String> = jobs.iter().map(command_name).collect();
        let mut metrics = DashboardMetrics {
            jobs: jobs.len(),
            hidden_noise,
            commands: commands.len(),
            artifacts: jobs.iter().map(|job| job.artifacts.len()).sum(),
            fanout_edges: all_fanouts
                .iter()
                .map(|fanout| fanout.consumers.len())
                .sum(),
            total_runtime: jobs.iter().filter_map(|job| job.runtime).sum(),
            total_cputime: jobs.iter().filter_map(|job| job.cputime).sum(),
            io_bytes: jobs
                .iter()
                .map(|job| job.ibytes.unwrap_or_default() + job.obytes.unwrap_or_default())
                .sum(),
            peak_memory_bytes: jobs
                .iter()
                .filter_map(|job| job.membytes)
                .max()
                .unwrap_or_default(),
            ..DashboardMetrics::default()
        };
        for job in &jobs {
            match job.status {
                Some(0) => metrics.passed += 1,
                Some(_) => metrics.failed += 1,
                None => metrics.running += 1,
            }
        }

        let mut groups: HashMap<String, DashboardGroup> = HashMap::new();
        for job in &jobs {
            let keys: Vec<String> = match group_by {
                GroupBy::Command => vec![command_name(job)],
                GroupBy::Label => vec![if job.label.is_empty() {
                    "(unlabelled)".to_owned()
                } else {
                    job.label.clone()
                }],
                GroupBy::Status => vec![match job.status {
                    Some(0) => "passed".to_owned(),
                    Some(_) => "failed".to_owned(),
                    None => "running".to_owned(),
                }],
                GroupBy::Artifact => {
                    let kinds: HashSet<String> = job
                        .artifacts
                        .iter()
                        .map(|artifact| artifact.kind.clone())
                        .collect();
                    if kinds.is_empty() {
                        vec!["(no artifacts)".to_owned()]
                    } else {
                        kinds.into_iter().collect()
                    }
                }
                GroupBy::Run => vec![format!("run #{}", job.run)],
            };
            for key in keys {
                let group = groups.entry(key.clone()).or_insert(DashboardGroup {
                    key,
                    jobs: 0,
                    failed: 0,
                    artifacts: 0,
                    runtime: 0.0,
                });
                group.jobs += 1;
                group.failed += usize::from(matches!(job.status, Some(status) if status != 0));
                group.artifacts += job.artifacts.len();
                group.runtime += job.runtime.unwrap_or_default();
            }
        }
        let mut groups: Vec<_> = groups.into_values().collect();
        groups.sort_by(|left, right| {
            right
                .failed
                .cmp(&left.failed)
                .then_with(|| right.jobs.cmp(&left.jobs))
                .then_with(|| {
                    right
                        .runtime
                        .partial_cmp(&left.runtime)
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
                .then_with(|| left.key.cmp(&right.key))
        });
        groups.truncate(limit.clamp(1, 200));

        let failures = jobs
            .iter()
            .filter(|job| matches!(job.status, Some(status) if status != 0))
            .take(20)
            .cloned()
            .collect();
        let mut fanouts = all_fanouts;
        fanouts.truncate(20);
        Ok(Dashboard {
            metrics,
            group_by: group_by.as_str().to_owned(),
            groups,
            failures,
            fanouts,
        })
    }

    pub fn runs(&self, limit: usize) -> Result<Vec<RunSummary>> {
        let connection = self.open()?;
        let mut statement = connection.prepare(
            "SELECT run_id, time, end_time, cmdline FROM runs ORDER BY run_id DESC LIMIT ?1",
        )?;
        let runs = statement
            .query_map([limit.clamp(1, 1_000) as i64], |row| {
                Ok(RunSummary {
                    run: row.get(0)?,
                    starttime: row.get(1)?,
                    endtime: row.get(2)?,
                    commandline: row.get(3)?,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;
        Ok(runs)
    }

    pub fn telemetry_run(&self, run_id: i64) -> Result<Option<TelemetryRun>> {
        let connection = self.open()?;
        let run = connection.query_row(
            "SELECT run_id, time, end_time FROM runs WHERE run_id = ?1 AND end_time IS NOT NULL",
            [run_id],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, i64>(2)?,
                ))
            },
        );
        let (run, starttime, endtime) = match run {
            Ok(run) => run,
            Err(rusqlite::Error::QueryReturnedNoRows) => return Ok(None),
            Err(error) => return Err(error.into()),
        };
        let used_jobs = connection.query_row(
            "SELECT count(*) FROM run_jobs WHERE run_id = ?1",
            [run_id],
            |row| row.get(0),
        )?;
        let mut statement = connection.prepare(
            "SELECT j.job_id, j.label, s.status, s.runtime, s.cputime, s.membytes, \
                    s.ibytes, s.obytes, j.starttime, j.endtime \
             FROM jobs j JOIN stats s ON j.stat_id = s.stat_id \
             WHERE j.run_id = ?1 ORDER BY j.job_id",
        )?;
        let jobs = statement
            .query_map([run_id], |row| {
                Ok(TelemetryJob {
                    job: row.get(0)?,
                    label: row.get(1)?,
                    status: row.get(2)?,
                    runtime: row.get(3)?,
                    cputime: row.get(4)?,
                    membytes: row.get(5)?,
                    ibytes: row.get(6)?,
                    obytes: row.get(7)?,
                    starttime: row.get(8)?,
                    endtime: row.get(9)?,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;
        Ok(Some(TelemetryRun {
            run,
            starttime,
            endtime,
            used_jobs,
            jobs,
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::Connection;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn splits_wake_blobs() {
        assert_eq!(
            split_blob(b"cc\0-c\0main.c\0".to_vec()),
            ["cc", "-c", "main.c"]
        );
    }

    #[test]
    fn rejects_uninitialized_database() {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("wake-tools-{suffix}.db"));
        Connection::open(&path).unwrap();
        let result = WakeDb::new(&path);
        std::fs::remove_file(path).unwrap();
        assert!(result.is_err());
    }

    #[test]
    fn reads_completed_run_for_telemetry() {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("wake-telemetry-{suffix}.db"));
        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch(
                "PRAGMA user_version = 1;
                 CREATE TABLE runs(run_id INTEGER PRIMARY KEY, time INTEGER, end_time INTEGER);
                 CREATE TABLE run_jobs(run_id INTEGER, job_id INTEGER);
                 CREATE TABLE stats(stat_id INTEGER PRIMARY KEY, status INTEGER, runtime REAL,
                                    cputime REAL, membytes INTEGER, ibytes INTEGER, obytes INTEGER);
                 CREATE TABLE jobs(job_id INTEGER PRIMARY KEY, run_id INTEGER, label TEXT,
                                   stat_id INTEGER, starttime INTEGER, endtime INTEGER);
                 INSERT INTO runs VALUES(7, 100, 900);
                 INSERT INTO stats VALUES(3, 0, 0.8, 0.4, 1024, 20, 30);
                 INSERT INTO jobs VALUES(11, 7, 'compile', 3, 120, 800);
                 INSERT INTO run_jobs VALUES(7, 11);
                 INSERT INTO run_jobs VALUES(7, 4);",
            )
            .unwrap();
        drop(connection);

        let run = WakeDb::new(&path)
            .unwrap()
            .telemetry_run(7)
            .unwrap()
            .unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(run.used_jobs, 2);
        assert_eq!(run.jobs.len(), 1);
        assert_eq!(run.jobs[0].label, "compile");
    }

    #[test]
    fn dashboard_hides_noise_and_traces_fanout() {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("wake-dashboard-{suffix}.db"));
        let connection = Connection::open(&path).unwrap();
        connection
            .execute_batch(
                "PRAGMA user_version = 16;
                 CREATE TABLE runs(run_id INTEGER PRIMARY KEY, time INTEGER, end_time INTEGER,
                                   cmdline TEXT);
                 CREATE TABLE stats(stat_id INTEGER PRIMARY KEY, status INTEGER, runtime REAL,
                                    cputime REAL, membytes INTEGER, ibytes INTEGER, obytes INTEGER);
                 CREATE TABLE jobs(job_id INTEGER PRIMARY KEY, run_id INTEGER, label TEXT,
                                   directory TEXT, commandline BLOB, environment BLOB, stdin TEXT,
                                   stack BLOB, stat_id INTEGER, starttime INTEGER, endtime INTEGER);
                 CREATE TABLE run_jobs(run_id INTEGER, job_id INTEGER);
                 CREATE TABLE files(file_id INTEGER PRIMARY KEY, path TEXT, hash TEXT, type TEXT,
                                    mode INTEGER, deleted INTEGER);
                 CREATE TABLE filetree(job_id INTEGER, file_id INTEGER, access INTEGER);
                 CREATE TABLE log(log_id INTEGER PRIMARY KEY, job_id INTEGER, descriptor INTEGER,
                                  output TEXT);
                 CREATE TABLE tags(job_id INTEGER, uri TEXT, content TEXT);
                 INSERT INTO runs VALUES(1, 100, 900, 'wake all');
                 INSERT INTO stats VALUES(1, 0, .01, .01, 100, 0, 0);
                 INSERT INTO stats VALUES(2, 0, 2.0, 1.5, 2048, 10, 20);
                 INSERT INTO stats VALUES(3, 1, 1.0, .5, 1024, 30, 40);
                 INSERT INTO jobs VALUES(1, 1, 'make directory', '.',
                     X'3c6d6b6469723e002d70006f757400', X'', '', X'', 1, 100, 101);
                 INSERT INTO jobs VALUES(2, 1, 'compile', '.',
                     X'6363002d63006d61696e2e6300', X'', '', X'', 2, 102, 200);
                 INSERT INTO jobs VALUES(3, 1, 'link', '.',
                     X'6c64006d61696e2e6f00', X'', '', X'', 3, 201, 300);
                 INSERT INTO run_jobs VALUES(1, 1);
                 INSERT INTO run_jobs VALUES(1, 2);
                 INSERT INTO run_jobs VALUES(1, 3);
                 INSERT INTO files VALUES(10, 'out/main.o', 'abc', 'file', 420, 0);
                 INSERT INTO filetree VALUES(2, 10, 2);
                 INSERT INTO filetree VALUES(3, 10, 1);",
            )
            .unwrap();
        drop(connection);

        let db = WakeDb::new(&path).unwrap();
        let dashboard_filter = JobFilter {
            noise_regex: Some(r"^<mkdir>( |$)".to_owned()),
            ..JobFilter::default()
        };
        let dashboard = db
            .dashboard(&dashboard_filter, GroupBy::Command, 20)
            .unwrap();
        assert_eq!(dashboard.metrics.jobs, 2);
        assert_eq!(dashboard.metrics.failed, 1);
        assert_eq!(dashboard.metrics.hidden_noise, 1);
        assert_eq!(dashboard.metrics.fanout_edges, 1);
        assert_eq!(dashboard.fanouts[0].producer_job, 2);
        assert_eq!(dashboard.fanouts[0].consumers[0].job, 3);

        let mkdir_filter = JobFilter {
            command: Some("<mkdir>".to_owned()),
            noise_regex: Some(r"^<mkdir>( |$)".to_owned()),
            ..JobFilter::default()
        };
        assert!(db.filtered_jobs(&mkdir_filter, 20).unwrap().is_empty());
        let visible_mkdirs = db
            .filtered_jobs(
                &JobFilter {
                    hide_noise: false,
                    ..mkdir_filter
                },
                20,
            )
            .unwrap();
        assert_eq!(
            visible_mkdirs[0].noise.as_deref(),
            Some("matches /^<mkdir>( |$)/")
        );

        let detail = db.job(2).unwrap().unwrap();
        assert_eq!(detail.fanouts.len(), 1);
        assert_eq!(detail.fanouts[0].artifact, "out/main.o");
        let page = db
            .filtered_jobs_page(
                &JobFilter {
                    hide_noise: false,
                    ..JobFilter::default()
                },
                1,
                1,
            )
            .unwrap();
        assert_eq!(page.len(), 1);
        assert_eq!(page[0].job, 2);
        std::fs::remove_file(path).unwrap();
    }
}
