use anyhow::{anyhow, Context, Result};
use serde::{Deserialize, Serialize};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::{Component, Path, PathBuf};

pub const DEFAULT_READ_LIMIT: usize = 64 * 1024;
pub const MAX_READ_LIMIT: usize = 1024 * 1024;
const MAX_DIRECTORY_ENTRIES: usize = 2_000;

/// A read-only, path-confined view of the artifacts belonging to one Wake database.
#[derive(Clone, Debug)]
pub struct ArtifactRoot {
    root: PathBuf,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ArtifactEntry {
    pub name: String,
    pub kind: String,
    pub size: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ArtifactInspection {
    pub uri: String,
    pub kind: String,
    pub size: u64,
    pub offset: u64,
    pub content: Option<String>,
    pub entries: Vec<ArtifactEntry>,
    pub truncated: bool,
}

impl ArtifactRoot {
    pub fn new(root: impl Into<PathBuf>) -> Result<Self> {
        let root = root.into();
        let root = fs::canonicalize(&root)
            .with_context(|| format!("resolving artifact root {}", root.display()))?;
        if !root.is_dir() {
            return Err(anyhow!(
                "artifact root is not a directory: {}",
                root.display()
            ));
        }
        Ok(Self { root })
    }

    pub fn path(&self) -> &Path {
        &self.root
    }

    fn resolve(&self, path: &str) -> Result<PathBuf> {
        let relative = Path::new(path);
        if relative.as_os_str().is_empty()
            || relative.is_absolute()
            || relative.components().any(|component| {
                matches!(
                    component,
                    Component::ParentDir | Component::RootDir | Component::Prefix(_)
                )
            })
        {
            return Err(anyhow!("artifact path must be a non-empty relative path"));
        }
        let resolved = fs::canonicalize(self.root.join(relative))
            .with_context(|| format!("resolving artifact {path}"))?;
        if !resolved.starts_with(&self.root) {
            return Err(anyhow!("artifact path escapes the configured root"));
        }
        Ok(resolved)
    }

    pub fn inspect(
        &self,
        source_id: &str,
        path: &str,
        offset: u64,
        limit: usize,
    ) -> Result<ArtifactInspection> {
        let resolved = self.resolve(path)?;
        let metadata =
            fs::metadata(&resolved).with_context(|| format!("reading artifact {path}"))?;
        let uri = format!("wake://{source_id}/{}", path.trim_start_matches('/'));
        let limit = limit.clamp(1, MAX_READ_LIMIT);

        if metadata.is_file() {
            let size = metadata.len();
            let mut file =
                File::open(&resolved).with_context(|| format!("opening artifact {path}"))?;
            let offset = offset.min(size);
            file.seek(SeekFrom::Start(offset))?;
            let mut bytes = Vec::with_capacity(limit.min(size.saturating_sub(offset) as usize));
            file.take(limit as u64).read_to_end(&mut bytes)?;
            return Ok(ArtifactInspection {
                uri,
                kind: "file".to_owned(),
                size,
                offset,
                truncated: offset.saturating_add(bytes.len() as u64) < size,
                content: Some(String::from_utf8_lossy(&bytes).into_owned()),
                entries: Vec::new(),
            });
        }

        if metadata.is_dir() {
            let mut entries = fs::read_dir(&resolved)?
                .map(|entry| {
                    let entry = entry?;
                    let file_type = entry.file_type()?;
                    let metadata = fs::symlink_metadata(entry.path())?;
                    let kind = if file_type.is_symlink() {
                        "symlink"
                    } else if file_type.is_dir() {
                        "directory"
                    } else if file_type.is_file() {
                        "file"
                    } else {
                        "other"
                    };
                    Ok(ArtifactEntry {
                        name: entry.file_name().to_string_lossy().into_owned(),
                        kind: kind.to_owned(),
                        size: metadata.len(),
                    })
                })
                .collect::<std::io::Result<Vec<_>>>()?;
            entries.sort_by(|left, right| left.name.cmp(&right.name));
            let truncated = entries.len() > MAX_DIRECTORY_ENTRIES;
            entries.truncate(MAX_DIRECTORY_ENTRIES);
            return Ok(ArtifactInspection {
                uri,
                kind: "directory".to_owned(),
                size: metadata.len(),
                offset: 0,
                content: None,
                entries,
                truncated,
            });
        }

        Ok(ArtifactInspection {
            uri,
            kind: "other".to_owned(),
            size: metadata.len(),
            offset: 0,
            content: None,
            entries: Vec::new(),
            truncated: false,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temporary_root() -> PathBuf {
        let suffix = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("wake-artifacts-{suffix}"));
        fs::create_dir(&root).unwrap();
        root
    }

    #[test]
    fn reads_bounded_artifacts_and_builds_virtual_uri() {
        let root = temporary_root();
        fs::write(root.join("output.txt"), b"abcdef").unwrap();
        let artifacts = ArtifactRoot::new(&root).unwrap();
        let inspection = artifacts.inspect("gpu-a", "output.txt", 2, 3).unwrap();
        assert_eq!(inspection.uri, "wake://gpu-a/output.txt");
        assert_eq!(inspection.content.as_deref(), Some("cde"));
        assert!(inspection.truncated);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_parent_and_absolute_paths() {
        let root = temporary_root();
        let artifacts = ArtifactRoot::new(&root).unwrap();
        assert!(artifacts.inspect("local", "../secret", 0, 10).is_err());
        assert!(artifacts.inspect("local", "/etc/passwd", 0, 10).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn rejects_symlinks_that_escape_the_root() {
        use std::os::unix::fs::symlink;
        let root = temporary_root();
        symlink("/etc/passwd", root.join("escape")).unwrap();
        let artifacts = ArtifactRoot::new(&root).unwrap();
        assert!(artifacts.inspect("local", "escape", 0, 10).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
