use crate::database;
use crate::types::{GetUploadUrlResponse, PostBlobResponse, PostBlobResponsePart};
use async_trait::async_trait;
use axum::{
    body::StreamBody,
    extract::{Multipart, Path},
    http::{header, StatusCode},
    response::{IntoResponse, Response},
    Json,
};
use entity::blob;
use futures::stream::BoxStream;
use futures::TryStreamExt;
use sea_orm::prelude::Uuid;
use sea_orm::{ActiveValue::*, DatabaseConnection, EntityTrait};
use std::collections::HashMap;
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

    async fn download_url(&self, id: Uuid, key: String) -> String;
    // Inline and non-filesystem stores keep their own URL schemes.
    async fn content(&self, _key: String) -> Result<BlobContent, std::io::Error> {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "Store does not support HTTP downloads",
        ))
    }
    async fn delete_key(&self, key: String) -> Result<(), std::io::Error>;
}

pub trait DebugBlobStore: BlobStore + std::fmt::Debug {}

pub struct BlobContent {
    pub stream: BoxStream<'static, Result<Bytes, std::io::Error>>,
    pub size: u64,
}

#[tracing::instrument(skip_all)]
pub async fn download_blob(
    Path(id): Path<Uuid>,
    db: Arc<DatabaseConnection>,
    stores: HashMap<Uuid, Arc<dyn DebugBlobStore + Send + Sync>>,
) -> Response {
    let blob = match blob::Entity::find_by_id(id).one(db.as_ref()).await {
        Ok(Some(blob)) => blob,
        Ok(None) => return StatusCode::NOT_FOUND.into_response(),
        Err(err) => {
            tracing::error!(%err, %id, "Failed to look up blob");
            return StatusCode::INTERNAL_SERVER_ERROR.into_response();
        }
    };
    let Some(store) = stores.get(&blob.store_id) else {
        return StatusCode::NOT_FOUND.into_response();
    };
    match store.content(blob.key).await {
        Ok(content) => (
            [
                (header::CONTENT_TYPE, "application/octet-stream".to_string()),
                (header::CONTENT_LENGTH, content.size.to_string()),
            ],
            StreamBody::new(content.stream),
        )
            .into_response(),
        Err(err) => {
            tracing::warn!(%err, %id, "Failed to open blob content");
            match err.kind() {
                std::io::ErrorKind::NotFound => StatusCode::NOT_FOUND,
                std::io::ErrorKind::Unsupported => StatusCode::NOT_IMPLEMENTED,
                _ => StatusCode::INTERNAL_SERVER_ERROR,
            }
            .into_response()
        }
    }
}

#[tracing::instrument(skip_all)]
pub async fn get_upload_url(
    server_addr: String,
    public_url: Option<url::Url>,
) -> Json<GetUploadUrlResponse> {
    let url = match public_url {
        Some(base) => base
            .join("blob")
            .expect("validated HTTP base URL")
            .to_string(),
        None => server_addr + "/blob",
    };
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

    loop {
        let field = match multipart.next_field().await {
            Ok(Some(field)) => field,
            Ok(None) => break,
            Err(err) => {
                tracing::warn!(%err, "Failed to read multipart field");
                return (
                    StatusCode::BAD_REQUEST,
                    Json(PostBlobResponse::Error {
                        message: "Invalid multipart upload".into(),
                    }),
                );
            }
        };
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
                        message: "Failed to store blob".into(),
                    }),
                );
            }
            Ok(resp) => resp,
        };

        let active_blob = blob::ActiveModel {
            id: NotSet,
            created_at: NotSet,
            updated_at: NotSet,
            key: Set(blob_key.clone()),
            size: Set(blob_size),
            store_id: Set(store.id()),
            content_hash: Set(content_hash.clone().unwrap_or_default()),
        };

        match database::upsert_blob(db.as_ref(), active_blob).await {
            Err(err) => {
                tracing::error! {%err, "Failed to upsert blob"};
                if let Err(err) = store.delete_key(blob_key).await {
                    tracing::warn!(%err, "Failed to remove unpublished blob");
                }
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    Json(PostBlobResponse::Error {
                        message: "Failed to publish blob".into(),
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
