use anyhow::{anyhow, Result};
use std::path::Path;

#[path = "mcp.rs"]
mod mcp;
#[path = "otel.rs"]
mod otel;
#[path = "tui.rs"]
mod tui;
#[path = "ui.rs"]
mod ui;

fn main() -> Result<()> {
    let executable = std::env::args_os().next().unwrap_or_default();
    match Path::new(&executable)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_default()
    {
        "wake-ui" => ui::run(),
        "wake-tui" => tui::run(),
        "wake-mcp" => mcp::run(),
        "wake-otel" => otel::run(),
        name => Err(anyhow!(
            "{name} must be installed or linked as wake-ui, wake-tui, wake-mcp, or wake-otel"
        )),
    }
}
