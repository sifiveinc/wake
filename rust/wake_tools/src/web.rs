use crate::db::{JobState, WakeDb};
use anyhow::{Context, Result};
use axum::extract::{Path, Query, Request, State};
use axum::http::{header, HeaderValue, StatusCode};
use axum::response::{Html, IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use serde::Deserialize;
use serde_json::json;
use std::net::SocketAddr;

const INDEX: &str = include_str!("ui.html");

#[derive(Debug, Deserialize)]
struct JobQuery {
    q: Option<String>,
    state: Option<String>,
    limit: Option<usize>,
}

fn error_response(error: anyhow::Error) -> Response {
    (
        StatusCode::INTERNAL_SERVER_ERROR,
        Json(json!({ "error": error.to_string() })),
    )
        .into_response()
}

async fn response_headers(request: Request, next: axum::middleware::Next) -> Response {
    let mut response = next.run(request).await;
    let headers = response.headers_mut();
    headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    headers.insert(
        header::X_CONTENT_TYPE_OPTIONS,
        HeaderValue::from_static("nosniff"),
    );
    headers.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static("default-src 'self' 'unsafe-inline'"),
    );
    response
}

async fn index() -> Html<&'static str> {
    Html(INDEX)
}

async fn health() -> &'static str {
    "ok\n"
}

async fn jobs(State(db): State<WakeDb>, Query(query): Query<JobQuery>) -> Response {
    let state = match query.state.as_deref() {
        Some("failed") => JobState::Failed,
        Some("passed") => JobState::Passed,
        _ => JobState::All,
    };
    match tokio::task::spawn_blocking(move || {
        db.jobs(query.q.as_deref(), state, query.limit.unwrap_or(500))
    })
    .await
    {
        Ok(Ok(jobs)) => Json(jobs).into_response(),
        Ok(Err(error)) => error_response(error),
        Err(error) => error_response(error.into()),
    }
}

async fn job(State(db): State<WakeDb>, Path(job_id): Path<i64>) -> Response {
    match tokio::task::spawn_blocking(move || db.job(job_id)).await {
        Ok(Ok(Some(job))) => Json(job).into_response(),
        Ok(Ok(None)) => (
            StatusCode::NOT_FOUND,
            Json(json!({ "error": "job not found" })),
        )
            .into_response(),
        Ok(Err(error)) => error_response(error),
        Err(error) => error_response(error.into()),
    }
}

async fn runs(State(db): State<WakeDb>) -> Response {
    match tokio::task::spawn_blocking(move || db.runs(200)).await {
        Ok(Ok(runs)) => Json(runs).into_response(),
        Ok(Err(error)) => error_response(error),
        Err(error) => error_response(error.into()),
    }
}

pub async fn serve(db: WakeDb, address: SocketAddr) -> Result<()> {
    let app = Router::new()
        .route("/", get(index))
        .route("/index.html", get(index))
        .route("/healthz", get(health))
        .route("/api/jobs", get(jobs))
        .route("/api/jobs/{job_id}", get(job))
        .route("/api/runs", get(runs))
        .with_state(db)
        .layer(axum::middleware::from_fn(response_headers));
    let listener = tokio::net::TcpListener::bind(address)
        .await
        .with_context(|| format!("binding Wake UI to {address}"))?;
    println!("Wake UI listening on http://{address}");
    if !address.ip().is_loopback() {
        eprintln!("warning: Wake UI is remotely accessible and has no authentication");
    }
    axum::serve(listener, app)
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await
        .context("serving Wake UI")?;
    Ok(())
}
