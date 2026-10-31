Portable RSC regression
=======================

The default PostgreSQL-backed Rust suite contains
portable_network_job_round_trip_and_concurrent_transfers. It uses a real TCP
listener, uploads binary blobs concurrently, posts/matches a complete job, follows
its returned HTTP URL, and compares bytes and BLAKE3 hashes. It covers 257-byte
non-inline writes (below the storage writer buffer), larger streamed writes,
empty/small inline blobs, stdout/stderr, file modes, directory/symlink metadata,
nonzero job status, cache-key misses, unknown UUIDs, and server-side deletion.
The client code never opens the server directory; only fault injection does.

The additional portable_isolated_wake_clients test is deliberately opt-in. It
requires Linux user/mount namespaces, mount/unshare from util-linux, Python 3,
curl, and a built Wake with its share/wake libraries and lib/wake helpers in the
usual repository/install layout. It fails explicitly if isolation is unavailable;
it never silently runs without isolation.

Build Wake's bootstrap executable and helpers from the repository root:

  make -j4 bin/wake bin/wakebox lib/wake/fuse-waked lib/wake/shim-wake lib/wake/wake-hash bin/wake-migrate

Prepare the same disposable local PostgreSQL service as the existing RSC tests:
localhost:5432, a shim database, and a role matching the invoking Unix user with
CREATEDB permission. Supply PGPASSWORD if password authentication is configured.
Every test creates its own migrated database. Use only a disposable PostgreSQL
instance; the existing test harness retains those databases.

Run the standard suite:

  cargo test --manifest-path rust/rsc/Cargo.toml

Run the isolated real-Wake smoke from the repository root:

  WAKE_ISOLATION_BINARY="$PWD/bin/wake" cargo test --manifest-path rust/rsc/Cargo.toml --bin rsc portable_isolated_wake_clients -- --ignored --nocapture

On a disk-constrained machine, reuse a target on an executable temporary memory
filesystem, with capacity sufficient for Rust dependencies and linking:

  export CARGO_TARGET_DIR=/dev/shm/instead-wake-rsc-target
  export CARGO_PROFILE_DEV_DEBUG=0 CARGO_PROFILE_TEST_DEBUG=0 CARGO_INCREMENTAL=0

Isolation boundary
------------------

The Rust test owns the PostgreSQL connection and actual network server outside
the client's namespace. A child enters a new user and mount namespace, makes
mount propagation private, and mounts an empty tmpfs over the exact server blob
root. The client checks that this root is a mount point and empty, and that
opening the known server-only sentinel fails. The server verifies its original
sentinel is still present after the client exits. Loopback networking remains
shared intentionally so clients reach the server over HTTP.

Inside that namespace, separate fresh Wake workspaces start without a wake.db,
.build directory, or CAS. The producer uses mkRemoteCacheRunner with
stagingDirRunner to execute and upload a job. A new pull-only Wake process
restores it. An HTTP execution counter proves the second client did not execute
the base job; successful bytes alone would not establish a cache hit.

The smoke checks binary bytes, independently recomputed BLAKE3 hashes, empty
files, permissions, directories, symlinks, stdout/stderr, and successful status.
Three further fresh clients restore concurrently. Policy cases prove push-only
does not search, and a pull-only miss does not upload. An explicit wrong-hash
check rejects a blob before materialization. A local observing proxy injects
corrupt bytes and 404 responses, and verifies safe fallback to actual local job
execution. Actual server file deletion is covered separately by the normal
Rust network test. All requests use test-only InsecureKey authorization.

This proves actual Wake runner/API/CAS behavior with an inaccessible server
storage mount. stagingDirRunner is used for the fixture job; Wake's HTTP jobs
use their existing runner. This is not a Kubernetes test, a network namespace
isolation test, a hostile-client security sandbox, or a PVC restart/TLS/ingress
test. See deploy/rsc for the separately supplied cluster smoke example.
