use crate::db::{JobState, WakeDb};
use serde_json::{json, Value};

const SERVER_INFO_KEY: &str = "io.modelcontextprotocol/serverInfo";
const MODERN_VERSION: &str = "2026-07-28";
const LEGACY_VERSION: &str = "2025-11-25";

fn server_info() -> Value {
    json!({
        "name": "wake-mcp",
        "version": env!("CARGO_PKG_VERSION"),
        "description": "Read-only access to Wake build jobs, artifacts, logs, and runs"
    })
}

fn tools() -> Value {
    json!([
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
            "name": "list_wake_jobs",
            "title": "List Wake Jobs",
            "description": "List recent Wake jobs, optionally filtering labels and artifact paths or selecting failed/passed jobs.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "query": { "type": "string", "description": "Substring in job label, ID, or artifact path" },
                    "state": { "type": "string", "enum": ["all", "failed", "passed"], "default": "all" },
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

fn tool_call(db: &WakeDb, params: &Value) -> Value {
    let Some(name) = params.get("name").and_then(Value::as_str) else {
        return tool_error("missing tool name");
    };
    let arguments = params
        .get("arguments")
        .cloned()
        .unwrap_or_else(|| json!({}));
    match name {
        "get_wake_job" => {
            let Some(job_id) = arguments.get("job_id").and_then(Value::as_i64) else {
                return tool_error("job_id must be an integer");
            };
            match db.job(job_id) {
                Ok(Some(job)) => tool_result(json!({ "job": job })),
                Ok(None) => tool_error(format!("Wake job {job_id} was not found")),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "list_wake_jobs" => {
            let query = arguments.get("query").and_then(Value::as_str);
            let state = match arguments
                .get("state")
                .and_then(Value::as_str)
                .unwrap_or("all")
            {
                "all" => JobState::All,
                "failed" => JobState::Failed,
                "passed" => JobState::Passed,
                _ => return tool_error("state must be all, failed, or passed"),
            };
            let limit = arguments.get("limit").and_then(Value::as_u64).unwrap_or(50);
            if !(1..=200).contains(&limit) {
                return tool_error("limit must be between 1 and 200");
            }
            match db.jobs(query, state, limit as usize) {
                Ok(jobs) => tool_result(json!({ "jobs": jobs })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        "list_wake_runs" => {
            let limit = arguments.get("limit").and_then(Value::as_u64).unwrap_or(50);
            if !(1..=200).contains(&limit) {
                return tool_error("limit must be between 1 and 200");
            }
            match db.runs(limit as usize) {
                Ok(runs) => tool_result(json!({ "runs": runs })),
                Err(error) => tool_error(error.to_string()),
            }
        }
        _ => tool_error(format!("unknown Wake tool: {name}")),
    }
}

pub fn handle(db: &WakeDb, request: Value) -> Option<Value> {
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
            "instructions": "Use the read-only Wake tools to inspect build jobs, artifacts, logs, and run history."
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
                "instructions": "Use the read-only Wake tools to inspect build jobs, artifacts, logs, and run history."
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
        "tools/call" => tool_call(db, &params),
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
}
