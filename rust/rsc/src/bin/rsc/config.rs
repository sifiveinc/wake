use config::{Config, ConfigError, Environment, File};
use serde::{Deserialize, Serialize};

#[derive(Debug, Deserialize, Serialize)]
pub struct RSCCronLoopConfig {
    // How often to run the loop in seconds
    pub tick_rate: u64,
    // Maximum number of objects to procss per tick. Must be 1 >= x <= 16000
    pub chunk_size: i32,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct RSCTTLConfig {
    // How often to run the eviction check in seconds
    pub tick_rate: u64,
    // How long an object is allowed to live
    pub ttl: u64,
    // Maximum number of objects to delete from the db at a time. Must be 1 >= x <= 16000
    pub chunk_size: u32,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct RSCBlobTTLConfig {
    // How often to run the eviction check in seconds
    pub tick_rate: u64,
    // How long an object is allowed to live
    pub ttl: u64,
    // Maximum number of blobs to delete from the db at a time. Must be 1 >= x <= 64000
    pub chunk_size: u32,
    // Maximum number of files to delete from the disk per task
    pub file_chunk_size: usize,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct RSCLRUConfig {
    // The max size in bytes that the cache may reach before eviction
    pub high_mark: u64,
    // The end size in bytes that the cache should reach after eviction
    pub low_mark: u64,
    // Maximum number of objects to delete at a time. Must be 1 >= x <= 16000
    pub chunk_size: u32,
}

#[derive(Debug, Deserialize, Serialize)]
pub enum RSCJobEvictionConfig {
    // Time to live eviction strategy
    #[serde(rename = "ttl")]
    TTL(RSCTTLConfig),
    // Least recently used eviction strategy
    #[serde(rename = "lru")]
    LRU(RSCLRUConfig),
}

#[derive(Debug, Deserialize, Serialize)]
pub struct RSCLoadShedConfig {
    // How often to refresh the system load
    pub tick_rate: u64,
    // Load value after which load should be statistically shed
    pub target: f64,
    // The minimum amount of time a job must take to complete in order to be cached
    pub min_runtime: f64,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct RSCConfig {
    // The url used to connect to the postgres database
    pub database_url: String,
    // The address the that server should bind to
    pub server_address: String,
    // Optional externally reachable HTTP(S) base URL. Without it, local blobs retain file:// URLs.
    // A reverse proxy must strip any base path before forwarding requests to server_address.
    #[serde(default)]
    pub public_base_url: Option<String>,
    // The max number of connnections to open in the connection pool. Value must consider the
    // postgres server max
    pub connection_pool_max_connect: u32,
    // The amount of time a query should wait for a connection before timing out in seconds
    pub connection_pool_timeout: u64,
    // The blob store that new blobs should be written into
    pub active_store: String,
    // The directory that server logs should be written to. If None logs are written to stdout
    pub log_directory: Option<String>,
    // The config to control blob eviction
    pub blob_eviction: RSCBlobTTLConfig,
    // The config to control job eviction
    pub job_eviction: RSCJobEvictionConfig,
    // The config to control job size calculation
    pub job_size_calculate: RSCCronLoopConfig,
    // The config to control load shed
    pub load_shed: RSCLoadShedConfig,
}

impl RSCConfig {
    pub fn new() -> Result<RSCConfig, ConfigError> {
        // Gather the config
        let config = Config::builder()
            .add_source(Environment::with_prefix("WAKE_RSC_CONFIG"))
            .add_source(File::with_name(".config"))
            .build()?;

        let config: RSCConfig = config.try_deserialize()?;

        config.validate()?;

        Ok(config)
    }

    fn validate(&self) -> Result<(), ConfigError> {
        self.server_address
            .parse::<std::net::SocketAddr>()
            .map_err(|_| {
                ConfigError::Message(
                    "server_address must be a listen socket such as 0.0.0.0:3002".into(),
                )
            })?;
        self.public_url()?;

        if self.load_shed.target == 0.0 {
            return Err(ConfigError::Message(
                "Load shed target must not be zero".to_string(),
            ));
        }

        Ok(())
    }

    pub fn public_url(&self) -> Result<Option<url::Url>, ConfigError> {
        self.public_base_url.as_deref().map(|base| {
            let invalid = || ConfigError::Message(
                "public_base_url must be an absolute HTTP(S) URL without credentials, query, or fragment".into()
            );
            let mut url = url::Url::parse(base).map_err(|_| invalid())?;
            if !matches!(url.scheme(), "http" | "https")
                || url.host_str().is_none()
                || !url.username().is_empty()
                || url.password().is_some()
                || url.query().is_some()
                || url.fragment().is_some()
            {
                return Err(invalid());
            }
            // Treat both /cache and /cache/ as a directory prefix when joining endpoints.
            if !url.path().ends_with('/') {
                url.set_path(&format!("{}/", url.path()));
            }
            Ok(url)
        }).transpose()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn legacy_config() -> RSCConfig {
        serde_json::from_str(include_str!("../../../.config.json")).unwrap()
    }

    #[test]
    fn legacy_config_remains_valid() {
        let config = legacy_config();
        config.validate().unwrap();
        assert!(config.public_url().unwrap().is_none());
    }

    #[test]
    fn public_url_preserves_prefix_and_normalizes_slash() {
        for base in ["https://cache.example/rsc", "https://cache.example/rsc/"] {
            let mut config = legacy_config();
            config.public_base_url = Some(base.into());
            assert_eq!(
                config
                    .public_url()
                    .unwrap()
                    .unwrap()
                    .join("blob")
                    .unwrap()
                    .as_str(),
                "https://cache.example/rsc/blob"
            );
        }
    }

    #[test]
    fn public_url_and_listen_socket_are_validated_separately() {
        let mut config = legacy_config();
        for invalid in [
            "0.0.0.0:3002",
            "/cache",
            "file:///tmp/cache",
            "ftp://cache.example",
            "https://user:secret@cache.example",
            "https://cache.example?x=1",
            "https://cache.example/#fragment",
            "",
        ] {
            config.public_base_url = Some(invalid.into());
            assert!(config.validate().is_err(), "accepted {invalid}");
        }
        config.public_base_url = Some("http://cache.example:3002".into());
        config.validate().unwrap();
        config.server_address = "http://cache.example:3002".into();
        assert!(config.validate().is_err());
    }
}
