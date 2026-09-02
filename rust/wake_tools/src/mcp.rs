use crate::artifact::{ArtifactRoot, DEFAULT_READ_LIMIT, MAX_READ_LIMIT};
use crate::db::{GroupBy, JobFilter, JobState, WakeDb};
use serde_json::{json, Value};

#[derive(Clone, Debug)]
pub struct WakeService {
    pub db: WakeDb,
    pub artifacts: ArtifactRoot,
    pub source_id: String,
}

const SERVER_INFO_KEY: &str = "io.modelcontextprotocol/serverInfo";
const MODERN_VERSION: &str = "2026-07-28";
const LEGACY_VERSION: &str = "2025-11-25";

fn server_info() -> Value {
    json!({
        "name": "wake-mcp",
        "version": env!("CARGO_PKG_VERSION"),
        "description": "Read-only access to Wake build metrics, grouped jobs, artifacts, fanouts, logs, and runs"
    })
}

fn tools() -> Value {
    json!([
        {
            "name": "get_wake_dashboard",
            "title": "Get Wake Dashboard",
            "description": "Summarize Wake jobs with status and resource metrics, grouped jobs, recent failures, and high-fanout artifacts.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "query": { "type": "string", "description": "Substring in a label, job ID, command, or artifact path" },
                    "state": { "type": "string", "enum": ["all", "failed", "passed", "running"], "default": "all" },
                    "run_id": { "type": "integer", "minimum": 1, "description": "Only jobs used by this run" },
                    "command": { "type": "string", "description": "Substring in the command line" },
                    "artifact": { "type": "string", "description": "Substring in an output artifact path or kind" },
                    "min_runtime": { "type": "number", "minimum": 0 },
                    "include_noise": { "type": "boolean", "default": false },
                    "group_by": { "type": "string", "enum": ["command", "label", "status", "artifact", "run"], "default": "command" },
                    "limit": { "type": "integer", "minimum": 1, "maximum": 200, "default": 20 }
                },
                "additionalProperties": false
            },
            "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
        },
        {
            "name": "get_wake_job",
            "title": "Get Wake Job",
            "description": "Get one Wake job with its command, output artifacts, tags, stdout, and stderr.",
            "inputSchema": {
                "type": "object",
                "properties": { "job_id": { "type": "integer", "description": "Wake job ID" } },
                "required": ["job_id"],
                "additionalProperties": false
            },
            "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
        },
        {
            "name": "inspect_wake_artifact",
            "title": "Inspect Wake Artifact",
            "description": "Read a bounded text window from a recorded output artifact or list a recorded output directory. Paths are checked against wake.db and confined to the configured artifact root.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "path": { "type": "string", "description": "Relative artifact path" },
                    "offset": { "type": "integer", "minimum": 0, "default": 0 },
                    "limit": { "type": "integer", "minimum": 1, "maximum": MAX_READ_LIMIT, "default": DEFAULT_READ_LIMIT }
                },
                "required": ["path"],
                "additionalProperties": false
            },
            "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
        },
        {
            "name": "list_wake_fanouts",
            "title": "List Wake Fanouts",
            "description": "List output artifacts consumed by downstream jobs, ordered by consumer count. Accepts the same producer-job filters as list_wake_jobs.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "query": { "type": "string" },
                    "state": { "type": "string", "enum": ["all", "failed", "passed", "running"], "default": "all" },
                    "run_id": { "type": "integer", "minimum": 1 },
                    "command": { "type": "string" },
                    "artifact": { "type": "string", "description": "Substring in an output artifact path or kind" },
                    "min_runtime": { "type": "number", "minimum": 0 },
                    "include_noise": { "type": "boolean", "default": false },
                    "limit": { "type": "integer", "minimum": 1, "maximum": 200, "default": 50 }
                },
                "additionalProperties": false
            },
            "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
        },
        {
            "name": "list_wake_jobs",
            "title": "List Wake Jobs",
            "description": "List recent Wake jobs with composable filters for labels, commands, output artifacts, runs, status, and runtime.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "query": { "type": "string", "description": "Substring in a label, job ID, command, or artifact path" },
                    "state": { "type": "string", "enum": ["all", "failed", "passed", "running"], "default": "all" },
                    "run_id": { "type": "integer", "minimum": 1, "description": "Only jobs used by this run" },
                    "command": { "type": "string", "description": "Substring in the command line" },
                    "artifact": { "type": "string", "description": "Substring in an output artifact path or kind" },
                    "min_runtime": { "type": "number", "minimum": 0 },
                    "include_noise": { "type": "boolean", "default": false },
                    "offset": { "type": "integer", "minimum": 0, "maximum": 100000, "default": 0 },
                    "limit": { "type": "integer", "minimum": 1, "maximum": 200, "default": 50 }
                },
                "additionalProperties": false
            },
            "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
        },
        {
            "name": "list_wake_runs",
            "title": "List Wake Runs",
            "description": "List recent Wake invocations and whether each run has completed.",
            "inputSchema": {
                "type": "object",
                "properties": { "limit": { "type": "integer", "minimum": 1, "maximum": 200, "default": 50 } },
                "additionalProperties": false
            },
            "annotations": { "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
        }
    ])
}

fn modern_request(request: &Value) -> bool {
    request.get("method").and_then(Value::as_str) == Some("server/discover")
        || request
            .pointer("/params/_meta/io.modelcontextprotocol~1protocolVersion")
            .and_then(Value::as_str)
            == Some(MODERN_VERSION)
}

fn success(id: Value, mut result: Value, modern: bool) -> Value {
    if modern {
        if let Some(object) = result.as_object_mut() {
            object.entry("_meta").or_insert_with(|| json!({}))[SERVER_INFO_KEY] = server_info();
        }
    }
    json!({ "jsonrpc": "2.0", "id": id, "result": result })
}

fn error(id: Value, code: i64, message: impl Into<String>) -> Value {
    json!({ "jsonrpc": "2.0", "id": id, "error": { "code": code, "message": message.into() } })
}

fn tool_result(value: Value) -> Value {
    let text = serde_json::to_string_pretty(&value).unwrap_or_else(|_| "{}".to_owned());
    json!({
        "content": [{ "type": "text", "text": text }],
        "structuredContent": value,
        "isError": false
    })
}

fn tool_error(message: impl Into<String>) -> Value {
    json!({
        "content": [{ "type": "text", "text": message.into() }],
        "isError": true
    })
}

fn job_filter(arguments: &Value) -> Result<JobFilter, String> {
    let state = match arguments
        .get("state")
        .and_then(Value::as_str)
        .unwrap_or("all")
    {
        "all" => JobState::All,
        "failed" => JobState::Failed,
        "passed" => JobState::Passed,
        "running" => JobState::Running,
        _ => return Err("state must be all, failed, passed, or running".to_owned()),
    };
    let run = match arguments.get("run_id") {
        Some(value) => Some(
            value
                .as_i64()
                .ok_or_else(|| "run_id must be an integer".to_owned())?,
        ),
        None => None,
    };
    if matches!(run, Some(value) if value < 1) {
        return Err("run_id must be positive".to_owned());
    }
    let min_runtime = match arguments.get("min_runtime") {
        Some(value) => Some(
            value
                .as_f64()
                .ok_or_else(|| "min_runtime must be a number".to_owned())?,
        ),
        None => None,
    };
    if matches!(min_runtime, Some(value) if !value.is_finite() || value < 0.0) {
        return Err("min_runtime must be non-negative".to_owned());
    }
    let include_noise = match arguments.get("include_noise") {
        Some(value) => value
            .as_bool()
            .ok_or_else(|| "include_noise must be a boolean".to_owned())?,
        None => false,
    };
    Ok(JobFilter {
        query: arguments
            .get("query")
            .and_then(Value::as_str)
            .map(str::to_owned),
        state,
        run,
        command: arguments
            .get("command")
            .and_then(Value::as_str)
            .map(str::to_owned),
        artifact: arguments
            .get("artifact")
            .and_then(Value::as_str)
            .map(str::to_owned),
        min_runtime,
        hide_noise: !include_noise,
        noise_regex: None,
    })
}

fn tool_call(service: &WakeService, params: &Value) -> Value {
    let Some(name) = params.get("name").and_then(Value::as_str) else {
        return tool_error("missing tool name");
    };
    let arguments = params
        .get("arguments")
        .cloned()
        .unwrap_or_else(|| json!({}));
    match name {
        "get_wake_dashboard" => {
            let filter = match job_filter(&arguments) {
                Ok(filter) => filter,
                Err(error) => return tool_error(error),
            };
            let group = match arguments
                .get("group_by")
                .and_then(Value::as_str)
                .unwrap_or("command")
            {
                value if GroupBy::parse(value).is_some() => GroupBy::parse(value).unwrap(),
                _ => {
                    return tool_error("group_by must be command, label, status, artifact, or run")
                }
            };
            let limit = arguments.get("limit").and_then(Value::as_u64).unwrap_or(20);
            if !(1..=200).contains(&limit) {
                return tool_error("limit must be between 1 and 200");
            }
            match service.db.dashboard(&filter, group, limit as usize) {
                Ok(dashboard) => tool_result(json!({ "dashboard": dashboard })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "get_wake_job" => {
            let Some(job_id) = arguments.get("job_id").and_then(Value::as_i64) else {
                return tool_error("job_id must be an integer");
            };
            match service.db.job(job_id) {
                Ok(Some(job)) => tool_result(json!({ "job": job })),
                Ok(None) => tool_error(format!("Wake job {job_id} was not found")),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "inspect_wake_artifact" => {
            let Some(path) = arguments.get("path").and_then(Value::as_str) else {
                return tool_error("path must be a string");
            };
            let offset = arguments.get("offset").and_then(Value::as_u64).unwrap_or(0);
            let limit = arguments
                .get("limit")
                .and_then(Value::as_u64)
                .unwrap_or(DEFAULT_READ_LIMIT as u64);
            if !(1..=MAX_READ_LIMIT as u64).contains(&limit) {
                return tool_error(format!("limit must be between 1 and {MAX_READ_LIMIT}"));
            }
            match service.db.owns_artifact_path(path) {
                Ok(true) => {}
                Ok(false) => {
                    return tool_error(
                        "path is not a non-deleted output artifact recorded in wake.db",
                    )
                }
                Err(error) => return tool_error(error.to_string()),
            }
            match service
                .artifacts
                .inspect(&service.source_id, path, offset, limit as usize)
            {
                Ok(artifact) => tool_result(json!({ "artifact": artifact })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "list_wake_fanouts" => {
            let filter = match job_filter(&arguments) {
                Ok(filter) => filter,
                Err(error) => return tool_error(error),
            };
            let limit = arguments.get("limit").and_then(Value::as_u64).unwrap_or(50);
            if !(1..=200).contains(&limit) {
                return tool_error("limit must be between 1 and 200");
            }
            match service.db.fanouts(&filter, limit as usize) {
                Ok(fanouts) => tool_result(json!({ "fanouts": fanouts })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "list_wake_jobs" => {
            let filter = match job_filter(&arguments) {
                Ok(filter) => filter,
                Err(error) => return tool_error(error),
            };
            let limit = arguments.get("limit").and_then(Value::as_u64).unwrap_or(50);
            if !(1..=200).contains(&limit) {
                return tool_error("limit must be between 1 and 200");
            }
            let offset = arguments.get("offset").and_then(Value::as_u64).unwrap_or(0);
            if offset > 100_000 {
                return tool_error("offset must be between 0 and 100000");
            }
            match service
                .db
                .filtered_jobs_page(&filter, offset as usize, limit as usize)
            {
                Ok(jobs) => tool_result(json!({ "jobs": jobs })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "list_wake_runs" => {
            let limit = arguments.get("limit").and_then(Value::as_u64).unwrap_or(50);
            if !(1..=200).contains(&limit) {
                return tool_error("limit must be between 1 and 200");
            }
            match service.db.runs(limit as usize) {
                Ok(runs) => tool_result(json!({ "runs": runs })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        _ => tool_error(format!("unknown Wake tool: {name}")),
    }
}

pub fn handle(service: &WakeService, request: Value) -> Option<Value> {
    let id = request.get("id").cloned();
    let method = request.get("method").and_then(Value::as_str);
    if request.get("jsonrpc").and_then(Value::as_str) != Some("2.0") || method.is_none() {
        return Some(error(id.unwrap_or(Value::Null), -32600, "Invalid Request"));
    }
    id.as_ref()?;
    let id = id.unwrap();
    let modern = modern_request(&request);
    let params = request.get("params").cloned().unwrap_or_else(|| json!({}));
    let result = match method.unwrap() {
        "server/discover" => json!({
            "resultType": "complete",
            "supportedVersions": [MODERN_VERSION],
            "capabilities": { "tools": { "listChanged": false } },
            "instructions": "Use the read-only Wake tools to inspect build metrics, grouped jobs, artifacts, fanouts, logs, and run history."
        }),
        "initialize" => {
            let requested = params
                .get("protocolVersion")
                .and_then(Value::as_str)
                .unwrap_or(LEGACY_VERSION);
            let version = match requested {
                "2024-11-05" | "2025-03-26" | "2025-06-18" | "2025-11-25" => requested,
                _ => LEGACY_VERSION,
            };
            json!({
                "protocolVersion": version,
                "capabilities": { "tools": { "listChanged": false } },
                "serverInfo": server_info(),
                "instructions": "Use the read-only Wake tools to inspect build metrics, grouped jobs, artifacts, fanouts, logs, and run history."
            })
        }
        "ping" => json!({}),
        "tools/list" => {
            let mut result = json!({ "tools": tools() });
            if modern {
                result["ttlMs"] = json!(1_000);
                result["cacheScope"] = json!("private");
            }
            result
        }
        "tools/call" => tool_call(service, &params),
        _ => {
            return Some(error(
                id,
                -32601,
                format!("Method not found: {}", method.unwrap()),
            ))
        }
    };
    Some(success(id, result, modern))
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::Connection;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn tools_are_deterministically_sorted() {
        let definitions = tools();
        let names: Vec<&str> = definitions
            .as_array()
            .unwrap()
            .iter()
            .map(|tool| tool["name"].as_str().unwrap())
            .collect();
        let mut sorted = names.clone();
        sorted.sort_unstable();
        assert_eq!(names, sorted);
    }

    #[test]
    fn artifact_tool_is_bounded_and_source_qualified() {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("wake-mcp-artifact-{suffix}"));
        fs::create_dir(&root).unwrap();
        let database = root.join("wake.db");
        Connection::open(&database)
            .unwrap()
            .execute_batch(
                "PRAGMA user_version = 1;
                 CREATE TABLE files(file_id INTEGER PRIMARY KEY, path TEXT, type TEXT,
                                    deleted INTEGER);
                 CREATE TABLE filetree(job_id INTEGER, file_id INTEGER, access INTEGER);
                 INSERT INTO files VALUES(1, 'result.txt', 'file', 0);
                 INSERT INTO filetree VALUES(1, 1, 2);",
            )
            .unwrap();
        fs::write(root.join("result.txt"), "tunnel vision").unwrap();
        fs::write(root.join("secret.txt"), "not an artifact").unwrap();
        let service = WakeService {
            db: WakeDb::new(database).unwrap(),
            artifacts: ArtifactRoot::new(&root).unwrap(),
            source_id: "slurm-a".to_owned(),
        };
        let response = handle(
            &service,
            json!({
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {
                    "name": "inspect_wake_artifact",
                    "arguments": { "path": "result.txt", "limit": 6 }
                }
            }),
        )
        .unwrap();
        assert_eq!(
            response.pointer("/result/structuredContent/artifact/uri"),
            Some(&json!("wake://slurm-a/result.txt"))
        );
        assert_eq!(
            response.pointer("/result/structuredContent/artifact/content"),
            Some(&json!("tunnel"))
        );
        assert_eq!(
            response.pointer("/result/structuredContent/artifact/truncated"),
            Some(&json!(true))
        );
        let rejected = handle(
            &service,
            json!({
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {
                    "name": "inspect_wake_artifact",
                    "arguments": { "path": "secret.txt" }
                }
            }),
        )
        .unwrap();
        assert_eq!(rejected.pointer("/result/isError"), Some(&json!(true)));
        fs::remove_dir_all(root).unwrap();
    }
}
