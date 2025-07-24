use crate::blob;
use crate::types::{
    AllowJobPayload, Dir, JobKeyHash, ReadJobPayload, ReadJobResponse, ResolvedBlob,
    ResolvedBlobFile, Symlink,
};
use axum::Json;
use entity::{job, job_use, output_dir, output_file, output_symlink};
use entity::prelude::Blob;
use futures::future::join_all;
use hyper::StatusCode;
use rand::{thread_rng, Rng};
use rsc::database;
use sea_orm::{
    prelude::Uuid, ActiveModelTrait, ActiveValue::*, ColumnTrait, DatabaseConnection, DbErr,
    EntityTrait, ModelTrait, QueryFilter, TransactionTrait, ConnectionTrait,
};
use std::collections::HashMap;
use std::sync::{Arc, RwLock};
use tracing;
use std::time::Instant;
use lazy_static::lazy_static;
use prometheus::{register_counter, register_histogram, Counter, Histogram};

lazy_static! {
    /// Counts how many cache hits we've had.
    pub static ref CACHE_HITS: Counter = register_counter!(
        "cache_hits",
        "Number of cache hits"
    ).unwrap();

    /// Counts how many cache misses we've had.
    pub static ref CACHE_MISSES: Counter = register_counter!(
        "cache_misses",
        "Number of cache misses"
    ).unwrap();

    /// Tracks latencies (in milliseconds) for read_job requests.
    pub static ref HIT_LATENCY_MS: Histogram = register_histogram!(
        "hit_latency_ms",
        "Histogram of cache hit latencies in milliseconds"
    ).unwrap();

    pub static ref MISS_LATENCY_MS: Histogram = register_histogram!(
        "miss_latency_ms",
        "Histogram of cache miss latencies in milliseconds"
    ).unwrap();
}


#[tracing::instrument(skip_all)]
fn update_hit_counters(start_time: Instant) {
    CACHE_HITS.inc();
    let elapsed_ms = start_time.elapsed().as_millis() as f64;
    HIT_LATENCY_MS.observe(elapsed_ms);
}

#[tracing::instrument(skip_all)]
fn update_miss_counters(start_time: Instant) {
    CACHE_MISSES.inc();
    let elapsed_ms = start_time.elapsed().as_millis() as f64;
    MISS_LATENCY_MS.observe(elapsed_ms);
}

#[tracing::instrument(skip(hash, conn))]
async fn record_hit(job_id: Uuid, hash: String, conn: Arc<DatabaseConnection>) {
    let usage = job_use::ActiveModel {
        id: NotSet,
        created_at: NotSet,
        job_id: Set(job_id),
    };
    let _ = usage.insert(conn.as_ref()).await;
    let _ = database::record_job_hit(conn.as_ref(), hash).await;
}

#[tracing::instrument(skip(hash, conn))]
async fn record_miss(hash: String, conn: Arc<DatabaseConnection>) {
    let _ = database::record_job_miss(conn.as_ref(), hash).await;
}

#[tracing::instrument(skip(db, stores))]
async fn resolve_blobs<T: ConnectionTrait>(
    ids: &Vec<Uuid>,
    db: &T,
    stores: &HashMap<Uuid, Arc<dyn blob::DebugBlobStore + Sync + Send>>,
) -> Result<HashMap<Uuid, ResolvedBlob>, String> {
    //Postgres has a 65,535 parameter limit, ensuring we chunk ID's below that: https://www.postgresql.org/docs/current/limits.html
    const CHUNK_SIZE: usize = 50_000;

    let mut resolved_map = HashMap::new();
    
    for chunk in ids.chunks(CHUNK_SIZE) {
        // Fetch chunked blobs in a single query
        let blob_map: HashMap<Uuid, entity::blob::Model> = Blob::find()
            .filter(entity::blob::Column::Id.is_in(chunk.to_vec()))
            .all(db)
            .await
            .map_err(|e| format!("Failed to query blobs, database error: {}", e))?
            .into_iter()
            .map(|b| (b.id, b))
            .collect();

        // Ensure we have all requested blobs
        for &id in chunk {
            if !blob_map.contains_key(&id) {
                return Err(format!("Unable to find blob {} by id", id));
            }
        }

        // Resolve all download URLs in parallel
        let futures = blob_map.iter().map(|(id, blob)| {
            let store_opt = stores.get(&blob.store_id).cloned();
            let key = blob.key.clone();

            async move {
                let store = store_opt.ok_or_else(|| {
                    format!("Unable to find backing store {} for blob {}", blob.store_id, id)
                })?;
                let url = store.download_url(key).await;
                Ok::<(Uuid, ResolvedBlob), String>((*id, ResolvedBlob { id: *id, url }))
            }
        });

        let results = join_all(futures).await;

        let partial_map: HashMap<Uuid, ResolvedBlob> = results.into_iter().collect::<Result<_,_>>()?;
        resolved_map.extend(partial_map);
    }

    Ok(resolved_map)
}

#[tracing::instrument(skip_all)]
pub async fn read_job(
    Json(payload): Json<ReadJobPayload>,
    conn: Arc<DatabaseConnection>,
    blob_stores: HashMap<Uuid, Arc<dyn blob::DebugBlobStore + Sync + Send>>,
) -> (StatusCode, Json<ReadJobResponse>) {
    let start = Instant::now();

    let hash = payload.hash();
    let hash_for_spawns = hash.clone();

    // Fetch the job and related entities in a single transaction
    let fetch_result = conn
        .as_ref()
        .transaction::<_, Option<(job::Model, Vec<output_file::Model>, Vec<output_symlink::Model>, Vec<output_dir::Model>)>, DbErr>(|txn| {
            Box::pin(async move {
                let Some(matching_job) = job::Entity::find()
                    .filter(job::Column::Hash.eq(hash.clone()))
                    .one(txn)
                    .await?
                else {
                    tracing::info!(%hash, "Miss");
                    return Ok(None);
                };
            
                let output_files = matching_job.find_related(output_file::Entity).all(txn).await?;
                let output_symlinks = matching_job.find_related(output_symlink::Entity).all(txn).await?;
                let output_dirs = matching_job.find_related(output_dir::Entity).all(txn).await?;

                Ok(Some((matching_job, output_files, output_symlinks, output_dirs)))
            })
        })
        .await;
    
    let hash_copy = hash_for_spawns.clone();
    let Some((matching_job, output_files, output_symlinks, output_dirs)) = fetch_result.ok().flatten() else {
        tokio::spawn(async move {
            record_miss(hash_copy, conn.clone()).await;
        });
        
        update_miss_counters(start);

        return (StatusCode::NOT_FOUND, Json(ReadJobResponse::NoMatch));
    };

    // Collect all the blob IDs we need to resolve
    let mut blob_ids: Vec<Uuid> = output_files.iter().map(|f| f.blob_id).collect();
    blob_ids.push(matching_job.stdout_blob_id);
    blob_ids.push(matching_job.stderr_blob_id);

    // Resolve all needed blobs in one go
    let resolved_blob_map = match resolve_blobs(&blob_ids, conn.as_ref(), &blob_stores).await {
        Ok(map) => map,
        Err(err) => {
            tracing::error!(%err, "Failed to resolve blobs. Resolving job as a cache miss.");

            update_miss_counters(start);

            return (StatusCode::NOT_FOUND, Json(ReadJobResponse::NoMatch));
        }
    };

    // Construct ResolvedBlobFile for each output file
    let output_files = output_files
        .into_iter()
        .map(|m| {
            let blob_id = m.blob_id;
            let job_id = m.job_id;
            let resolved_blob = resolved_blob_map.get(&blob_id).cloned().ok_or_else(|| {
                format!("Missing resolved blob for {}, from job_id: {}", blob_id, job_id)
            })?;
            Ok(ResolvedBlobFile {
                path: m.path,
                mode: m.mode,
                blob: resolved_blob,
            })
        })
        .collect::<Result<Vec<_>, String>>();

    let output_files = match output_files {
        Ok(files) => files,
        Err(err) => {
            tracing::error!(%err, "Failed to resolve all output files. Resolving job as a cache miss.");
            
            update_miss_counters(start);
            
            return (StatusCode::NOT_FOUND, Json(ReadJobResponse::NoMatch));
        }
    };

    // Collect other resolved entities
    let output_symlinks: Vec<Symlink> = output_symlinks
        .into_iter()
        .map(|m| Symlink {
            path: m.path,
            link: m.link,
        })
        .collect();

    let output_dirs: Vec<Dir> = output_dirs
        .into_iter()
        .map(|m| Dir {
            path: m.path,
            mode: m.mode,
            hidden: Some(m.hidden),
        })
        .collect();

    // Resolve stdout and stderr blobs from the map
    let stdout_blob = match resolved_blob_map.get(&matching_job.stdout_blob_id) {
        Some(blob) => blob.clone(),
        None => {
            tracing::error!("Failed to resolve stdout blob. Resolving job as a cache miss.");
            update_miss_counters(start);
            return (StatusCode::NOT_FOUND, Json(ReadJobResponse::NoMatch));
        }
    };

    let stderr_blob = match resolved_blob_map.get(&matching_job.stderr_blob_id) {
        Some(blob) => blob.clone(),
        None => {
            tracing::error!("Failed to resolve stderr blob. Resolving job as a cache miss.");
            update_miss_counters(start);
            return (StatusCode::NOT_FOUND, Json(ReadJobResponse::NoMatch));
        }
    };

    // Construct response
    let response = ReadJobResponse::Match {
        output_symlinks,
        output_dirs,
        output_files,
        stdout_blob,
        stderr_blob,
        status: matching_job.status,
        runtime: matching_job.runtime,
        cputime: matching_job.cputime,
        memory: matching_job.memory as u64,
        ibytes: matching_job.i_bytes as u64,
        obytes: matching_job.o_bytes as u64,
    };

    let job_id = matching_job.id;
    let hash_copy = hash_for_spawns.clone();
    tracing::info!(%hash_copy, "Hit");
    tokio::spawn(async move {
        record_hit(job_id, hash_copy, conn.clone()).await;
    });
    update_hit_counters(start);
    (StatusCode::OK, Json(response))
}


#[tracing::instrument(skip_all)]
pub async fn allow_job(
    Json(payload): Json<AllowJobPayload>,
    conn: Arc<DatabaseConnection>,
    target_load: f64,
    system_load: Arc<RwLock<f64>>,
    min_runtime: f64,
) -> StatusCode {
    let hash = payload.hash();

    // Reject a subset of jobs that are never worth caching
    if payload.runtime < min_runtime {
        let denied_hash = hash.clone();
        tokio::spawn(async move {
            let _ = database::record_job_denied(conn.as_ref(), denied_hash).await;
        });
        return StatusCode::NOT_ACCEPTABLE;
    }

    // Statistically reject jobs when the system should shed load
    let current_load = match system_load.read() {
        Ok(lock) => *lock,
        Err(err) => {
            tracing::error!(%err, "Unable to lock system load for reading. Returning load with 50% shed chance");
            target_load + target_load / 2.0
        }
    };

    // Determine the chance of shedding the job and clamp to 0.0-1.0
    let mut shed_chance = current_load / target_load - 1.0;
    shed_chance = f64::min(shed_chance, 1.0);
    shed_chance = f64::max(shed_chance, 0.0);

    // When under high load we expect that this route is being hit a lot. This has two effects
    // 1: The logs will get very spammy, 2: The act of logging will increase the load
    // This creates the opportunity of a feedback loop so we dampen the log rate based on the
    // current load. shed_chance = 0.5 log_chance = 0.51. shed_chance = 1.0, log_chance = 0.01
    if shed_chance > 0.5 && thread_rng().gen_bool(0.01 + (1.0 - shed_chance)) {
        tracing::warn!(%shed_chance, "Machine is highly loaded and more likely than not to shed a job");
    }

    if thread_rng().gen_bool(shed_chance) {
        let shed_hash = hash.clone();
        tokio::spawn(async move {
            let _ = database::record_job_shed(conn.as_ref(), shed_hash).await;
        });
        return StatusCode::TOO_MANY_REQUESTS;
    }

    // Reject jobs that are already cached
    match job::Entity::find()
        .filter(job::Column::Hash.eq(hash.clone()))
        .one(conn.as_ref())
        .await
    {
        // Unable to run the query to lookup the job
        Err(err) => {
            tracing::error!(%err, "Failed to search for cached job");
            StatusCode::INTERNAL_SERVER_ERROR
        }
        // Job is cached, don't try again
        Ok(Some(_)) => {
            tracing::warn!(%hash, "Rejecting job push for already cached job");
            tokio::spawn(async move {
                let _ = database::record_job_conflict(conn.as_ref(), hash).await;
            });
            StatusCode::CONFLICT
        }
        // Job is not cached, use the other deciding factors
        Ok(None) => StatusCode::OK,
    }
}
