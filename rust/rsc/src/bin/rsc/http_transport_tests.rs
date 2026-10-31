use super::*;
use axum::{
    body::Body,
    http::{header, Request, StatusCode},
    response::Response,
};
use sea_orm::{ActiveModelTrait, ActiveValue::*};
use serde_json::{json, Value};
use tower::Service;

async fn request(router: &mut Router, method: &str, path: &str, body: Value) -> Response {
    router
        .call(
            Request::builder()
                .method(method)
                .uri(path)
                .header(header::CONTENT_TYPE, "application/json")
                .header(header::AUTHORIZATION, "InsecureKey")
                .header(header::HOST, "untrusted.example")
                .body(Body::from(serde_json::to_vec(&body).unwrap()))
                .unwrap(),
        )
        .await
        .unwrap()
}

async fn json_body(response: Response) -> Value {
    serde_json::from_slice(&hyper::body::to_bytes(response).await.unwrap()).unwrap()
}

fn multipart(bytes: &[u8], complete: bool) -> Vec<u8> {
    let mut body =
        b"--test-boundary\r\nContent-Disposition: form-data; name=\"file\"\r\n\r\n".to_vec();
    body.extend_from_slice(bytes);
    if complete {
        body.extend_from_slice(b"\r\n--test-boundary--\r\n");
    }
    body
}

async fn upload(router: &mut Router, body: Body) -> Response {
    router
        .call(
            Request::builder()
                .method("POST")
                .uri("/blob")
                .header(header::AUTHORIZATION, "InsecureKey")
                .header(
                    header::CONTENT_TYPE,
                    "multipart/form-data; boundary=test-boundary",
                )
                .body(body)
                .unwrap(),
        )
        .await
        .unwrap()
}

async fn setup() -> (Arc<DatabaseConnection>, Router, tempfile::TempDir, Uuid) {
    let db = tests::create_standalone_db().await.unwrap();
    database::create_dbonly_blob_store(&db).await.unwrap();
    tests::create_insecure_api_key(&db).await.unwrap();
    let root = tempfile::tempdir().unwrap();
    let local = database::create_local_blob_store(&db, root.path().to_str().unwrap().into())
        .await
        .unwrap();
    let mut config = tests::create_config(local.id);
    config.public_base_url = Some("https://cache.example/prefix".into());
    let db = Arc::new(db);
    let stores = activate_stores(db.clone(), config.public_url().unwrap()).await;
    let router = create_router(
        db.clone(),
        Arc::new(config),
        &stores,
        Arc::new(RwLock::new(0.0)),
    );
    (db, router, root, local.id)
}

async fn insert_blob(db: &DatabaseConnection, store_id: Uuid, key: &str) -> Uuid {
    database::upsert_blob(
        db,
        entity::blob::ActiveModel {
            id: NotSet,
            created_at: NotSet,
            updated_at: NotSet,
            key: Set(key.into()),
            size: Set(0),
            store_id: Set(store_id),
            content_hash: Set("hash".into()),
        },
    )
    .await
    .unwrap()
}

#[tokio::test]
async fn portable_router_upload_download_and_matching() {
    let (db, mut router, root, local_id) = setup().await;
    let response = request(&mut router, "GET", "/blob", Value::Null).await;
    assert_eq!(
        json_body(response).await["url"],
        "https://cache.example/prefix/blob"
    );

    // Small binary writes exercise the buffer flush; larger ones cross stream boundaries.
    for size in [0, 23, 131073] {
        let bytes = (0..size).map(|i| (i % 256) as u8).collect::<Vec<_>>();
        let response = upload(&mut router, Body::from(multipart(&bytes, true))).await;
        assert_eq!(response.status(), StatusCode::OK);
        let body = json_body(response).await;
        let id: Uuid = body["blobs"][0]["id"].as_str().unwrap().parse().unwrap();
        let response = request(&mut router, "GET", &format!("/blob/{id}"), Value::Null).await;
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(
            response.headers()[header::CONTENT_TYPE],
            "application/octet-stream"
        );
        assert_eq!(response.headers()[header::CONTENT_LENGTH], size.to_string());
        assert_eq!(hyper::body::to_bytes(response).await.unwrap(), bytes);
    }

    // Existing local keys and inline records need no migration or relocation.
    std::fs::create_dir_all(root.path().join("AA/BB")).unwrap();
    std::fs::write(root.path().join("AA/BB/old"), b"existing").unwrap();
    let old = insert_blob(db.as_ref(), local_id, "AA/BB/old").await;
    let inline = insert_blob(db.as_ref(), database::read_dbonly_blob_store_id(), "%00%FF").await;
    let response = request(&mut router, "GET", &format!("/blob/{old}"), Value::Null).await;
    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(hyper::body::to_bytes(response).await.unwrap(), "existing");

    let job = json!({
        "cmd": b"portable", "env": b"PATH=/usr/bin", "cwd": "/workspace", "stdin": "",
        "is_atty": false, "hidden_info": "", "visible_files": [], "output_dirs": [],
        "output_symlinks": [], "output_files": [], "stdout_blob_id": old, "stderr_blob_id": inline,
        "status": 0, "runtime": 1.0, "cputime": 1.0, "memory": 1000, "ibytes": 0, "obytes": 0,
    });
    let response = request(&mut router, "POST", "/job", job.clone()).await;
    assert_eq!(response.status(), StatusCode::OK);
    let response = request(&mut router, "POST", "/job/matching", job).await;
    assert_eq!(response.status(), StatusCode::OK);
    let body = json_body(response).await;
    assert_eq!(
        body["stdout_blob"]["url"],
        format!("https://cache.example/prefix/blob/{old}")
    );
    assert_eq!(body["stderr_blob"]["url"], "db://%00%FF");
}

#[tokio::test]
async fn portable_router_missing_unsupported_and_invalid_content() {
    let (db, mut router, root, local_id) = setup().await;
    for path in [
        "/blob/not-a-uuid".to_string(),
        "/blob/%2E%2E%2Fetc%2Fpasswd".into(),
    ] {
        assert_eq!(
            request(&mut router, "GET", &path, Value::Null)
                .await
                .status(),
            StatusCode::BAD_REQUEST
        );
    }
    let unknown = Uuid::from_u128(rand::random());
    let missing = insert_blob(db.as_ref(), local_id, "missing").await;
    let inline = insert_blob(db.as_ref(), database::read_dbonly_blob_store_id(), "%01").await;
    std::fs::create_dir(root.path().join("directory")).unwrap();
    let directory = insert_blob(db.as_ref(), local_id, "directory").await;
    let traversal = insert_blob(db.as_ref(), local_id, "../../outside").await;
    let inactive_store = entity::blob_store::ActiveModel {
        id: NotSet,
        r#type: Set("UnsupportedStore".into()),
    }
    .insert(db.as_ref())
    .await
    .unwrap();
    let inactive = insert_blob(db.as_ref(), inactive_store.id, "unused").await;
    for (id, expected) in [
        (unknown, StatusCode::NOT_FOUND),
        (missing, StatusCode::NOT_FOUND),
        (inline, StatusCode::NOT_IMPLEMENTED),
        (directory, StatusCode::INTERNAL_SERVER_ERROR),
        (traversal, StatusCode::INTERNAL_SERVER_ERROR),
        (inactive, StatusCode::NOT_FOUND),
    ] {
        let response = request(&mut router, "GET", &format!("/blob/{id}"), Value::Null).await;
        assert_eq!(response.status(), expected);
        assert!(hyper::body::to_bytes(response).await.unwrap().is_empty());
    }
}

#[tokio::test]
async fn portable_router_interrupted_upload_does_not_publish() {
    let (db, mut router, root, _) = setup().await;
    let body = futures::stream::iter(vec![
        Ok(multipart(&vec![42; 16384], false)),
        Err(std::io::Error::new(
            std::io::ErrorKind::ConnectionReset,
            "disconnected",
        )),
    ]);
    let response = upload(&mut router, Body::wrap_stream(body)).await;
    assert!(response.status().is_client_error() || response.status().is_server_error());
    assert_eq!(database::count_blobs(db.as_ref()).await.unwrap(), 0);
    // Temporary files are removed; empty shard directories may remain.
    for shard in std::fs::read_dir(root.path()).unwrap() {
        for subdir in std::fs::read_dir(shard.unwrap().path()).unwrap() {
            assert_eq!(
                std::fs::read_dir(subdir.unwrap().path()).unwrap().count(),
                0
            );
        }
    }
    let response = upload(&mut router, Body::from("invalid multipart framing")).await;
    assert_eq!(response.status(), StatusCode::BAD_REQUEST);
    assert_eq!(database::count_blobs(db.as_ref()).await.unwrap(), 0);
}

#[tokio::test]
async fn portable_router_database_failure_is_an_error() {
    let (db, mut router, _root, _) = setup().await;
    db.as_ref().clone().close().await.unwrap();
    let response = request(
        &mut router,
        "GET",
        &format!("/blob/{}", Uuid::from_u128(rand::random())),
        Value::Null,
    )
    .await;
    assert_eq!(response.status(), StatusCode::INTERNAL_SERVER_ERROR);
}

#[tokio::test]
async fn portable_router_unpublished_file_is_removed_on_database_error() {
    let (db, _router, root, _) = setup().await;
    // Use a valid local writer whose store has no database record, forcing publication to fail.
    let id = Uuid::from_u128(rand::random());
    let mut stores = activate_stores(db.clone(), None).await;
    stores.insert(
        id,
        Arc::new(blob_store_impls::LocalBlobStore {
            id,
            root: root.path().to_str().unwrap().into(),
            public_base_url: None,
        }),
    );
    let mut router = create_router(
        db.clone(),
        Arc::new(tests::create_config(id)),
        &stores,
        Arc::new(RwLock::new(0.0)),
    );
    let response = upload(&mut router, Body::from(multipart(b"unpublished", true))).await;
    assert_eq!(response.status(), StatusCode::INTERNAL_SERVER_ERROR);
    assert_eq!(database::count_blobs(db.as_ref()).await.unwrap(), 0);
    for shard in std::fs::read_dir(root.path()).unwrap() {
        for subdir in std::fs::read_dir(shard.unwrap().path()).unwrap() {
            assert_eq!(
                std::fs::read_dir(subdir.unwrap().path()).unwrap().count(),
                0
            );
        }
    }
}
