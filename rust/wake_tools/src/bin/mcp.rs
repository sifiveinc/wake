use anyhow::Result;
use clap::Parser;
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::path::PathBuf;
use wake_tools::{db::WakeDb, mcp};

#[derive(Debug, Parser)]
#[command(name = "wake-mcp", about = "Expose Wake build data over MCP stdio")]
struct Options {
    #[arg(long)]
    database: Option<PathBuf>,
}

pub fn run() -> Result<()> {
    let options = Options::parse();
    let db = WakeDb::discover(options.database)?;
    let stdin = io::stdin();
    let mut stdout = io::stdout().lock();
    for line in stdin.lock().lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let response = match serde_json::from_str::<Value>(&line) {
            Ok(request) => mcp::handle(&db, request),
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
