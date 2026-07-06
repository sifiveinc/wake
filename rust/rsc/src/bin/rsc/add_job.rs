use crate::types::{AddJobPayload, JobKeyHash};
use axum::{http::StatusCode, Json};

use entity::{job, output_dir, output_file, output_symlink};

use sea_orm::{ActiveModelTrait, ActiveValue::*, DatabaseConnection, DbErr, TransactionTrait};
use std::sync::Arc;
use tracing;

use rsc::database;

#[tracing::instrument(skip_all)]
pub async fn add_job(
    Json(payload): Json<AddJobPayload>,
    conn: Arc<DatabaseConnection>,
) -> StatusCode {
    // First construct all the job details as an ActiveModel for insert
    let hash = payload.hash();
    tracing::info!(hash);

    let output_files = payload.output_files;
    let output_symlinks = payload.output_symlinks;
    let output_dirs = payload.output_dirs;
    let insert_job = job::ActiveModel {
        id: NotSet,
        created_at: NotSet,
        hash: Set(hash.clone().into()),
        cmd: Set(payload.cmd),
        env: Set(payload.env),
        cwd: Set(payload.cwd),
        stdin: Set(payload.stdin),
        is_atty: Set(payload.is_atty),
        hidden_info: Set(payload.hidden_info),
        stdout_blob_id: Set(payload.stdout_blob_id),
        stderr_blob_id: Set(payload.stderr_blob_id),
        status: Set(payload.status),
        runtime: Set(payload.runtime),
        cputime: Set(payload.cputime),
        memory: Set(payload.memory as i64),
        i_bytes: Set(payload.ibytes as i64),
        o_bytes: Set(payload.obytes as i64),
        label: Set(payload.label.unwrap_or("".to_string())),
        size: NotSet,
    };

    // Now perform the insert as a single transaction
    let insert_result = conn
        .as_ref()
        .transaction::<_, (), DbErr>(|txn| {
            Box::pin(async move {
                let job = insert_job.save(txn).await?;
                let job_id = job.id.unwrap();

                database::create_many_output_files(
                    txn,
                    output_files
                        .into_iter()
                        .map(|out_file| output_file::ActiveModel {
                            id: NotSet,
                            created_at: NotSet,
                            path: Set(out_file.path),
                            mode: Set(out_file.mode),
                            job_id: Set(job_id),
                            blob_id: Set(out_file.blob_id),
                        })
                        .collect(),
                )
                .await?;

                database::create_many_output_symlinks(
                    txn,
                    output_symlinks
                        .into_iter()
                        .map(|out_symlink| output_symlink::ActiveModel {
                            id: NotSet,
                            created_at: NotSet,
                            path: Set(out_symlink.path),
                            link: Set(out_symlink.link),
                            job_id: Set(job_id),
                            hash: Set(out_symlink.hash.unwrap_or_default()),
                        })
                        .collect(),
                )
                .await?;

                database::create_many_output_dirs(
                    txn,
                    output_dirs
                        .into_iter()
                        .map(|dir| output_dir::ActiveModel {
                            id: NotSet,
                            created_at: NotSet,
                            path: Set(dir.path),
                            mode: Set(dir.mode),
                            hidden: Set(dir.hidden.unwrap_or(false)),
                            job_id: Set(job_id),
                        })
                        .collect(),
                )
                .await?;

                Ok(())
            })
        })
        .await;
    match insert_result {
        Ok(_) => StatusCode::OK,
        // TODO: We should returnn 500 on some errors
        //       but 4** when the user trys to add an invalid job
        Err(cause) => {
            tracing::error! {
              %cause,
              "failed to add job"
            };
            StatusCode::BAD_REQUEST
        }
    }
}
