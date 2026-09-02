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
use wake_tools::db::{JobDetail, JobState, JobSummary, WakeDb};

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
    jobs: Vec<JobSummary>,
    detail: Option<JobDetail>,
    selected: usize,
    filter: String,
    state: JobState,
    editing: bool,
}

impl App {
    fn new(db: WakeDb) -> Result<Self> {
        let mut app = Self {
            db,
            jobs: Vec::new(),
            detail: None,
            selected: 0,
            filter: String::new(),
            state: JobState::All,
            editing: false,
        };
        app.refresh()?;
        Ok(app)
    }

    fn refresh(&mut self) -> Result<()> {
        let selected_job = self.jobs.get(self.selected).map(|job| job.job);
        self.jobs = self.db.jobs(
            (!self.filter.is_empty()).then_some(self.filter.as_str()),
            self.state,
            1_000,
        )?;
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
            JobState::Passed => JobState::All,
        };
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
    let columns = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(43), Constraint::Percentage(57)])
        .split(vertical[1]);

    let state_name = match app.state {
        JobState::All => "all",
        JobState::Failed => "failed",
        JobState::Passed => "passed",
    };
    let filter_style = if app.editing {
        Style::default()
            .fg(Color::Yellow)
            .add_modifier(Modifier::BOLD)
    } else {
        Style::default()
    };
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
                "  jobs: {}  state: {state_name}  search: ",
                app.jobs.len()
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
                "job #{} · run #{} · status {:?} · {:.3}s",
                job.summary.job,
                job.summary.run,
                job.summary.status,
                job.summary.runtime.unwrap_or_default()
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
    frame.render_widget(
        Paragraph::new(" ↑/↓ or j/k select · / search · f status · r refresh · q quit ")
            .style(Style::default().fg(Color::DarkGray)),
        vertical[2],
    );
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
