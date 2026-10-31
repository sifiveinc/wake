use crate::blob::*;
use async_trait::async_trait;
use futures::stream::BoxStream;
use rand::{thread_rng, RngCore};
use sea_orm::prelude::Uuid;
use std::fmt::Write;
use tokio::fs::File;
use tokio::io::BufWriter;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio_util::bytes::Bytes;
use tokio_util::io::{ReaderStream, StreamReader};

fn create_random_blob_path() -> std::path::PathBuf {
    // 2 deep @ 8 bytes wide
    let mut parts = [0u8; 10];
    thread_rng().fill_bytes(&mut parts);

    let mut buf = std::path::PathBuf::from("");

    // First 2 bytes represent the containing directories
    for i in 0..2 {
        let mut s = String::new();
        write!(&mut s, "{:02X}", parts[i]).unwrap();
        buf.push(s);
    }

    // Next 8 bytes represent the file name
    let mut s = String::new();
    for i in 2..10 {
        write!(&mut s, "{:02X}", parts[i]).unwrap();
    }
    buf.push(s);

    return buf;
}

#[derive(Debug, Clone)]
pub struct LocalBlobStore {
    pub id: Uuid,
    pub root: String,
    pub public_base_url: Option<url::Url>,
}

#[async_trait]
impl BlobStore for LocalBlobStore {
    fn id(&self) -> Uuid {
        self.id
    }

    async fn stream<'a>(
        &self,
        stream: BoxStream<'a, Result<Bytes, std::io::Error>>,
    ) -> Result<(String, i64), std::io::Error> {
        let reader = StreamReader::new(stream);
        futures::pin_mut!(reader);

        let rel_path = create_random_blob_path();
        let path = std::path::Path::new(&self.root).join(rel_path.clone());
        tokio::fs::create_dir_all(path.parent().unwrap()).await?;

        // Keep the temporary file owned until all buffered writes have completed. Dropping the
        // upload future (or any error) removes it, including when the request is disconnected.
        let mut builder = tempfile::Builder::new();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            // Match File::create's permissions (filtered by umask) for legacy file:// readers.
            builder.permissions(std::fs::Permissions::from_mode(0o666));
        }
        let temporary = builder.tempfile_in(path.parent().unwrap())?;
        let mut file = BufWriter::new(File::from_std(temporary.reopen()?));
        let written = tokio::io::copy(&mut reader, &mut file).await?;
        file.flush().await?;
        file.get_ref().sync_all().await?;
        drop(file);
        // Atomically expose only the completed file, without overwriting an existing blob.
        temporary
            .persist_noclobber(&path)
            .map_err(|err| err.error)?;

        let size = match i64::try_from(written) {
            Err(err) => {
                tracing::error!(%err, %written, "Size overflows i64, setting to i64::MAX instead");
                i64::MAX
            }
            Ok(size) => size,
        };

        let key = match rel_path.into_os_string().into_string() {
            Err(path) => {
                tracing::error!("Cannot convert path to string, returning lossy path instead");
                path.to_string_lossy().to_string()
            }
            Ok(s) => s,
        };

        Ok((key, size))
    }

    async fn download_url(&self, id: Uuid, key: String) -> String {
        match &self.public_base_url {
            Some(base) => base
                .join(&format!("blob/{id}"))
                .expect("validated HTTP base URL")
                .to_string(),
            None => format!("file://{0}/{key}", self.root),
        }
    }

    async fn content(&self, key: String) -> Result<BlobContent, std::io::Error> {
        use std::io::{Error, ErrorKind};
        use std::path::{Component, Path};

        // Only database keys are accepted, never request paths. Also reject invalid stored keys
        // and symlinks that would escape the local store's root.
        if key.is_empty()
            || Path::new(&key)
                .components()
                .any(|part| !matches!(part, Component::Normal(_)))
        {
            return Err(Error::new(ErrorKind::InvalidData, "Invalid local blob key"));
        }
        let root = tokio::fs::canonicalize(&self.root).await?;
        let path = tokio::fs::canonicalize(root.join(key)).await?;
        if !path.starts_with(&root) {
            return Err(Error::new(
                ErrorKind::InvalidData,
                "Blob key escapes local store",
            ));
        }
        let file = File::open(path).await?;
        let metadata = file.metadata().await?;
        if !metadata.is_file() {
            return Err(Error::new(
                ErrorKind::InvalidData,
                "Blob is not a regular file",
            ));
        }
        Ok(BlobContent {
            size: metadata.len(),
            stream: Box::pin(ReaderStream::new(file)),
        })
    }

    async fn delete_key(&self, key: String) -> Result<(), std::io::Error> {
        let path = std::path::Path::new(&self.root).join(key);
        tokio::fs::remove_file(path).await
    }
}

impl DebugBlobStore for LocalBlobStore {}

#[derive(Debug, Clone)]
pub struct TestBlobStore {
    pub id: Uuid,
}

#[async_trait]
impl BlobStore for TestBlobStore {
    fn id(&self) -> Uuid {
        self.id
    }

    async fn stream<'a>(
        &self,
        _stream: BoxStream<'a, Result<Bytes, std::io::Error>>,
    ) -> Result<(String, i64), std::io::Error> {
        Ok(("TestTestTest".to_string(), 0xDEADBEEF))
    }

    async fn download_url(&self, _id: Uuid, key: String) -> String {
        return format!("test://{0}/{key}", self.id);
    }

    async fn delete_key(&self, _key: String) -> Result<(), std::io::Error> {
        Ok(())
    }
}

impl DebugBlobStore for TestBlobStore {}

#[derive(Debug, Clone)]
pub struct DbOnlyBlobStore {
    pub id: Uuid,
}

#[async_trait]
impl BlobStore for DbOnlyBlobStore {
    fn id(&self) -> Uuid {
        self.id
    }

    async fn stream<'a>(
        &self,
        stream: BoxStream<'a, Result<Bytes, std::io::Error>>,
    ) -> Result<(String, i64), std::io::Error> {
        let reader = StreamReader::new(stream);
        futures::pin_mut!(reader);
        let mut buffer = [0u8; 101];
        let mut count = 0;

        loop {
            // If we have filled the buffer then the request has exceeded the max.
            if count == 101 {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    "DbOnlyBlobStore only supports up to 100 byte blobs",
                ));
            }

            let n = reader.read(&mut buffer[count..]).await?;
            if n == 0 {
                break;
            }

            count += n;
        }

        // Safe to unwrap here since count is stopped at 101
        let size: i64 = count.try_into().unwrap();

        Ok((
            buffer[0..count]
                .iter()
                .map(|x| format!("%{:02X}", x))
                .collect::<Vec<_>>()
                .concat(),
            size,
        ))
    }

    async fn download_url(&self, _id: Uuid, key: String) -> String {
        return format!("db://{key}");
    }

    async fn delete_key(&self, _key: String) -> Result<(), std::io::Error> {
        // Nothing outside of the db to delete
        Ok(())
    }
}

impl DebugBlobStore for DbOnlyBlobStore {}

#[cfg(test)]
mod tests {
    use super::*;
    use futures::{stream, StreamExt, TryStreamExt};
    use std::io::{Error, ErrorKind};

    fn local(root: &tempfile::TempDir) -> LocalBlobStore {
        LocalBlobStore {
            id: Uuid::from_u128(rand::random()),
            root: root.path().to_str().unwrap().to_string(),
            public_base_url: None,
        }
    }

    fn files(root: &std::path::Path) -> Vec<std::path::PathBuf> {
        std::fs::read_dir(root)
            .unwrap()
            .flat_map(|entry| {
                let path = entry.unwrap().path();
                if path.is_dir() {
                    files(&path)
                } else {
                    vec![path]
                }
            })
            .collect()
    }

    #[tokio::test]
    async fn local_write_is_complete_before_return_and_can_be_streamed() {
        let root = tempfile::tempdir().unwrap();
        let store = local(&root);
        for size in [0, 1, 8191, 8192, 8193, 131073] {
            let bytes = Bytes::from((0..size).map(|i| (i % 256) as u8).collect::<Vec<_>>());
            let chunks = bytes
                .chunks(997)
                .map(|chunk| Ok(Bytes::copy_from_slice(chunk)))
                .collect::<Vec<_>>();
            let (key, written) = store.stream(Box::pin(stream::iter(chunks))).await.unwrap();
            assert_eq!(written, size as i64);
            assert_eq!(
                tokio::fs::read(root.path().join(&key)).await.unwrap(),
                bytes
            );
            let content = store.content(key.clone()).await.unwrap();
            assert_eq!(content.size, size as u64);
            let downloaded = content
                .stream
                .map_ok(|chunk| chunk.to_vec())
                .try_concat()
                .await
                .unwrap();
            assert_eq!(downloaded, bytes);
            store.delete_key(key).await.unwrap();
        }
        assert!(files(root.path()).is_empty());
    }

    #[tokio::test]
    async fn failed_and_cancelled_writes_remove_incomplete_files() {
        let root = tempfile::tempdir().unwrap();
        let store = local(&root);
        let chunks = stream::iter(vec![
            Ok(Bytes::from(vec![1; 16384])),
            Err(Error::new(ErrorKind::ConnectionReset, "interrupted")),
        ]);
        assert!(store.stream(Box::pin(chunks)).await.is_err());
        assert!(files(root.path()).is_empty());

        let (ready_tx, ready_rx) = tokio::sync::oneshot::channel();
        let chunks = stream::once(async { Ok(Bytes::from(vec![2; 16384])) }).chain(stream::once(
            async move {
                ready_tx.send(()).unwrap();
                futures::future::pending::<Result<Bytes, Error>>().await
            },
        ));
        let writer = tokio::spawn(async move { store.stream(Box::pin(chunks)).await });
        ready_rx.await.unwrap();
        writer.abort();
        assert!(writer.await.unwrap_err().is_cancelled());
        assert!(files(root.path()).is_empty());
    }

    #[tokio::test]
    async fn local_content_rejects_missing_invalid_and_escaping_keys() {
        let root = tempfile::tempdir().unwrap();
        let store = local(&root);
        assert_eq!(
            store.content("missing".into()).await.err().unwrap().kind(),
            ErrorKind::NotFound
        );
        for key in ["", "/etc/passwd", "../outside", "a/../../outside", "."] {
            assert_eq!(
                store.content(key.into()).await.err().unwrap().kind(),
                ErrorKind::InvalidData
            );
        }
        let outside = tempfile::NamedTempFile::new().unwrap();
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(outside.path(), root.path().join("link")).unwrap();
            assert_eq!(
                store.content("link".into()).await.err().unwrap().kind(),
                ErrorKind::InvalidData
            );
        }
        std::fs::create_dir(root.path().join("directory")).unwrap();
        assert_eq!(
            store
                .content("directory".into())
                .await
                .err()
                .unwrap()
                .kind(),
            ErrorKind::InvalidData
        );
    }

    #[tokio::test]
    async fn portable_and_legacy_urls_and_inline_blobs_remain_compatible() {
        let root = tempfile::tempdir().unwrap();
        let mut store = local(&root);
        let id = Uuid::from_u128(rand::random());
        assert_eq!(
            store.download_url(id, "AA/BB/key".into()).await,
            format!("file://{}/AA/BB/key", store.root)
        );
        store.public_base_url = Some(url::Url::parse("https://cache.example/rsc/").unwrap());
        assert_eq!(
            store.download_url(id, "AA/BB/key".into()).await,
            format!("https://cache.example/rsc/blob/{id}")
        );
        let dbonly = DbOnlyBlobStore {
            id: Uuid::from_u128(rand::random()),
        };
        let bytes = Bytes::from_static(b"\x00\xffinline");
        let (key, size) = dbonly
            .stream(Box::pin(stream::once(async move { Ok(bytes) })))
            .await
            .unwrap();
        assert_eq!(size, 8);
        assert_eq!(
            dbonly.download_url(id, key).await,
            "db://%00%FF%69%6E%6C%69%6E%65"
        );
        assert_eq!(
            dbonly.content("ignored".into()).await.err().unwrap().kind(),
            ErrorKind::Unsupported
        );
        let too_large = Bytes::from(vec![0; 101]);
        assert!(dbonly
            .stream(Box::pin(stream::once(async move { Ok(too_large) })))
            .await
            .is_err());
    }
}
