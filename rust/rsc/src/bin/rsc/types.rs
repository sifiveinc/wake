use blake3;
use sea_orm::prelude::DateTime;
use sea_orm::prelude::Uuid;
use serde::{Deserialize, Serialize};

// Note:
//   When changing these types, remember to review database.rs for max parameter queries.
//   Without the implementation of a macro, we need to manually maintain the number of
//   parameters in each type.

pub trait JobKeyHash {
    fn cmd(&self) -> &Vec<u8>;
    fn env(&self) -> &Vec<u8>;
    fn cwd(&self) -> &String;
    fn stdin(&self) -> &String;
    fn is_atty(&self) -> bool;
    fn hidden_info(&self) -> &String;
    fn visible_files(&self) -> &Vec<VisibleFile>;

    fn hash(&self) -> String {
        let mut hasher = blake3::Hasher::new();
        hasher.update(&self.cmd().len().to_le_bytes());
        hasher.update(self.cmd());
        hasher.update(&self.env().len().to_le_bytes());
        hasher.update(self.env());
        hasher.update(&self.cwd().len().to_le_bytes());
        hasher.update(self.cwd().as_bytes());
        hasher.update(&self.stdin().len().to_le_bytes());
        hasher.update(self.stdin().as_bytes());
        hasher.update(&self.hidden_info().len().to_le_bytes());
        hasher.update(self.hidden_info().as_bytes());
        hasher.update(&[self.is_atty() as u8]);
        hasher.update(&self.visible_files().len().to_le_bytes());
        for file in self.visible_files() {
            hasher.update(&file.path.len().to_le_bytes());
            hasher.update(file.path.as_bytes());
            hasher.update(&file.hash.len().to_le_bytes());
            hasher.update(file.hash.as_bytes());
        }
        hasher.finalize().to_string()
    }
}

#[derive(Debug, Deserialize, Serialize)]
pub struct VisibleFile {
    pub path: String,
    pub hash: String,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct File {
    pub path: String,
    pub mode: i32,
    pub blob_id: Uuid,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct Dir {
    pub path: String,
    pub mode: i32,
    // Optional member to allow for soft migration
    pub hidden: Option<bool>,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct Symlink {
    pub path: String,
    pub link: String,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct AddJobPayload {
    pub cmd: Vec<u8>,
    pub env: Vec<u8>,
    pub cwd: String,
    pub stdin: String,
    pub is_atty: bool,
    pub hidden_info: String,
    pub visible_files: Vec<VisibleFile>,
    pub output_dirs: Vec<Dir>,
    pub output_symlinks: Vec<Symlink>,
    pub output_files: Vec<File>,
    pub stdout_blob_id: Uuid,
    pub stderr_blob_id: Uuid,
    pub status: i32,
    pub runtime: f64,
    pub cputime: f64,
    pub memory: u64,
    pub ibytes: u64,
    pub obytes: u64,

    // Label is not part of the job key and is not considered in any caching decisions. It is
    // strictly used for inspecting the remote cache's database. Left as optional for soft migration
    // purposes. May become required in the future
    pub label: Option<String>,
}

impl JobKeyHash for AddJobPayload {
    fn cmd(&self) -> &Vec<u8> {
        &self.cmd
    }

    fn env(&self) -> &Vec<u8> {
        &self.env
    }

    fn cwd(&self) -> &String {
        &self.cwd
    }

    fn stdin(&self) -> &String {
        &self.stdin
    }

    fn is_atty(&self) -> bool {
        self.is_atty
    }

    fn hidden_info(&self) -> &String {
        &self.hidden_info
    }

    fn visible_files(&self) -> &Vec<VisibleFile> {
        &self.visible_files
    }
}

#[derive(Debug, Deserialize)]
pub struct ReadJobPayload {
    pub cmd: Vec<u8>,
    pub env: Vec<u8>,
    pub cwd: String,
    pub stdin: String,
    pub is_atty: bool,
    pub hidden_info: String,
    pub visible_files: Vec<VisibleFile>,
}

impl JobKeyHash for ReadJobPayload {
    fn cmd(&self) -> &Vec<u8> {
        &self.cmd
    }

    fn env(&self) -> &Vec<u8> {
        &self.env
    }

    fn cwd(&self) -> &String {
        &self.cwd
    }

    fn stdin(&self) -> &String {
        &self.stdin
    }

    fn is_atty(&self) -> bool {
        self.is_atty
    }

    fn hidden_info(&self) -> &String {
        &self.hidden_info
    }

    fn visible_files(&self) -> &Vec<VisibleFile> {
        &self.visible_files
    }
}

#[derive(Debug, Deserialize, Serialize)]
pub struct AllowJobPayload {
    pub cmd: Vec<u8>,
    pub env: Vec<u8>,
    pub cwd: String,
    pub stdin: String,
    pub is_atty: bool,
    pub hidden_info: String,
    pub visible_files: Vec<VisibleFile>,
    // Above is the job key and required.
    // Below is optional and used for determining allowability
    pub status: i32,
    pub runtime: f64,
    pub cputime: f64,
    pub memory: u64,
    pub obytes: u64,
    pub label: String,
}

impl JobKeyHash for AllowJobPayload {
    fn cmd(&self) -> &Vec<u8> {
        &self.cmd
    }

    fn env(&self) -> &Vec<u8> {
        &self.env
    }

    fn cwd(&self) -> &String {
        &self.cwd
    }

    fn stdin(&self) -> &String {
        &self.stdin
    }

    fn is_atty(&self) -> bool {
        self.is_atty
    }

    fn hidden_info(&self) -> &String {
        &self.hidden_info
    }

    fn visible_files(&self) -> &Vec<VisibleFile> {
        &self.visible_files
    }
}

#[derive(Debug, Serialize, Deserialize)]
pub struct PostBlobResponsePart {
    pub id: Uuid,
    pub name: String,
    pub content_hash: String,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum PostBlobResponse {
    Error { message: String },
    Ok { blobs: Vec<PostBlobResponsePart> },
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ResolvedBlob {
    pub id: Uuid,
    pub url: String,
    pub content_hash: String,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct ResolvedBlobFile {
    pub path: String,
    pub mode: i32,
    pub blob: ResolvedBlob,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ReadJobResponse {
    NoMatch,
    Match {
        output_symlinks: Vec<Symlink>,
        output_dirs: Vec<Dir>,
        output_files: Vec<ResolvedBlobFile>,
        stdout_blob: ResolvedBlob,
        stderr_blob: ResolvedBlob,
        content_hash: String,
        status: i32,
        runtime: f64,
        cputime: f64,
        memory: u64,
        ibytes: u64,
        obytes: u64,
    },
}

#[derive(Debug, Serialize, Deserialize)]
pub struct GetUploadUrlResponse {
    pub url: String,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct DashboardStatsOldestJob {
    pub label: String,
    pub created_at: DateTime,
    pub reuses: i32,
    pub savings: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct DashboardStatsMostReusedJob {
    pub label: String,
    pub reuses: i32,
    pub savings: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct DashboardStatsLostOpportunityJob {
    pub label: String,
    pub reuses: i32,
    pub misses: i32,
    pub real_savings: i64,
    pub lost_savings: i64,
    pub potential_savings: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct DashboardStatsSizeRuntimeValueJob {
    pub label: String,
    pub runtime: i64,
    pub disk_usage: i64,
    pub ns_saved_per_byte: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct DashboardStatsBlobUseByStore {
    pub store_id: String,
    pub store_type: String,
    pub refs: i64,
    pub blob_count: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct DashboardStatsResponse {
    pub job_count: u64,
    pub blob_count: u64,
    pub size: i64,
    pub savings: i64,
    pub oldest_jobs: Vec<DashboardStatsOldestJob>,
    pub most_reused_jobs: Vec<DashboardStatsMostReusedJob>,
    pub most_time_saved_jobs: Vec<DashboardStatsMostReusedJob>,
    pub lost_opportunity_jobs: Vec<DashboardStatsLostOpportunityJob>,
    pub most_space_efficient_jobs: Vec<DashboardStatsSizeRuntimeValueJob>,
    pub most_space_use_jobs: Vec<DashboardStatsSizeRuntimeValueJob>,
    pub blob_use_by_store: Vec<DashboardStatsBlobUseByStore>,
}
