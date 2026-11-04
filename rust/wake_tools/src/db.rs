use anyhow::{anyhow, Context, Result};
use rusqlite::{params, Connection, OpenFlags};
use serde::Serialize;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct WakeDb {
    path: PathBuf,
}

#[derive(Clone, Debug, Serialize)]
pub struct Artifact {
    pub path: String,
    pub kind: String,
    pub hash: String,
    pub mode: i64,
    pub deleted: bool,
}

#[derive(Clone, Debug, Serialize)]
pub struct JobSummary {
    pub job: i64,
    pub run: i64,
    pub label: String,
    pub directory: String,
    pub commandline: Vec<String>,
    pub status: Option<i64>,
    pub runtime: Option<f64>,
    pub starttime: i64,
    pub endtime: i64,
    pub artifacts: Vec<Artifact>,
}

#[derive(Clone, Debug, Serialize)]
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
}

#[derive(Clone, Debug, Serialize)]
pub struct Tag {
    pub uri: String,
    pub content: String,
}

#[derive(Clone, Debug, Serialize)]
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

#[derive(Clone, Copy, Debug, Default)]
pub enum JobState {
    #[default]
    All,
    Failed,
    Passed,
}

fn split_blob(bytes: Vec<u8>) -> Vec<String> {
    bytes
        .split(|byte| *byte == 0)
        .filter(|part| !part.is_empty())
        .map(|part| String::from_utf8_lossy(part).into_owned())
        .collect()
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
        let connection = self.open()?;
        let limit = limit.clamp(1, 1_000) as i64;
        let pattern = format!("%{}%", query.unwrap_or_default());
        let state_filter = match state {
            JobState::All => 0,
            JobState::Failed => 1,
            JobState::Passed => 2,
        };
        let mut statement = connection.prepare(
            "SELECT j.job_id, j.run_id, j.label, j.directory, j.commandline, s.status, \
                    s.runtime, j.starttime, j.endtime \
             FROM jobs j LEFT JOIN stats s ON j.stat_id = s.stat_id \
             WHERE (?1 = '%%' OR j.label LIKE ?1 OR CAST(j.job_id AS TEXT) LIKE ?1 \
                    OR EXISTS (SELECT 1 FROM filetree ft JOIN files f ON f.file_id = ft.file_id \
                               WHERE ft.job_id = j.job_id AND ft.access = 2 AND f.path LIKE ?1)) \
               AND (?2 = 0 OR (?2 = 1 AND s.status IS NOT NULL AND s.status != 0) \
                            OR (?2 = 2 AND s.status = 0)) \
             ORDER BY j.job_id DESC LIMIT ?3",
        )?;
        let rows = statement.query_map(params![pattern, state_filter, limit], |row| {
            Ok(JobSummary {
                job: row.get(0)?,
                run: row.get(1)?,
                label: row.get(2)?,
                directory: row.get(3)?,
                commandline: split_blob(row.get(4)?),
                status: row.get(5)?,
                runtime: row.get(6)?,
                starttime: row.get(7)?,
                endtime: row.get(8)?,
                artifacts: Vec::new(),
            })
        })?;
        let mut jobs: Vec<JobSummary> = rows.collect::<rusqlite::Result<_>>()?;
        let mut artifact_statement = connection.prepare(
            "SELECT f.path, f.type, f.hash, f.mode, f.deleted \
             FROM filetree ft JOIN files f ON f.file_id = ft.file_id \
             WHERE ft.job_id = ?1 AND ft.access = 2 ORDER BY f.path",
        )?;
        for job in &mut jobs {
            job.artifacts = artifact_statement
                .query_map([job.job], |row| {
                    Ok(Artifact {
                        path: row.get(0)?,
                        kind: row.get(1)?,
                        hash: row.get(2)?,
                        mode: row.get(3)?,
                        deleted: row.get::<_, i64>(4)? != 0,
                    })
                })?
                .collect::<rusqlite::Result<_>>()?;
        }
        Ok(jobs)
    }

    pub fn job(&self, job_id: i64) -> Result<Option<JobDetail>> {
        let connection = self.open()?;
        let result = connection.query_row(
            "SELECT j.job_id, j.run_id, j.label, j.directory, j.commandline, s.status, \
                    s.runtime, j.starttime, j.endtime, j.environment, j.stdin, j.stack \
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
                        starttime: row.get(7)?,
                        endtime: row.get(8)?,
                        artifacts: Vec::new(),
                    },
                    environment: split_blob(row.get(9)?),
                    stdin: row.get(10)?,
                    stack: String::from_utf8_lossy(&row.get::<_, Vec<u8>>(11)?).into_owned(),
                    stdout: String::new(),
                    stderr: String::new(),
                    runner_output: String::new(),
                    runner_error: String::new(),
                    tags: Vec::new(),
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
}
