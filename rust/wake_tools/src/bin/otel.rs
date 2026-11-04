use anyhow::{anyhow, Result};
use clap::Parser;
use std::path::PathBuf;
use wake_tools::{db::WakeDb, otel};

#[derive(Debug, Parser)]
#[command(name = "wake-otel", about = "Export a completed Wake run over OTLP")]
struct Options {
    #[arg(long)]
    database: Option<PathBuf>,
    #[arg(long)]
    run_id: i64,
    #[arg(long)]
    exit_code: i32,
    #[arg(long)]
    wake_version: String,
}

pub fn run() -> Result<()> {
    let options = Options::parse();
    let db = WakeDb::discover(options.database)?;
    let run = db
        .telemetry_run(options.run_id)?
        .ok_or_else(|| anyhow!("completed Wake run {} was not found", options.run_id))?;
    otel::export_run(&run, options.exit_code, &options.wake_version)
}
