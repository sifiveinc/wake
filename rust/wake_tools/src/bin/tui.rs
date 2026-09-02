use anyhow::{anyhow, Result};
use clap::Parser;
use crossterm::event::{self, Event, KeyCode, KeyEventKind};
use crossterm::execute;
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use ratatui::backend::CrosstermBackend;
use ratatui::layout::{Constraint, Direction, Layout};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, List, ListItem, ListState, Paragraph, Wrap};
use ratatui::Terminal;
use std::io::{self, IsTerminal};
use std::path::PathBuf;
use std::time::{Duration, Instant};
use wake_tools::artifact::{ArtifactInspection, ArtifactRoot, DEFAULT_READ_LIMIT};
use wake_tools::db::{Dashboard, GroupBy, JobDetail, JobFilter, JobState, JobSummary, WakeDb};
use wake_tools::tunnel::{SourceInfo, TunnelSnapshot, TunnelVision};

#[derive(Debug, Parser)]
#[command(
    name = "wake-tui",
    about = "Interactively triage Wake jobs and artifacts"
)]
struct Options {
    #[arg(long)]
    database: Option<PathBuf>,
    #[arg(long, conflicts_with = "database")]
    tunnel_vision: bool,
    #[arg(long, conflicts_with = "database")]
    tunnel_config: Option<PathBuf>,
}

struct TerminalGuard;

impl TerminalGuard {
    fn enter() -> Result<Self> {
        enable_raw_mode()?;
        execute!(io::stdout(), EnterAlternateScreen)?;
        Ok(Self)
    }
}

impl Drop for TerminalGuard {
    fn drop(&mut self) {
        let _ = disable_raw_mode();
        let _ = execute!(io::stdout(), LeaveAlternateScreen);
    }
}

struct App {
    backend: Backend,
    dashboard: Option<Dashboard>,
    tunnel: Option<TunnelSnapshot>,
    jobs: Vec<JobSummary>,
    job_sources: Vec<Option<SourceInfo>>,
    detail: Option<JobDetail>,
    detail_source: Option<SourceInfo>,
    detail_error: Option<String>,
    artifact_index: usize,
    artifact_preview: Option<ArtifactInspection>,
    artifact_error: Option<String>,
    selected: usize,
    filter: String,
    state: JobState,
    group_by: GroupBy,
    include_noise: bool,
    view: View,
    editing: bool,
}

enum Backend {
    Local { db: WakeDb, artifacts: ArtifactRoot },
    Tunnel(TunnelVision),
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Dashboard,
    Jobs,
}

impl App {
    fn new(backend: Backend) -> Result<Self> {
        let mut app = Self {
            backend,
            dashboard: None,
            tunnel: None,
            jobs: Vec::new(),
            job_sources: Vec::new(),
            detail: None,
            detail_source: None,
            detail_error: None,
            artifact_index: 0,
            artifact_preview: None,
            artifact_error: None,
            selected: 0,
            filter: String::new(),
            state: JobState::All,
            group_by: GroupBy::Command,
            include_noise: false,
            view: View::Dashboard,
            editing: false,
        };
        app.refresh()?;
        Ok(app)
    }

    fn source_id_at(&self, index: usize) -> &str {
        self.job_sources
            .get(index)
            .and_then(Option::as_ref)
            .map(|source| source.id.as_str())
            .unwrap_or("local")
    }

    fn selected_key(&self) -> Option<(String, i64)> {
        self.jobs
            .get(self.selected)
            .map(|job| (self.source_id_at(self.selected).to_owned(), job.job))
    }

    fn refresh(&mut self) -> Result<()> {
        let selected_job = self.selected_key();
        let filter = JobFilter {
            query: (!self.filter.is_empty()).then(|| self.filter.clone()),
            state: self.state,
            hide_noise: !self.include_noise,
            ..JobFilter::default()
        };
        match &mut self.backend {
            Backend::Local { db, .. } => {
                self.dashboard = Some(db.dashboard(&filter, self.group_by, 30)?);
                self.tunnel = None;
                self.jobs = db.filtered_jobs(&filter, 1_000)?;
                self.job_sources = vec![None; self.jobs.len()];
            }
            Backend::Tunnel(tunnel) => {
                self.dashboard = None;
                let snapshot = tunnel.snapshot(&filter, 1_000);
                self.job_sources = snapshot
                    .jobs
                    .iter()
                    .map(|job| Some(job.source.clone()))
                    .collect();
                self.jobs = snapshot
                    .jobs
                    .iter()
                    .map(|job| job.summary.clone())
                    .collect();
                self.tunnel = Some(snapshot);
            }
        }
        self.selected = selected_job
            .as_ref()
            .and_then(|(source_id, id)| {
                self.jobs.iter().enumerate().position(|(index, job)| {
                    job.job == *id && self.source_id_at(index) == source_id
                })
            })
            .unwrap_or(0)
            .min(self.jobs.len().saturating_sub(1));
        if selected_job != self.selected_key() {
            self.artifact_index = 0;
            self.artifact_preview = None;
            self.artifact_error = None;
        }
        self.load_detail()
    }

    fn load_detail(&mut self) -> Result<()> {
        let job_id = self.jobs.get(self.selected).map(|job| job.job);
        let source = self
            .job_sources
            .get(self.selected)
            .and_then(Option::as_ref)
            .cloned();
        match (&mut self.backend, job_id, source) {
            (Backend::Local { db, .. }, Some(job_id), _) => {
                self.detail = db.job(job_id)?;
                self.detail_source = None;
                self.detail_error = None;
            }
            (Backend::Tunnel(tunnel), Some(job_id), Some(source)) => {
                match tunnel.job(&source.id, job_id) {
                    Ok(result) => {
                        self.detail = result.as_ref().map(|job| job.detail.clone());
                        self.detail_source = result.map(|job| job.source);
                        self.detail_error = None;
                    }
                    Err(error) => {
                        self.detail = None;
                        self.detail_source = Some(source);
                        self.detail_error = Some(error.to_string());
                    }
                }
            }
            _ => {
                self.detail = None;
                self.detail_source = None;
                self.detail_error = None;
            }
        }
        if let Some(detail) = &self.detail {
            self.artifact_index = self
                .artifact_index
                .min(detail.summary.artifacts.len().saturating_sub(1));
        }
        Ok(())
    }

    fn move_selection(&mut self, delta: isize) -> Result<()> {
        if self.jobs.is_empty() {
            return Ok(());
        }
        self.selected =
            (self.selected as isize + delta).clamp(0, self.jobs.len() as isize - 1) as usize;
        self.artifact_index = 0;
        self.artifact_preview = None;
        self.artifact_error = None;
        self.load_detail()
    }

    fn cycle_artifact(&mut self, delta: isize) {
        let count = self
            .detail
            .as_ref()
            .map(|detail| detail.summary.artifacts.len())
            .unwrap_or_default();
        if count == 0 {
            return;
        }
        self.artifact_index =
            (self.artifact_index as isize + delta).rem_euclid(count as isize) as usize;
        self.artifact_preview = None;
        self.artifact_error = None;
    }

    fn inspect_artifact(&mut self) {
        let Some(path) = self.detail.as_ref().and_then(|detail| {
            detail
                .summary
                .artifacts
                .get(self.artifact_index)
                .map(|artifact| artifact.path.clone())
        }) else {
            self.artifact_error = Some("selected job has no output artifacts".to_owned());
            return;
        };
        let result = match &mut self.backend {
            Backend::Local { artifacts, .. } => {
                artifacts.inspect("local", &path, 0, DEFAULT_READ_LIMIT)
            }
            Backend::Tunnel(tunnel) => {
                let source_id = self
                    .detail_source
                    .as_ref()
                    .map(|source| source.id.as_str())
                    .unwrap_or("local");
                tunnel.inspect(source_id, &path, 0, DEFAULT_READ_LIMIT)
            }
        };
        match result {
            Ok(artifact) => {
                self.artifact_preview = Some(artifact);
                self.artifact_error = None;
            }
            Err(error) => {
                self.artifact_preview = None;
                self.artifact_error = Some(error.to_string());
            }
        }
    }

    fn cycle_state(&mut self) -> Result<()> {
        self.state = match self.state {
            JobState::All => JobState::Failed,
            JobState::Failed => JobState::Passed,
            JobState::Passed => JobState::Running,
            JobState::Running => JobState::All,
        };
        self.refresh()
    }

    fn cycle_group(&mut self) -> Result<()> {
        self.group_by = match self.group_by {
            GroupBy::Command => GroupBy::Label,
            GroupBy::Label => GroupBy::Status,
            GroupBy::Status => GroupBy::Artifact,
            GroupBy::Artifact => GroupBy::Run,
            GroupBy::Run => GroupBy::Command,
        };
        self.refresh()
    }

    fn toggle_noise(&mut self) -> Result<()> {
        self.include_noise = !self.include_noise;
        self.refresh()
    }
}

fn draw(frame: &mut ratatui::Frame, app: &App) {
    let vertical = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(frame.area());
    let state_name = match app.state {
        JobState::All => "all",
        JobState::Failed => "failed",
        JobState::Passed => "passed",
        JobState::Running => "running",
    };
    let filter_style = if app.editing {
        Style::default()
            .fg(Color::Yellow)
            .add_modifier(Modifier::BOLD)
    } else {
        Style::default()
    };
    let view_name = if app.view == View::Dashboard {
        "Dashboard"
    } else {
        "Jobs"
    };
    let noise_name = if app.include_noise { "shown" } else { "hidden" };
    let tunnel_label = app
        .tunnel
        .as_ref()
        .map(|snapshot| {
            format!(
                "Tunnel Vision {} · {} sources · ",
                snapshot.triage_id,
                snapshot.sources.len()
            )
        })
        .unwrap_or_default();
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled(
                if app.tunnel.is_some() {
                    " Wake Tunnel Vision "
                } else {
                    " Wake TUI "
                },
                Style::default()
                    .fg(Color::Black)
                    .bg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::raw(format!(
                "  {tunnel_label}{view_name} · jobs: {} · state: {state_name} · noise: {noise_name} · search: ",
                app.jobs.len(),
            )),
            Span::styled(
                if app.filter.is_empty() {
                    "(none)"
                } else {
                    &app.filter
                },
                filter_style,
            ),
        ]))
        .block(
            Block::default()
                .borders(Borders::ALL)
                .title(if app.tunnel.is_some() {
                    "Federated artifact triage"
                } else {
                    "Artifact triage"
                }),
        ),
        vertical[0],
    );

    if app.view == View::Dashboard {
        if app.tunnel.is_some() {
            draw_tunnel_dashboard(frame, app, vertical[1]);
        } else {
            draw_dashboard(frame, app, vertical[1]);
        }
    } else {
        draw_jobs(frame, app, vertical[1]);
    }
    let help = if app.view == View::Dashboard {
        " d jobs · t triage failures · g grouping · n noise · / search · f state · r refresh · q quit "
    } else {
        " ↑/↓ or j/k job · [/ ] artifact · a preview · d dashboard · / search · f state · r refresh · q quit "
    };
    frame.render_widget(
        Paragraph::new(help).style(Style::default().fg(Color::DarkGray)),
        vertical[2],
    );
}

fn draw_tunnel_dashboard(frame: &mut ratatui::Frame, app: &App, area: ratatui::layout::Rect) {
    let Some(snapshot) = &app.tunnel else {
        return;
    };
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(5), Constraint::Min(1)])
        .split(area);
    let failed = snapshot
        .jobs
        .iter()
        .filter(|job| matches!(job.summary.status, Some(status) if status != 0))
        .count();
    let running = snapshot
        .jobs
        .iter()
        .filter(|job| job.summary.status.is_none())
        .count();
    let unavailable = snapshot
        .sources
        .iter()
        .filter(|source| source.error.is_some())
        .count();
    frame.render_widget(
        Paragraph::new(vec![
            Line::from(vec![
                Span::styled(
                    format!(" {:>5} failed ", failed),
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                ),
                Span::styled(
                    format!(" {:>5} running ", running),
                    Style::default().fg(Color::Yellow),
                ),
                Span::raw(format!(" {:>5} jobs ", snapshot.jobs.len())),
                Span::styled(
                    format!(" {:>4}× peak parallelism ", snapshot.peak_parallelism),
                    Style::default().fg(Color::Cyan),
                ),
            ]),
            Line::raw(format!(
                " triage {} · {} source(s) unavailable · source-qualified IDs and wake:// artifact paths",
                snapshot.triage_id, unavailable
            )),
        ])
        .block(Block::default().borders(Borders::ALL).title("Triage overview")),
        rows[0],
    );

    let columns = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(55), Constraint::Percentage(45)])
        .split(rows[1]);
    let sources = snapshot
        .sources
        .iter()
        .map(|source| {
            let Some(info) = &source.source else {
                return ListItem::new("unknown source");
            };
            let status = source
                .error
                .as_ref()
                .map(|error| format!("unavailable: {}", truncate(error, 38)))
                .unwrap_or_else(|| {
                    format!(
                        "{} jobs · {} runs · {} fail · {} running",
                        source.jobs, source.runs, source.failed, source.running
                    )
                });
            ListItem::new(vec![
                Line::from(vec![
                    Span::styled(
                        format!("{:<18}", truncate(&info.label, 18)),
                        Style::default().fg(if source.error.is_some() {
                            Color::Red
                        } else {
                            Color::Cyan
                        }),
                    ),
                    Span::raw(format!(" {}@{}", info.runner, info.host)),
                ]),
                Line::raw(format!("  {status}")),
            ])
        })
        .collect::<Vec<_>>();
    frame.render_widget(
        List::new(sources).block(
            Block::default()
                .borders(Borders::ALL)
                .title("Execution lanes · local / Slurm / Ray"),
        ),
        columns[0],
    );

    let triage = snapshot
        .jobs
        .iter()
        .filter(|job| {
            job.summary.status.is_none()
                || matches!(job.summary.status, Some(status) if status != 0)
        })
        .take(50)
        .map(|job| {
            let (word, color) = match job.summary.status {
                None => ("run ", Color::Yellow),
                Some(_) => ("fail", Color::Red),
            };
            ListItem::new(Line::from(vec![
                Span::styled(format!("{word} "), Style::default().fg(color)),
                Span::styled(
                    format!("{}#{} ", job.source.id, job.summary.job),
                    Style::default().fg(Color::Cyan),
                ),
                Span::raw(truncate(&job.summary.label, 30)),
            ]))
        })
        .collect::<Vec<_>>();
    frame.render_widget(
        List::new(triage).block(
            Block::default()
                .borders(Borders::ALL)
                .title("Parallel triage queue · failures and active jobs"),
        ),
        columns[1],
    );
}

fn draw_dashboard(frame: &mut ratatui::Frame, app: &App, area: ratatui::layout::Rect) {
    let Some(dashboard) = &app.dashboard else {
        frame.render_widget(Paragraph::new("Loading dashboard…"), area);
        return;
    };
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(6), Constraint::Min(1)])
        .split(area);
    let metric = &dashboard.metrics;
    frame.render_widget(
        Paragraph::new(vec![
            Line::from(vec![
                Span::styled(
                    format!(" {:>5} failed ", metric.failed),
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                ),
                Span::styled(
                    format!(" {:>5} passed ", metric.passed),
                    Style::default().fg(Color::Green),
                ),
                Span::styled(
                    format!(" {:>5} running ", metric.running),
                    Style::default().fg(Color::Yellow),
                ),
                Span::raw(format!(" {:>5} artifacts ", metric.artifacts)),
                Span::raw(format!(" {:>5} fanouts ", metric.fanout_edges)),
            ]),
            Line::raw(format!(
                " runtime {:.2}s · CPU {:.2}s · I/O {} · peak memory {} · {} noise jobs hidden",
                metric.total_runtime,
                metric.total_cputime,
                format_bytes(metric.io_bytes),
                format_bytes(metric.peak_memory_bytes),
                metric.hidden_noise,
            )),
        ])
        .block(Block::default().borders(Borders::ALL).title("Overview")),
        rows[0],
    );
    let columns = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(52), Constraint::Percentage(48)])
        .split(rows[1]);
    let groups = dashboard
        .groups
        .iter()
        .map(|group| {
            ListItem::new(Line::from(vec![
                Span::styled(
                    format!("{:<24}", truncate(&group.key, 24)),
                    Style::default().fg(Color::Cyan),
                ),
                Span::raw(format!(
                    " {:>4} jobs  {:>3} fail  {:>7.2}s",
                    group.jobs, group.failed, group.runtime
                )),
            ]))
        })
        .collect::<Vec<_>>();
    frame.render_widget(
        List::new(groups).block(
            Block::default()
                .borders(Borders::ALL)
                .title(format!("Groups · {}", dashboard.group_by)),
        ),
        columns[0],
    );
    let right = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Percentage(45), Constraint::Percentage(55)])
        .split(columns[1]);
    let failures = dashboard
        .failures
        .iter()
        .map(|job| {
            ListItem::new(Line::from(vec![
                Span::styled(format!("#{:<7}", job.job), Style::default().fg(Color::Red)),
                Span::raw(truncate(&job.label, 34)),
                Span::raw(format!("  {:.2}s", job.runtime.unwrap_or_default())),
            ]))
        })
        .collect::<Vec<_>>();
    frame.render_widget(
        List::new(failures).block(
            Block::default()
                .borders(Borders::ALL)
                .title("Triage queue · newest failures"),
        ),
        right[0],
    );
    let fanouts = dashboard
        .fanouts
        .iter()
        .map(|fanout| {
            ListItem::new(Line::from(vec![
                Span::styled(
                    format!("{:>3}× ", fanout.consumers.len()),
                    Style::default().fg(Color::Yellow),
                ),
                Span::raw(truncate(&fanout.artifact, 42)),
                Span::styled(
                    format!("  #{}", fanout.producer_job),
                    Style::default().fg(Color::DarkGray),
                ),
            ]))
        })
        .collect::<Vec<_>>();
    frame.render_widget(
        List::new(fanouts).block(
            Block::default()
                .borders(Borders::ALL)
                .title("High-fanout artifacts"),
        ),
        right[1],
    );
}

fn draw_jobs(frame: &mut ratatui::Frame, app: &App, area: ratatui::layout::Rect) {
    let columns = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(43), Constraint::Percentage(57)])
        .split(area);

    let items: Vec<ListItem> = app
        .jobs
        .iter()
        .enumerate()
        .map(|(index, job)| {
            let status = match job.status {
                Some(0) => Span::styled("pass", Style::default().fg(Color::Green)),
                Some(_) => Span::styled("fail", Style::default().fg(Color::Red)),
                None => Span::styled("run ", Style::default().fg(Color::Yellow)),
            };
            let source = app
                .job_sources
                .get(index)
                .and_then(Option::as_ref)
                .map(|source| format!("{}#", source.id))
                .unwrap_or_else(|| "#".to_owned());
            ListItem::new(Line::from(vec![
                Span::styled(
                    format!("{source}{:<7} ", job.job),
                    Style::default().fg(Color::Cyan),
                ),
                status,
                Span::raw(format!("  {} [{}]", job.label, job.artifacts.len())),
            ]))
        })
        .collect();
    let mut list_state =
        ListState::default().with_selected((!app.jobs.is_empty()).then_some(app.selected));
    frame.render_stateful_widget(
        List::new(items)
            .block(Block::default().borders(Borders::ALL).title("Jobs"))
            .highlight_symbol("▶ ")
            .highlight_style(
                Style::default()
                    .bg(Color::DarkGray)
                    .add_modifier(Modifier::BOLD),
            ),
        columns[0],
        &mut list_state,
    );

    let detail = if let Some(job) = &app.detail {
        let mut lines = Vec::new();
        if let Some(source) = &app.detail_source {
            lines.push(Line::from(vec![
                Span::styled(
                    format!("{}#{}", source.id, job.summary.job),
                    Style::default()
                        .fg(Color::Cyan)
                        .add_modifier(Modifier::BOLD),
                ),
                Span::raw(format!(
                    " · {} via {}@{} · run #{}",
                    source.label, source.runner, source.host, job.summary.run
                )),
            ]));
        }
        lines.extend(vec![
            Line::styled(
                &job.summary.label,
                Style::default()
                    .add_modifier(Modifier::BOLD)
                    .fg(Color::Cyan),
            ),
            Line::raw(format!(
                "job #{} · run #{} · status {:?} · {:.3}s · CPU {:.3}s",
                job.summary.job,
                job.summary.run,
                job.summary.status,
                job.summary.runtime.unwrap_or_default(),
                job.summary.cputime.unwrap_or_default(),
            )),
            Line::raw(""),
            Line::styled("Command", Style::default().fg(Color::Yellow)),
            Line::raw(job.summary.commandline.join(" ")),
            Line::raw(""),
            Line::styled(
                format!("Artifacts ({})", job.summary.artifacts.len()),
                Style::default().fg(Color::Yellow),
            ),
        ]);
        lines.extend(
            job.summary
                .artifacts
                .iter()
                .enumerate()
                .map(|(index, artifact)| {
                    let marker = if index == app.artifact_index {
                        "▶"
                    } else {
                        " "
                    };
                    let uri = app
                        .detail_source
                        .as_ref()
                        .map(|source| format!("wake://{}/{}", source.id, artifact.path))
                        .unwrap_or_else(|| artifact.path.clone());
                    Line::raw(format!(" {marker} {uri} ({})", artifact.kind))
                }),
        );
        if let Some(error) = &app.artifact_error {
            lines.push(Line::styled(
                format!("  preview error: {error}"),
                Style::default().fg(Color::Red),
            ));
        }
        if let Some(preview) = &app.artifact_preview {
            lines.push(Line::raw(""));
            lines.push(Line::styled(
                format!(
                    "Read-only preview · {} · {} · {}{}",
                    preview.uri,
                    preview.kind,
                    format_bytes(preview.size.min(i64::MAX as u64) as i64),
                    if preview.truncated {
                        " · truncated"
                    } else {
                        ""
                    }
                ),
                Style::default().fg(Color::Magenta),
            ));
            if let Some(content) = &preview.content {
                lines.extend(content.lines().take(200).map(Line::raw));
            } else {
                lines.extend(preview.entries.iter().take(200).map(|entry| {
                    Line::raw(format!(
                        "  {:<10} {:>10}  {}",
                        entry.kind,
                        format_bytes(entry.size.min(i64::MAX as u64) as i64),
                        entry.name
                    ))
                }));
            }
        }
        lines.push(Line::raw(""));
        lines.push(Line::styled(
            format!("Inputs ({})", job.inputs.len()),
            Style::default().fg(Color::Yellow),
        ));
        lines.extend(
            job.inputs
                .iter()
                .take(50)
                .map(|artifact| Line::raw(format!("  {} ({})", artifact.path, artifact.kind))),
        );
        lines.push(Line::raw(""));
        lines.push(Line::styled(
            format!("Downstream consumers ({})", job.fanouts.len()),
            Style::default().fg(Color::Yellow),
        ));
        lines.extend(job.fanouts.iter().take(50).map(|consumer| {
            Line::raw(format!(
                "  #{} {} ← {}",
                consumer.job, consumer.label, consumer.artifact
            ))
        }));
        lines.push(Line::raw(""));
        lines.push(Line::styled(
            "Standard output",
            Style::default().fg(Color::Yellow),
        ));
        lines.extend(job.stdout.lines().map(Line::raw));
        lines.push(Line::raw(""));
        lines.push(Line::styled(
            "Standard error",
            Style::default().fg(Color::Yellow),
        ));
        lines.extend(job.stderr.lines().map(Line::raw));
        lines
    } else if let Some(error) = &app.detail_error {
        vec![
            Line::styled("Job detail unavailable", Style::default().fg(Color::Red)),
            Line::raw(error),
            Line::raw(""),
            Line::raw("The source will be retried on the next refresh."),
        ]
    } else {
        vec![Line::raw("No jobs match the current filter.")]
    };
    frame.render_widget(
        Paragraph::new(detail)
            .block(Block::default().borders(Borders::ALL).title("Details"))
            .wrap(Wrap { trim: false }),
        columns[1],
    );
}

fn truncate(value: &str, width: usize) -> String {
    let count = value.chars().count();
    if count <= width {
        return value.to_owned();
    }
    value
        .chars()
        .take(width.saturating_sub(1))
        .collect::<String>()
        + "…"
}

fn format_bytes(value: i64) -> String {
    let value = value.max(0) as f64;
    if value >= 1024.0 * 1024.0 * 1024.0 {
        format!("{:.1} GiB", value / (1024.0 * 1024.0 * 1024.0))
    } else if value >= 1024.0 * 1024.0 {
        format!("{:.1} MiB", value / (1024.0 * 1024.0))
    } else if value >= 1024.0 {
        format!("{:.1} KiB", value / 1024.0)
    } else {
        format!("{} B", value as i64)
    }
}

pub fn run() -> Result<()> {
    if !io::stdin().is_terminal() || !io::stdout().is_terminal() {
        return Err(anyhow!("wake tui requires an interactive terminal"));
    }
    let options = Options::parse();
    let backend = if options.tunnel_vision || options.tunnel_config.is_some() {
        Backend::Tunnel(TunnelVision::discover(options.tunnel_config)?)
    } else {
        let db = WakeDb::discover(options.database)?;
        let root = db
            .path()
            .parent()
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("."));
        Backend::Local {
            db,
            artifacts: ArtifactRoot::new(root)?,
        }
    };
    let mut app = App::new(backend)?;
    let _guard = TerminalGuard::enter()?;
    let backend = CrosstermBackend::new(io::stdout());
    let mut terminal = Terminal::new(backend)?;
    terminal.clear()?;
    let mut last_refresh = Instant::now();

    loop {
        terminal.draw(|frame| draw(frame, &app))?;
        if event::poll(Duration::from_millis(200))? {
            if let Event::Key(key) = event::read()? {
                if key.kind != KeyEventKind::Press {
                    continue;
                }
                if app.editing {
                    match key.code {
                        KeyCode::Esc | KeyCode::Enter => app.editing = false,
                        KeyCode::Backspace => {
                            app.filter.pop();
                            app.refresh()?;
                        }
                        KeyCode::Char(character) => {
                            app.filter.push(character);
                            app.refresh()?;
                        }
                        _ => {}
                    }
                } else {
                    match key.code {
                        KeyCode::Char('q') => break,
                        KeyCode::Char('/') => app.editing = true,
                        KeyCode::Char('d') => {
                            app.view = if app.view == View::Dashboard {
                                View::Jobs
                            } else {
                                View::Dashboard
                            };
                        }
                        KeyCode::Char('t') => {
                            app.state = JobState::Failed;
                            app.view = View::Jobs;
                            app.refresh()?;
                        }
                        KeyCode::Char('g') if app.view == View::Dashboard => app.cycle_group()?,
                        KeyCode::Char('n') => app.toggle_noise()?,
                        KeyCode::Char('f') => app.cycle_state()?,
                        KeyCode::Char('r') => app.refresh()?,
                        KeyCode::Char('[') => app.cycle_artifact(-1),
                        KeyCode::Char(']') => app.cycle_artifact(1),
                        KeyCode::Char('a') | KeyCode::Enter if app.view == View::Jobs => {
                            app.inspect_artifact()
                        }
                        KeyCode::Down | KeyCode::Char('j') => app.move_selection(1)?,
                        KeyCode::Up | KeyCode::Char('k') => app.move_selection(-1)?,
                        _ => {}
                    }
                }
            }
        }
        if last_refresh.elapsed() >= Duration::from_secs(2) {
            app.refresh()?;
            last_refresh = Instant::now();
        }
    }
    Ok(())
}
