use lazy_static::lazy_static;
use prometheus::{
    register_counter, register_histogram, register_histogram_vec, register_int_counter_vec,
    Counter, Histogram, HistogramVec, IntCounterVec,
};

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

    /// Counts completed jobs pushed into the remote cache.
    pub static ref CACHE_PUSHES_TOTAL: IntCounterVec = register_int_counter_vec!(
        "cache_pushes_total",
        "Number of jobs successfully pushed into the remote cache",
        &["route"]
    ).unwrap();

    /// Counts cache lookup requests, including both hits and misses.
    pub static ref CACHE_PULLS_TOTAL: IntCounterVec = register_int_counter_vec!(
        "cache_pulls_total",
        "Number of remote cache lookup requests",
        &["route"]
    ).unwrap();

    /// Counts HTTP calls by method, normalized route, and response status.
    pub static ref HTTP_REQUESTS_TOTAL: prometheus::IntCounterVec = register_int_counter_vec!(
        "http_requests_total",
        "Number of HTTP requests handled by the remote cache",
        &["method", "route", "status"]
    ).unwrap();

    /// Tracks HTTP request latency by method and normalized route.
    pub static ref HTTP_REQUEST_DURATION_SECONDS: HistogramVec = register_histogram_vec!(
        "http_request_duration_seconds",
        "HTTP request latency in seconds",
        &["method", "route"]
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

    /// Tracks total runtime saved by cache hits (in seconds).
    pub static ref CACHE_RUNTIME_SAVINGS_SECONDS: Counter = register_counter!(
        "cache_runtime_savings_seconds_total",
        "Total runtime saved by cache hits in seconds"
    ).unwrap();

    /// Tracks total CPU time saved by cache hits (in seconds).
    pub static ref CACHE_CPUTIME_SAVINGS_SECONDS: Counter = register_counter!(
        "cache_cputime_savings_seconds_total",
        "Total CPU time saved by cache hits in seconds"
    ).unwrap();

    /// Tracks memory usage of cached jobs (in bytes).
    pub static ref CACHE_MEMORY_SAVINGS_BYTES: Counter = register_counter!(
        "cache_memory_savings_bytes_total",
        "Total memory usage saved by cache hits in bytes"
    ).unwrap();

    /// Tracks I/O bytes read saved by cache hits.
    pub static ref CACHE_IO_READ_SAVINGS_BYTES: Counter = register_counter!(
        "cache_io_read_savings_bytes_total",
        "Total I/O read bytes saved by cache hits"
    ).unwrap();

    /// Tracks I/O bytes written saved by cache hits.
    pub static ref CACHE_IO_WRITE_SAVINGS_BYTES: Counter = register_counter!(
        "cache_io_write_savings_bytes_total",
        "Total I/O write bytes saved by cache hits"
    ).unwrap();

    /// Histogram of runtime values for cache hits (in seconds).
    pub static ref CACHE_HIT_RUNTIME_HISTOGRAM: Histogram = register_histogram!(
        "cache_hit_runtime_seconds",
        "Histogram of runtime values for cache hits in seconds"
    ).unwrap();

    /// Histogram of CPU time values for cache hits (in seconds).
    pub static ref CACHE_HIT_CPUTIME_HISTOGRAM: Histogram = register_histogram!(
        "cache_hit_cputime_seconds",
        "Histogram of CPU time values for cache hits in seconds"
    ).unwrap();
}
