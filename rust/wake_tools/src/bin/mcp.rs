use anyhow::Result;
use clap::Parser;
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::path::PathBuf;
use wake_tools::{
    artifact::ArtifactRoot,
    db::WakeDb,
    mcp::{self, WakeService},
};

#[derive(Debug, Parser)]
#[command(name = "wake-mcp", about = "Expose Wake build data over MCP stdio")]
struct Options {
    #[arg(long)]
    database: Option<PathBuf>,
    #[arg(long)]
    artifact_root: Option<PathBuf>,
    #[arg(long, default_value = "local")]
    source_id: String,
}

pub fn run() -> Result<()> {
    let options = Options::parse();
    let db = WakeDb::discover(options.database)?;
    let artifact_root = options
        .artifact_root
        .or_else(|| db.path().parent().map(PathBuf::from))
        .unwrap_or_else(|| PathBuf::from("."));
    let service = WakeService {
        db,
        artifacts: ArtifactRoot::new(artifact_root)?,
        source_id: options.source_id,
    };
    let stdin = io::stdin();
    let mut stdout = io::stdout().lock();
    for line in stdin.lock().lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let response = match serde_json::from_str::<Value>(&line) {
            Ok(request) => mcp::handle(&service, request),
            Err(error) => Some(json!({
                "jsonrpc": "2.0",
                "id": null,
                "error": { "code": -32700, "message": format!("Parse error: {error}") }
            })),
        };
        if let Some(response) = response {
            serde_json::to_writer(&mut stdout, &response)?;
            stdout.write_all(b"\n")?;
            stdout.flush()?;
        }
    }
    Ok(())
}
