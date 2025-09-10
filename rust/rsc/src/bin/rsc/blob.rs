use crate::database;
use crate::types::{GetUploadUrlResponse, PostBlobResponse, PostBlobResponsePart};
use async_trait::async_trait;
use axum::{extract::Multipart, http::StatusCode, Json};
use entity::blob;
use futures::stream::BoxStream;
use futures::TryStreamExt;
use sea_orm::prelude::Uuid;
use sea_orm::{ActiveValue::*, DatabaseConnection};
use std::sync::Arc;
use tokio_util::bytes::Bytes;
use tracing;

#[async_trait]
pub trait BlobStore {
    fn id(&self) -> Uuid;
    async fn stream<'a>(
        &self,
        stream: BoxStream<'a, Result<Bytes, std::io::Error>>,
    ) -> Result<(String, i64), std::io::Error>;

    async fn download_url(&self, key: String) -> String;
    async fn delete_key(&self, key: String) -> Result<(), std::io::Error>;
}

pub trait DebugBlobStore: BlobStore + std::fmt::Debug {}

#[tracing::instrument(skip_all)]
pub async fn get_upload_url(server_addr: String) -> Json<GetUploadUrlResponse> {
    let url = server_addr + "/blob";
    Json(GetUploadUrlResponse { url })
}

#[tracing::instrument(skip_all)]
pub async fn create_blob(
    mut multipart: Multipart,
    db: Arc<DatabaseConnection>,
    active_store: Arc<dyn DebugBlobStore + Send + Sync>,
    dbonly_store: Arc<dyn DebugBlobStore + Send + Sync>,
) -> (StatusCode, Json<PostBlobResponse>) {
    let mut parts: Vec<PostBlobResponsePart> = Vec::new();
    let mut content_hash: Option<String> = None;

    while let Ok(Some(field)) = multipart.next_field().await {
        let name = match field.name() {
            Some(x) => x.to_string(),
            None => {
                return (
                    StatusCode::BAD_REQUEST,
                    Json(PostBlobResponse::Error {
                        message: "Multipart field must be named".into(),
                    }),
                )
            }
        };

        // Save the content hash of the blob
        if name == "content_hash" {
            match field.text().await {
                Ok(hash) => {
                    content_hash = Some(hash);
                    continue;
                }
                Err(err) => {
                    tracing::error!(%err, "Failed to read content_hash field");
                    return (
                        StatusCode::BAD_REQUEST,
                        Json(PostBlobResponse::Error {
                            message: "Invalid content_hash field".into(),
                        }),
                    );
                }
            }
        }

        // Process blob data fields
        let store = match field.content_type() {
            Some("blob/small") => dbonly_store.clone(),
            _ => active_store.clone(),
        };

        let result = store
            .stream(Box::pin(field.map_err(|err| {
                std::io::Error::new(std::io::ErrorKind::Other, err)
            })))
            .await;

        let (blob_key, blob_size) = match result {
            Err(err) => {
                tracing::error! {%err, "Failed to stream blob to store"};
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    Json(PostBlobResponse::Error {
                        message: err.to_string(),
                    }),
                );
            }
            Ok(resp) => resp,
        };

        let active_blob = blob::ActiveModel {
            id: NotSet,
            created_at: NotSet,
            updated_at: NotSet,
            key: Set(blob_key),
            size: Set(blob_size),
            store_id: Set(store.id()),
            content_hash: Set(content_hash.clone().unwrap_or_default()),
        };

        match database::upsert_blob(db.as_ref(), active_blob).await {
            Err(err) => {
                tracing::error! {%err, "Failed to upsert blob"};
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    Json(PostBlobResponse::Error {
                        message: err.to_string(),
                    }),
                );
            }
            Ok(id) => parts.push(PostBlobResponsePart {
                id,
                name,
                content_hash: content_hash.clone().unwrap_or_default(),
            }),
        }
    }

    (StatusCode::OK, Json(PostBlobResponse::Ok { blobs: parts }))
}
