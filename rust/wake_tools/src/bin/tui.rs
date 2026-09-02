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
use wake_tools::db::{Dashboard, GroupBy, JobDetail, JobFilter, JobState, JobSummary, WakeDb};

#[derive(Debug, Parser)]
#[command(
    name = "wake-tui",
    about = "Interactively triage Wake jobs and artifacts"
)]
struct Options {
    #[arg(long)]
    database: Option<PathBuf>,
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
    db: WakeDb,
    dashboard: Option<Dashboard>,
    jobs: Vec<JobSummary>,
    detail: Option<JobDetail>,
    selected: usize,
    filter: String,
    state: JobState,
    group_by: GroupBy,
    include_noise: bool,
    view: View,
    editing: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Dashboard,
    Jobs,
}

impl App {
    fn new(db: WakeDb) -> Result<Self> {
        let mut app = Self {
            db,
            dashboard: None,
            jobs: Vec::new(),
            detail: None,
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

    fn refresh(&mut self) -> Result<()> {
        let selected_job = self.jobs.get(self.selected).map(|job| job.job);
        let filter = JobFilter {
            query: (!self.filter.is_empty()).then(|| self.filter.clone()),
            state: self.state,
            hide_noise: !self.include_noise,
            ..JobFilter::default()
        };
        self.dashboard = Some(self.db.dashboard(&filter, self.group_by, 30)?);
        self.jobs = self.db.filtered_jobs(&filter, 1_000)?;
        self.selected = selected_job
            .and_then(|id| self.jobs.iter().position(|job| job.job == id))
            .unwrap_or(0)
            .min(self.jobs.len().saturating_sub(1));
        self.load_detail()
    }

    fn load_detail(&mut self) -> Result<()> {
        self.detail = match self.jobs.get(self.selected) {
            Some(job) => self.db.job(job.job)?,
            None => None,
        };
        Ok(())
    }

    fn move_selection(&mut self, delta: isize) -> Result<()> {
        if self.jobs.is_empty() {
            return Ok(());
        }
        self.selected =
            (self.selected as isize + delta).clamp(0, self.jobs.len() as isize - 1) as usize;
        self.load_detail()
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
    frame.render_widget(
        Paragraph::new(Line::from(vec![
            Span::styled(
                " Wake TUI ",
                Style::default()
                    .fg(Color::Black)
                    .bg(Color::Cyan)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::raw(format!(
                "  {view_name} · jobs: {} · state: {state_name} · noise: {noise_name} · search: ",
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
                .title("Artifact triage"),
        ),
        vertical[0],
    );

    if app.view == View::Dashboard {
        draw_dashboard(frame, app, vertical[1]);
    } else {
        draw_jobs(frame, app, vertical[1]);
    }
    let help = if app.view == View::Dashboard {
        " d jobs · t triage failures · g grouping · n noise · / search · f state · r refresh · q quit "
    } else {
        " ↑/↓ or j/k select · d dashboard · / search · f state · n noise · r refresh · q quit "
    };
    frame.render_widget(
        Paragraph::new(help).style(Style::default().fg(Color::DarkGray)),
        vertical[2],
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
        .map(|job| {
            let status = match job.status {
                Some(0) => Span::styled("pass", Style::default().fg(Color::Green)),
                Some(_) => Span::styled("fail", Style::default().fg(Color::Red)),
                None => Span::styled("run ", Style::default().fg(Color::Yellow)),
            };
            ListItem::new(Line::from(vec![
                Span::raw(format!("#{:<7} ", job.job)),
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
        let mut lines = vec![
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
        ];
        lines.extend(
            job.summary
                .artifacts
                .iter()
                .map(|artifact| Line::raw(format!("  {} ({})", artifact.path, artifact.kind))),
        );
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
    let mut app = App::new(WakeDb::discover(options.database)?)?;
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
