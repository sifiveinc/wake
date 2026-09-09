use tower_lsp::lsp_types::*;

use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::{ChildStdin, Command};
use tokio::sync::mpsc::{channel, Receiver};
use tokio::time::timeout;

use tower_lsp::lsp_types::notification::{
    DidChangeTextDocument, DidOpenTextDocument, Exit, Initialized, PublishDiagnostics,
};
use tower_lsp::lsp_types::request::{HoverRequest, Initialize, Shutdown};

use std::process::Stdio;

fn jrpc_serialize(req: tower_lsp::jsonrpc::Request) -> std::string::String {
    let str = req.to_string();
    let length = str.len();

    format!("Content-Length: {}\r\n\r\n{}", length, str)
}

fn make_request<R>(id: tower_lsp::jsonrpc::Id, params: R::Params) -> tower_lsp::jsonrpc::Request
where
    R: tower_lsp::lsp_types::request::Request,
    R::Params: serde::Serialize,
    R::Result: serde::de::DeserializeOwned,
{
    tower_lsp::jsonrpc::Request::build(R::METHOD)
        .id(id)
        .params(serde_json::to_value(params).expect(""))
        .finish()
}

fn make_notification<R>(params: R::Params) -> tower_lsp::jsonrpc::Request
where
    R: tower_lsp::lsp_types::notification::Notification,
    R::Params: serde::Serialize,
{
    tower_lsp::jsonrpc::Request::build(R::METHOD)
        .params(serde_json::to_value(params).expect(""))
        .finish()
}

struct LSPServer {
    send: ChildStdin,
    receive: Receiver<String>,
    peek: Option<String>,
}

impl LSPServer {
    fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let mut child = Command::new("../../lib/wake/lsp-wake")
            .stdout(Stdio::piped())
            .stdin(Stdio::piped())
            .spawn()?;

        let stdin = child
            .stdin
            .take()
            .expect("failed to take child stdin handle");

        let stdout = child
            .stdout
            .take()
            .expect("failed to take child stdout handle");

        // Spawn a task to constantly read messages from the server
        let (tx, rx) = channel(100);
        tokio::spawn(async move {
            let mut reader = BufReader::new(stdout).lines();
            loop {
                let Ok(_length) = reader.next_line().await else {
                    panic!("Failed to read length from msg");
                };
                let Ok(_end_header) = reader.next_line().await else {
                    panic!("Failed to read end header from msg");
                };
                let Ok(Some(json)) = reader.next_line().await else {
                    panic!("Failed to read json from msg");
                };

                if let Err(msg) = tx.send(json).await {
                    panic!("Dropped msg tx: {}", msg);
                }
            }
        });

        Ok(Self {
            send: stdin,
            receive: rx,
            peek: None,
        })
    }

    async fn write_request<R>(
        &mut self,
        id: tower_lsp::jsonrpc::Id,
        params: R::Params,
    ) -> Result<usize, std::io::Error>
    where
        R: tower_lsp::lsp_types::request::Request,
        R::Params: serde::Serialize,
        R::Result: serde::de::DeserializeOwned,
    {
        let req = make_request::<R>(id, params);
        self.send.write(jrpc_serialize(req).as_bytes()).await
    }

    async fn write_notification<R>(&mut self, params: R::Params) -> Result<usize, std::io::Error>
    where
        R: tower_lsp::lsp_types::notification::Notification,
        R::Params: serde::Serialize,
    {
        let req = make_notification::<R>(params);
        self.send.write(jrpc_serialize(req).as_bytes()).await
    }

    async fn expect_one<R>(&mut self) -> R::Result
    where
        R: tower_lsp::lsp_types::request::Request,
        R::Params: serde::Serialize,
        R::Result: serde::de::DeserializeOwned,
    {
        let json = match self.peek.take() {
            Some(json) => json,
            None => {
                let Ok(Some(json)) =
                    timeout(std::time::Duration::from_millis(1000), self.receive.recv()).await
                else {
                    panic!(
                        "expect_one<{}> failed! No server response within the deadline.",
                        R::METHOD
                    );
                };
                json
            }
        };

        let res: tower_lsp::jsonrpc::Response = match serde_json::from_str(&json) {
            Ok(res) => res,
            Err(msg) => panic!(
                "expect_one<{}> failed! Could not parse. Reason: {}. In: {}",
                R::METHOD,
                msg,
                json
            ),
        };

        let value = match res.into_parts() {
            (_, Ok(value)) => value,
            (_, Err(msg)) => panic!(
                "expect_one<{}> failed! jsonrpc error. Reason: {}. In: {}",
                R::METHOD,
                msg,
                json
            ),
        };

        let inner: R::Result = match serde_json::from_value(value.clone()) {
            Ok(res) => res,
            Err(msg) => panic!(
                "expect_one<{}> failed! Could not parse. Reason: {}. In: {}",
                R::METHOD,
                msg,
                json
            ),
        };

        inner
    }

    async fn expect_many<R>(&mut self) -> Vec<R::Params>
    where
        R: tower_lsp::lsp_types::notification::Notification,
        R::Params: serde::Serialize,
    {
        let mut out = Vec::new();

        loop {
            let json = match self.peek.take() {
                Some(json) => json,
                None => match timeout(std::time::Duration::from_millis(1000), self.receive.recv())
                    .await
                {
                    Ok(Some(json)) => json,
                    _ => break,
                },
            };

            let Ok(res): Result<tower_lsp::jsonrpc::Request, _> = serde_json::from_str(&json)
            else {
                panic!("Failed to parse jsonrpc request into expected type");
            };

            let Some(value) = res.params() else {
                self.peek = Some(json);
                break;
            };
            let Ok(inner): Result<R::Params, _> = serde_json::from_value(value.clone()) else {
                self.peek = Some(json);
                break;
            };

            out.push(inner);
        }

        out
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut lsp = LSPServer::new()?;

    let cwd = std::env::current_dir()?;
    let Some(cwd) = cwd.to_str() else {
        panic!("Failed to convert cwd to string");
    };

    lsp.write_request::<Initialize>(
        tower_lsp::jsonrpc::Id::Number(5),
        InitializeParams {
            process_id: Some(5),
            root_path: None,
            root_uri: None,
            initialization_options: None,
            capabilities: ClientCapabilities {
                workspace: None,
                text_document: None,
                window: None,
                general: None,
                experimental: None,
            },
            trace: None,
            workspace_folders: Some(
                [WorkspaceFolder {
                    uri: Url::parse(format!("file://{}", cwd).as_str())?,
                    name: "wake".to_string(),
                }]
                .to_vec(),
            ),
            client_info: Some(ClientInfo {
                name: "Neovim".to_string(),
                version: Some("0.8.0".to_string()),
            }),
            locale: None,
        },
    )
    .await?;

    let _response = lsp.expect_one::<Initialize>().await;

    lsp.write_notification::<Initialized>(InitializedParams {})
        .await?;

    let _responses = lsp.expect_many::<PublishDiagnostics>().await;

    let test_wake = Url::parse(format!("file://{}/test.wake", cwd).as_str())?;

    lsp.write_notification::<DidOpenTextDocument>(DidOpenTextDocumentParams {
        text_document: TextDocumentItem {
            uri: test_wake.clone(),
            language_id: "wake".to_string(),
            version: 1,
            text: "# comment\n# comment \n\ndef unused = 5".to_string(),
        },
    })
    .await?;

    lsp.write_notification::<DidChangeTextDocument>(DidChangeTextDocumentParams {
        text_document: VersionedTextDocumentIdentifier {
            uri: test_wake.clone(),
            version: 1,
        },
        content_changes: [TextDocumentContentChangeEvent {
            range: None,
            range_length: None,
            text: "# comment\n# comment \n\ndef y = 6".to_string(),
        }]
        .to_vec(),
    })
    .await?;

    let hover_params = HoverParams {
        text_document_position_params: TextDocumentPositionParams {
            text_document: TextDocumentIdentifier {
                uri: test_wake.clone(),
            },
            position: Position {
                line: 0,
                character: 0,
            },
        },
        work_done_progress_params: WorkDoneProgressParams {
            work_done_token: None,
        },
    };

    lsp.write_request::<HoverRequest>(tower_lsp::jsonrpc::Id::Number(6), hover_params.clone())
        .await?;

    let _response = lsp.expect_one::<HoverRequest>().await;

    lsp.write_request::<HoverRequest>(tower_lsp::jsonrpc::Id::Number(7), hover_params.clone())
        .await?;

    let _response = lsp.expect_one::<HoverRequest>().await;

    lsp.write_request::<HoverRequest>(tower_lsp::jsonrpc::Id::Number(8), hover_params.clone())
        .await?;

    let _responses = lsp.expect_many::<PublishDiagnostics>().await;
    let _response = lsp.expect_one::<HoverRequest>().await;

    lsp.write_request::<Shutdown>(tower_lsp::jsonrpc::Id::Number(9), ())
        .await?;
    let _response = lsp.expect_one::<Shutdown>().await;

    lsp.write_notification::<Exit>(()).await?;

    Ok(())
}
