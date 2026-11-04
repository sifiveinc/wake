use anyhow::Result;
use clap::Parser;
use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use wake_tools::{db::WakeDb, web};

#[derive(Debug, Parser)]
#[command(name = "wake-ui", about = "Serve Wake's artifact triage web UI")]
struct Options {
    #[arg(long, default_value = "127.0.0.1")]
    address: IpAddr,
    #[arg(long, default_value_t = 8080)]
    port: u16,
    #[arg(long)]
    database: Option<PathBuf>,
}

#[tokio::main]
pub async fn run() -> Result<()> {
    let options = Options::parse();
    let db = WakeDb::discover(options.database)?;
    web::serve(db, SocketAddr::new(options.address, options.port)).await
}
