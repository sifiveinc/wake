use axum::{
    extract::{DefaultBodyLimit, Multipart},
    routing::{get, post},
    Router,
    response::IntoResponse,
};
use chrono::Utc;
use clap::Parser;
use itertools::Itertools;
use migration::{Migrator, MigratorTrait};
use prometheus::{Encoder, TextEncoder};
use rlimit::Resource;
use rsc::database;
use sea_orm::{prelude::Uuid, ConnectOptions, Database, DatabaseConnection};
use std::collections::HashMap;
use std::io::{Error, ErrorKind};
use std::sync::{Arc, RwLock};
use std::time::Duration;
use tracing;

mod add_job;
mod api_key_check;
mod blob;
mod blob_store_impls;
mod config;
mod dashboard;
mod read_job;
mod types;

#[derive(Debug, Parser)]
struct ServerOptions {
    #[arg(help = "Shows the config and then exits", long)]
    show_config: bool,
}

// Maps databse store uuid -> dyn blob::DebugBlobStore
// that represent said store.
async fn activate_stores(
    conn: Arc<DatabaseConnection>,
) -> HashMap<Uuid, Arc<dyn blob::DebugBlobStore + Sync + Send>> {
    let mut active_stores: HashMap<Uuid, Arc<dyn blob::DebugBlobStore + Sync + Send>> =
        HashMap::new();

    // --- Activate Test Blob Stores  ---
    let test_stores = match database::read_test_blob_stores(conn.as_ref()).await {
        Ok(stores) => stores,
        Err(err) => {
            tracing::warn!(%err, "Failed to read test stores from database");
            Vec::new()
        }
    };

    for store in test_stores.into_iter() {
        active_stores.insert(
            store.id,
            Arc::new(blob_store_impls::TestBlobStore { id: store.id }),
        );
    }

    // --- Activate Local Blob Stores ---
    let stores = match database::read_local_blob_stores(conn.as_ref()).await {
        Ok(stores) => stores,
        Err(err) => {
            tracing::warn!(%err, "Failed to read local stores from database");
            Vec::new()
        }
    };

    for store in stores.into_iter() {
        active_stores.insert(
            store.id,
            Arc::new(blob_store_impls::LocalBlobStore {
                id: store.id,
                root: store.root,
            }),
        );
    }

    // ---    Activate DBOnly Store   ---
    let dbonly_store = match database::read_dbonly_blob_store(conn.as_ref()).await {
        Ok(Some(store)) => store,
        Ok(None) => {
            panic!("Database is not configured with a DbOnly store. Please bootstrap the db")
        }
        Err(_) => panic!("Failed to load DbOnly store from database. Unable to continue"),
    };
    active_stores.insert(
        dbonly_store.id,
        Arc::new(blob_store_impls::DbOnlyBlobStore {
            id: dbonly_store.id,
        }),
    );

    return active_stores;
}

#[derive(serde::Deserialize)]
struct VersionCheck {
    version: String,
}

async fn check_version(check: axum::extract::Query<VersionCheck>) -> axum::http::StatusCode {
    // During development, declare all version as compatible
    if !check.version.starts_with("sifive/wake/") {
        return axum::http::StatusCode::FORBIDDEN;
    }

    return axum::http::StatusCode::OK;
}

async fn metrics_handler() -> impl IntoResponse {
    let encoder = TextEncoder::new();
    let metric_families = prometheus::gather();
    let mut buffer = Vec::new();
    if let Err(err) = encoder.encode(&metric_families, &mut buffer) {
        tracing::error! {%err, "Failed to encode metrics"};
    }

    (
        axum::http::StatusCode::OK,
        String::from_utf8(buffer).unwrap_or_default()
    )
}

fn create_router(
    conn: Arc<DatabaseConnection>,
    config: Arc<config::RSCConfig>,
    blob_stores: &HashMap<Uuid, Arc<dyn blob::DebugBlobStore + Sync + Send>>,
    system_load: Arc<RwLock<f64>>,
) -> Router {
    let Ok(active_store_uuid) = Uuid::parse_str(&config.active_store) else {
        panic!("Failed to parse provided active store into uuid");
    };

    let Some(active_store) = blob_stores.get(&active_store_uuid).clone() else {
        panic!("UUID for active store not in database");
    };

    let dbonly_uuid = database::read_dbonly_blob_store_id();
    let Some(dbonly_store) = blob_stores.get(&dbonly_uuid).clone() else {
        panic!("UUID for db store not in database");
    };

    Router::new()
        // Authorized Routes
        .route(
            "/job",
            post({
                let conn = conn.clone();
                move |body| add_job::add_job(body, conn)
            })
            .layer(DefaultBodyLimit::disable())
            .layer(axum::middleware::from_fn({
                let conn = conn.clone();
                move |req, next| api_key_check::api_key_check_middleware(req, next, conn.clone())
            })),
        )
        .route(
            "/blob",
            post({
                let conn = conn.clone();
                let active = active_store.clone();
                let dbonly = dbonly_store.clone();

                move |multipart: Multipart| blob::create_blob(multipart, conn, active, dbonly)
            })
            .layer(DefaultBodyLimit::disable())
            .layer(axum::middleware::from_fn({
                let conn = conn.clone();
                move |req, next| api_key_check::api_key_check_middleware(req, next, conn.clone())
            })),
        )
        .route(
            "/auth/check",
            post(axum::http::StatusCode::OK).layer(axum::middleware::from_fn({
                let conn = conn.clone();
                move |req, next| api_key_check::api_key_check_middleware(req, next, conn.clone())
            })),
        )
        // Unauthorized Routes
        .route(
            "/dashboard",
            get({
                let conn = conn.clone();
                move || dashboard::stats(conn)
            }),
        )
        .route("/metrics", get(metrics_handler))
        .route(
            "/job/matching",
            post({
                let conn = conn.clone();
                let blob_stores = blob_stores.clone();
                move |body| read_job::read_job(body, conn, blob_stores)
            })
            .layer(DefaultBodyLimit::disable()),
        )
        .route(
            "/job/allowed",
            post({
                let conn = conn.clone();
                let target = config.load_shed.target.clone();
                let min_runtime = config.load_shed.min_runtime.clone();
                move |body| read_job::allow_job(body, conn, target, system_load, min_runtime)
            })
            .layer(DefaultBodyLimit::disable()),
        )
        .route(
            "/blob",
            get({
                let config = config.clone();
                move || blob::get_upload_url(config.server_address.clone())
            }),
        )
        .route("/version/check", get(check_version))
}

async fn connect_to_database(
    config: &config::RSCConfig,
) -> Result<DatabaseConnection, Box<dyn std::error::Error>> {
    let timeout = config.connection_pool_timeout;
    let max_connect = config.connection_pool_max_connect;
    let mut opt = ConnectOptions::new(&config.database_url);
    opt.sqlx_logging_level(tracing::log::LevelFilter::Debug)
        .acquire_timeout(std::time::Duration::from_secs(timeout))
        .max_connections(max_connect);

    tracing::info!(%timeout, "Max seconds to wait for connection from pool");
    tracing::info!(%max_connect, "Max number of connections in pool");

    let connection = Database::connect(opt).await?;
    let pending_migrations = Migrator::get_pending_migrations(&connection).await?;
    if pending_migrations.len() != 0 {
        let err = Error::new(
            ErrorKind::Other,
            format!(
                "This rsc version expects {:?} additional migrations to be applied",
                pending_migrations.len()
            ),
        );
        tracing::error! {%err, "unperformed migrations, please apply these migrations before starting rsc"};
        Err(err)?
    }

    Ok(connection)
}

fn launch_job_eviction(conn: Arc<DatabaseConnection>, tick_interval: u64, ttl: u64) {
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(tick_interval));
        loop {
            interval.tick().await;
            let ttl = (Utc::now() - Duration::from_secs(ttl)).naive_utc();

            match database::evict_jobs_ttl(conn.clone(), ttl).await {
                Ok(res) => tracing::info!(%res, "Deleted jobs from database"),
                Err(err) => tracing::error!(%err, "Failed to delete jobs for eviction"),
            };
        }
    });
}

fn launch_blob_eviction(
    conn: Arc<DatabaseConnection>,
    config: Arc<config::RSCConfig>,
    blob_stores: HashMap<Uuid, Arc<dyn blob::DebugBlobStore + Sync + Send>>,
) {
    tokio::spawn(async move {
        let mut interval =
            tokio::time::interval(Duration::from_secs(config.blob_eviction.tick_rate));
        let mut should_sleep = false;
        loop {
            if should_sleep {
                interval.tick().await;
            }

            // Blobs must be at least this old to be considered for eviction.
            // This gives clients time to reference a blob before it gets evicted.
            let ttl = (Utc::now() - Duration::from_secs(config.blob_eviction.ttl)).naive_utc();

            let blobs = match database::delete_unreferenced_blobs(
                conn.as_ref(),
                ttl,
                config.blob_eviction.chunk_size,
            )
            .await
            {
                Ok(b) => b,
                Err(err) => {
                    tracing::error!(%err, "Failed to delete blobs for eviction");
                    should_sleep = true;
                    continue; // Try again on the next tick
                }
            };

            let deleted = blobs.len();

            should_sleep = deleted == 0;

            tracing::info!(%deleted, "N blobs deleted for eviction");

            // Delete blobs from blob store
            let chunked: Vec<Vec<database::DeletedBlob>> = blobs
                .into_iter()
                .chunks(config.blob_eviction.file_chunk_size)
                .into_iter()
                .map(|chunk| chunk.collect())
                .collect();

            for chunk in chunked {
                let thread_store = blob_stores.clone();

                tokio::spawn(async move {
                    for blob in chunk {
                        let store = match thread_store.get(&blob.store_id) {
                            Some(s) => s.clone(),
                            None => {
                                let blob = blob.clone();
                                tracing::info!(%blob.store_id, %blob.key, "Blob has been orphaned!");
                                tracing::error!(%blob.store_id, "Blob's store id missing from activated stores");
                                continue;
                            }
                        };

                        store.delete_key(blob.key.clone()).await.unwrap_or_else(|err| {
                            let blob = blob.clone();
                            tracing::info!(%blob.store_id, %blob.key, "Blob has been orphaned!");
                            tracing::error!(%err, "Failed to delete blob from store for eviction. See above for blob info");
                        });
                    }
                });
            }
        }
    });
}

fn launch_job_size_calculate(conn: Arc<DatabaseConnection>, config: Arc<config::RSCConfig>) {
    tokio::spawn(async move {
        let mut interval =
            tokio::time::interval(Duration::from_secs(config.job_size_calculate.tick_rate));
        let mut should_sleep = false;
        loop {
            if should_sleep {
                interval.tick().await;
            }

            let count = match database::calculate_job_size(
                conn.as_ref(),
                config.job_size_calculate.chunk_size,
            )
            .await
            {
                Ok(Some(c)) => c.updated_count,
                Ok(None) => {
                    tracing::error!("Failed to extract result from calculating job size");
                    should_sleep = true;
                    continue; // Try again on the next tick
                }
                Err(err) => {
                    tracing::error!(%err, "Failed to calculate and update job size");
                    should_sleep = true;
                    continue; // Try again on the next tick
                }
            };

            should_sleep = count == 0;

            tracing::info!(%count, "Calculated and updated size for jobs");
        }
    });
}

fn launch_system_usage_refresh(config: Arc<config::RSCConfig>, system_load: Arc<RwLock<f64>>) {
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(config.load_shed.tick_rate));
        loop {
            interval.tick().await;
            let local_load = sysinfo::System::load_average().one;

            match system_load.write() {
                Ok(mut lock) => {
                    *lock = local_load;
                }
                Err(err) => {
                    tracing::error!(%err, "Failed to aquire write lock for system load");
                    continue;
                }
            };

            tracing::info!(%local_load, "System load");
        }
    });
}

fn request_max_fileno_limit() {
    let Ok((current, max)) = Resource::NOFILE.get() else {
        tracing::warn!("Unable to discover fileno limits. Using default");
        return;
    };

    tracing::info!(%current, %max, "Discovered fileno limits");

    let Ok(new_limit) = rlimit::increase_nofile_limit(max) else {
        tracing::warn!("Unable to increase fileno limits. Using default");
        return;
    };

    tracing::info!(%new_limit, "Increased fileno limit");
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Parse the arguments
    let args = ServerOptions::parse();

    // Get the configuration
    let config = config::RSCConfig::new()?;
    let config = Arc::new(config);

    // setup a subscriber for logging
    let _guard = if let Some(log_directory) = config.log_directory.clone() {
        let file_appender = tracing_appender::rolling::daily(log_directory, "rsc.log");
        let (non_blocking, guard) = tracing_appender::non_blocking(file_appender);
        tracing_subscriber::fmt().with_writer(non_blocking).init();
        Some(guard)
    } else {
        let subscriber = tracing_subscriber::FmtSubscriber::new();
        tracing::subscriber::set_global_default(subscriber)?;
        None
    };

    let config_json = serde_json::to_string_pretty(&config).unwrap();
    if args.show_config {
        println!("{}", config_json);
        return Ok(());
    }
    tracing::info!(%config_json, "Launching RSC with config");

    // Increase the number of allowed open files the the max
    request_max_fileno_limit();

    // Connect to the db
    let connection = connect_to_database(&config).await?;
    let connection = Arc::new(connection);

    // Activate blob stores
    let stores = activate_stores(connection.clone()).await;

    // Create the resouce needed to track the current system utilization
    let system_load = Arc::new(RwLock::new(0.0));

    // Launch long running concurrent threads
    match &config.job_eviction {
        config::RSCJobEvictionConfig::TTL(ttl) => {
            launch_job_eviction(connection.clone(), ttl.tick_rate, ttl.ttl);
        }
        config::RSCJobEvictionConfig::LRU(_) => panic!("LRU not implemented"),
    }

    launch_blob_eviction(connection.clone(), config.clone(), stores.clone());
    launch_job_size_calculate(connection.clone(), config.clone());
    launch_system_usage_refresh(config.clone(), system_load.clone());

    // Launch the server
    let router = create_router(connection.clone(), config.clone(), &stores, system_load);
    axum::Server::bind(&config.server_address.parse()?)
        .serve(router.into_make_service())
        .await?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use data_encoding::HEXLOWER;
    use entity::blob_store;
    use rand::{thread_rng, RngCore};
    use sea_orm::{
        prelude::Uuid, ActiveModelTrait, ActiveValue::*, ConnectionTrait, EntityTrait,
        PaginatorTrait,
    };
    use std::sync::Arc;

    use axum::{
        body::Body,
        http::{self, header::*, Request, StatusCode},
    };
    use chrono::Duration;
    use mime::BOUNDARY;
    use serde_json::{json, Value};
    use std::io::Write;
    use tower::Service;

    async fn create_test_store(
        db: &DatabaseConnection,
    ) -> Result<Uuid, Box<dyn std::error::Error>> {
        database::create_dbonly_blob_store(db).await?;

        let test_store = blob_store::ActiveModel {
            id: NotSet,
            r#type: Set("TestBlobStore".into()),
        };
        let inserted = test_store.insert(db).await?;

        Ok(inserted.id)
    }

    fn create_config(store_id: Uuid) -> config::RSCConfig {
        config::RSCConfig {
            database_url: "test:0000".to_string(),
            server_address: "".to_string(),
            active_store: store_id.to_string(),
            connection_pool_max_connect: 10,
            connection_pool_timeout: 10,
            log_directory: None,
            blob_eviction: config::RSCBlobTTLConfig {
                tick_rate: 10,
                ttl: 100,
                chunk_size: 100,
                file_chunk_size: 1,
            },
            job_eviction: config::RSCJobEvictionConfig::TTL(config::RSCTTLConfig {
                tick_rate: 10,
                ttl: 100,
                chunk_size: 100,
            }),
            job_size_calculate: config::RSCCronLoopConfig {
                tick_rate: 10,
                chunk_size: 100,
            },
            load_shed: config::RSCLoadShedConfig {
                tick_rate: 10,
                target: 10.0,
                min_runtime: 0.5,
            },
        }
    }

    async fn create_fake_blob(
        db: &DatabaseConnection,
        store_id: Uuid,
    ) -> Result<Uuid, Box<dyn std::error::Error>> {
        let active_key = entity::blob::ActiveModel {
            id: NotSet,
            created_at: Set((Utc::now() - Duration::days(5)).naive_utc()),
            updated_at: Set((Utc::now() - Duration::days(5)).naive_utc()),
            key: Set("InsecureKey".into()),
            size: Set(11),
            store_id: Set(store_id),
        };

        let inserted_key = active_key.insert(db).await?;

        Ok(inserted_key.id)
    }

    async fn create_insecure_api_key(
        db: &DatabaseConnection,
    ) -> Result<String, Box<dyn std::error::Error>> {
        let active_key = entity::api_key::ActiveModel {
            id: NotSet,
            created_at: NotSet,
            key: Set("InsecureKey".into()),
            desc: Set("Generated Insecure Key".into()),
        };

        let inserted_key = active_key.insert(db).await?;

        Ok(inserted_key.key)
    }

    async fn create_standalone_db() -> Result<DatabaseConnection, sea_orm::DbErr> {
        let shim_db = Database::connect("postgres://127.0.0.1/shim").await?;
        let mut buf = [0u8; 24];
        thread_rng().fill_bytes(&mut buf);
        let rng_str = HEXLOWER.encode(&buf);
        let db = format!("db_{}", rng_str);
        shim_db
            .execute_unprepared(&format!("CREATE DATABASE {}", db))
            .await?;
        drop(shim_db);
        let db = Database::connect(format!("postgres://127.0.0.1/{}", db)).await?;
        Migrator::up(&db, None).await?;
        Ok(db)
    }

    #[tokio::test]
    async fn nominal() {
        let db = create_standalone_db().await.unwrap();
        let store_id = create_test_store(&db).await.unwrap();
        let api_key = create_insecure_api_key(&db).await.unwrap();
        let blob_id = create_fake_blob(&db, store_id.clone()).await.unwrap();
        let config = create_config(store_id.clone());
        let db = Arc::new(db);
        let stores = activate_stores(db.clone()).await;
        let system_load = Arc::new(RwLock::new(0.0));
        let mut router = create_router(db.clone(), Arc::new(config), &stores, system_load);

        // Non-existant route should 404
        let res = router
            .call(Request::builder().uri("/").body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(res.status(), StatusCode::NOT_FOUND);

        // Protected route without auth should 401
        let res = router
            .call(
                Request::builder()
                    .uri("/job")
                    .method(http::Method::POST)
                    .header(CONTENT_TYPE, "application/json")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::UNAUTHORIZED);

        // Missing/malformed body should 400
        let res = router
            .call(
                Request::builder()
                    .uri("/job")
                    .method(http::Method::POST)
                    .header(CONTENT_TYPE, "application/json")
                    .header(AUTHORIZATION, api_key.clone())
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::BAD_REQUEST);

        // Authorization check with invalid auth should 401
        let res = router
            .call(
                Request::builder()
                    .uri("/auth/check")
                    .method(http::Method::POST)
                    .header("Authorization", "badauth")
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::UNAUTHORIZED);

        // Authorization check with valid auth should 200
        let res = router
            .call(
                Request::builder()
                    .uri("/auth/check")
                    .method(http::Method::POST)
                    .header("Authorization", api_key.clone())
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::OK);

        // Correctly inserting a job should 200
        let res = router
            .call(
                Request::builder()
                    .uri("/job")
                    .method(http::Method::POST)
                    .header(CONTENT_TYPE, "application/json")
                    .header(AUTHORIZATION, api_key.clone())
                    .body(Body::from(
                        serde_json::to_vec(&json!({
                            "cmd": b"blarg",
                            "env": b"PATH=/usr/bin",
                            "cwd":"/workspace",
                            "stdin":"",
                            "is_atty": false,
                            "hidden_info":"",
                            "visible_files": [],
                            "output_dirs": [],
                            "output_symlinks": [],
                            "output_files":[],
                            "stdout_blob_id": blob_id,
                            "stderr_blob_id": blob_id,
                            "status": 0,
                            "runtime":1.0,
                            "cputime":1.0,
                            "memory": 1000,
                            "ibytes":100000,
                            "obytes":1000
                        }))
                        .unwrap(),
                    ))
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::OK);

        // Non-matching job should 404 with expected body
        let res = router
            .call(
                Request::builder()
                    .uri("/job/matching")
                    .method(http::Method::POST)
                    .header(CONTENT_TYPE, "application/json")
                    .body(Body::from(
                        serde_json::to_vec(&json!({
                            "cmd": b"blrg",
                            "env": b"PATH=/usr/bin",
                            "cwd":"/workspace",
                            "stdin":"",
                            "is_atty": false,
                            "hidden_info":"",
                            "visible_files": []
                        }))
                        .unwrap(),
                    ))
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::NOT_FOUND);

        let body = hyper::body::to_bytes(res).await.unwrap();
        let body: Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(body, json!({ "type": "NoMatch" }));

        // Matching job should 200 with expected body
        let res = router
            .call(
                Request::builder()
                    .uri("/job/matching")
                    .method(http::Method::POST)
                    .header(CONTENT_TYPE, "application/json")
                    .body(Body::from(
                        serde_json::to_vec(&json!({
                            "cmd": b"blarg",
                            "env": b"PATH=/usr/bin",
                            "cwd":"/workspace",
                            "stdin":"",
                            "is_atty": false,
                            "hidden_info":"",
                            "visible_files": []
                        }))
                        .unwrap(),
                    ))
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::OK);

        let body = hyper::body::to_bytes(res).await.unwrap();
        let body: Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(
            body,
            json!({
                "type": "Match",
                "output_dirs": [],
                "output_symlinks": [],
                "output_files":[],
                "stdout_blob": {
                    "id": blob_id,
                    "url": format!("test://{0}/InsecureKey", store_id),
                },
                "stderr_blob": {
                    "id": blob_id,
                    "url": format!("test://{0}/InsecureKey", store_id),
                },
                "status": 0,
                "runtime":1.0,
                "cputime":1.0,
                "memory": 1000,
                "ibytes":100000,
                "obytes":1000
            })
        );

        // Protected post blob route without auth should 401
        let body_data: Vec<u8> = Vec::new();
        let res = router
            .call(
                Request::builder()
                    .uri("/blob")
                    .method(http::Method::POST)
                    .header(
                        CONTENT_TYPE,
                        format!("multipart/form-data; boundary={}", BOUNDARY),
                    )
                    .body(body_data.into())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::UNAUTHORIZED);

        // Protected post blob route with auth should 200
        let mut body_data: Vec<u8> = Vec::new();
        write!(body_data, "--{}\r\n", BOUNDARY).unwrap();
        write!(
            body_data,
            "Content-Disposition: form-data; name=\"file\";\r\n"
        )
        .unwrap();
        write!(body_data, "\r\n").unwrap();
        write!(body_data, "contents").unwrap();
        write!(body_data, "\r\n").unwrap();
        write!(body_data, "--{}--\r\n", BOUNDARY).unwrap();

        let res = router
            .call(
                Request::builder()
                    .uri("/blob")
                    .method(http::Method::POST)
                    .header(
                        CONTENT_TYPE,
                        format!("multipart/form-data; boundary={}", BOUNDARY),
                    )
                    .header(AUTHORIZATION, api_key)
                    .body(body_data.into())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::OK);

        // Allowed version should should 200
        let res = router
            .call(
                Request::builder()
                    .uri("/version/check?version=sifive/wake/1.2.3")
                    .method(http::Method::GET)
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::OK);

        // Disallowed version should should 403
        let res = router
            .call(
                Request::builder()
                    .uri("/version/check?version=sifive/foo/1.2.3")
                    .method(http::Method::GET)
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();

        assert_eq!(res.status(), StatusCode::FORBIDDEN);
    }

    #[tokio::test]
    async fn ttl_eviction() {
        let db = create_standalone_db().await.unwrap();
        let store_id = create_test_store(&db).await.unwrap();
        let blob_id = create_fake_blob(&db, store_id).await.unwrap();
        let conn = Arc::new(db);

        // Create a job that is 5 days old
        let insert_job = entity::job::ActiveModel {
            id: NotSet,
            created_at: Set((Utc::now() - Duration::days(5)).naive_utc()),
            hash: Set("00000000".to_string()),
            cmd: Set("blarg".into()),
            env: Set("PATH=/usr/bin".as_bytes().into()),
            cwd: Set("/workspace".into()),
            stdin: Set("".into()),
            is_atty: Set(false),
            hidden_info: Set("".into()),
            stdout_blob_id: Set(blob_id),
            stderr_blob_id: Set(blob_id),
            status: Set(0),
            runtime: Set(1.0),
            cputime: Set(1.0),
            memory: Set(1000),
            i_bytes: Set(100000),
            o_bytes: Set(1000),
            label: Set("".to_string()),
            size: NotSet,
        };

        insert_job.save(conn.clone().as_ref()).await.unwrap();

        // Create a job that is 1 day old
        let insert_job = entity::job::ActiveModel {
            id: NotSet,
            created_at: Set((Utc::now() - Duration::days(1)).naive_utc()),
            hash: Set("00000001".to_string()),
            cmd: Set("blarg2".into()),
            env: Set("PATH=/usr/bin".as_bytes().into()),
            cwd: Set("/workspace".into()),
            stdin: Set("".into()),
            is_atty: Set(false),
            hidden_info: Set("".into()),
            stdout_blob_id: Set(blob_id),
            stderr_blob_id: Set(blob_id),
            status: Set(0),
            runtime: Set(1.0),
            cputime: Set(1.0),
            memory: Set(1000),
            i_bytes: Set(100000),
            o_bytes: Set(1000),
            label: Set("".to_string()),
            size: NotSet,
        };

        insert_job.save(conn.clone().as_ref()).await.unwrap();

        let count = entity::job::Entity::find()
            .count(conn.clone().as_ref())
            .await
            .unwrap();
        assert_eq!(count, 2);

        // Setup eviction for jobs older than 3 days ticking every second
        launch_job_eviction(conn.clone(), 1, 60 * 60 * 24 * 3);
        tokio::time::sleep(tokio::time::Duration::from_millis(2000)).await;

        let count = entity::job::Entity::find()
            .count(conn.clone().as_ref())
            .await
            .unwrap();
        assert_eq!(count, 1);
    }
}
