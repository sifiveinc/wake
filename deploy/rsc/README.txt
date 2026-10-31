RSC on Kubernetes with a private cache PVC
========================================

Topology: many build pods -> HTTP Service rsc:3002 -> one RSC pod -> cache PVC.
RSC also connects to a durable PostgreSQL database for metadata, API keys, and
store registrations. Build pods need neither PostgreSQL access nor the cache
PVC. This example does not provision PostgreSQL, ingress, or cloud resources.

Keep replicas=1 and strategy=Recreate. Recreate stops the old workload before
starting the replacement during an upgrade, accepting a short outage instead
of competing for the ReadWriteOnce volume. ReadWriteOnce alone does not enforce
one writer on a node. For manual replacement, scale to zero and wait for all RSC
pods to disappear before scaling up; do not force-delete a pod on a failed node
without first fencing that node. Independent RSC replicas with unrelated local
disks behind one Service are NOT supported: metadata would point to blobs that
only some replicas can read. This topology can serve many concurrent build pods.
Kubernetes rollout/storage background:
  https://kubernetes.io/docs/concepts/workloads/controllers/deployment/#recreate-deployment
  https://kubernetes.io/docs/concepts/storage/persistent-volumes/#access-modes

Files and prerequisites
-----------------------
Run the commands below from the repository root. Local rendering requires
Python >=3.8 with no extra packages; cluster commands require kubectl access
to a namespace you control. The namespace, PostgreSQL, and a suitable storage
class must already exist. Set image pull credentials in your namespace if needed.
Use a storage class that survives pod/node replacement, not ephemeral local
storage, and understand its reclaim policy before allocating the claim.

Dockerfile builds the existing rsc, rsc_tool, and migration binaries with their
checked-in Cargo locks and installs them in /usr/local/bin. It does not build
Wake. Runtime dependencies are OpenSSL, SQLite (migration tooling), and CA
certificates; the container runs as UID/GID 10001 with working directory /work.
Build images for the architecture of your cluster and pin image digests for
repeatable use. RUST_IMAGE and RUNTIME_IMAGE are optional Docker build args;
keep their distributions/ABIs compatible if overriding the Bookworm defaults.

  export RSC_IMAGE=your-registry.example/wake-rsc:your-revision
  docker build -f deploy/rsc/Dockerfile -t "$RSC_IMAGE" .

Make that image available to your cluster by your normal image delivery process.
No image publication or live deployment is performed by the renderer.

Database and credentials
------------------------
Use a dedicated PostgreSQL database and role. The role used for migrations must
own its schema and be able to create/alter its objects. RSC needs ongoing access
to read/write metadata. The migration CLI defaults to the public schema; use the
same schema/search_path for RSC and tooling. PostgreSQL durability and backups
are separate from the blob PVC and are required for cache persistence.

  export RSC_NAMESPACE=your-existing-namespace
  export RSC_SECRET=rsc-credentials

Create a Secret from a protected local file containing only the PostgreSQL URL,
including username/password and any required TLS options. URL-encode credentials
as appropriate. Do not check this file or the Secret into source control.

  kubectl -n "$RSC_NAMESPACE" create secret generic "$RSC_SECRET" \
    --from-file=database-url=/secure/path/database-url

The file must contain the URL without a trailing newline. RSC gets it through
WAKE_RSC_CONFIG_DATABASE_URL; migration uses DATABASE_URL. RSC currently logs
its full effective configuration, including database_url, at startup. Restrict
access to pod logs as well as Secrets. --show-config also prints that URL.

Start an admin pod WITHOUT the cache PVC; it only manages database records:

  python3 deploy/rsc/render.py admin > /tmp/rsc-admin.yaml
  kubectl -n "$RSC_NAMESPACE" apply -f /tmp/rsc-admin.yaml
  kubectl -n "$RSC_NAMESPACE" wait --for=condition=Ready pod/rsc-admin --timeout=180s
  kubectl -n "$RSC_NAMESPACE" exec rsc-admin -- /usr/local/bin/migration status
  kubectl -n "$RSC_NAMESPACE" exec rsc-admin -- /usr/local/bin/migration up

Both rsc and rsc_tool reject pending migrations; neither automatically migrates.
Use the migration binary from the same revision. Back up existing PostgreSQL and
blob storage before applying migrations. HTTP blob transport itself adds no
migrations. Never use fresh, refresh, reset, or down to enable HTTP transport.

For a NEW database only, run the existing interactive bootstrap:

  kubectl -n "$RSC_NAMESPACE" exec -it rsc-admin -- /usr/local/bin/rsc_tool --bootstrap

Answer NO to the development/test key, describe its use, and enter /var/lib/rsc
for the local blob root. Save the generated key securely and record the local
store UUID printed by the tool. Bootstrap creates an API key, local store, and
the required DbOnly store. Its mkdir runs in the admin pod's disposable root;
the registered path describes the server's PVC mount, not shared admin storage.

For an EXISTING database, do not bootstrap again. Inspect existing registrations:

  kubectl -n "$RSC_NAMESPACE" exec rsc-admin -- /usr/local/bin/rsc_tool list local-blob-store

Reuse the original local-store UUID and its EXACT root path and volume contents.
The supplied manifest mounts at /var/lib/rsc; if the old root differs, adapt the
mountPath to that root. Keep every registered root containing old blobs reachable
by RSC, even when active_store selects a different store for new uploads. Merely
changing active_store does not move existing data. Preserve shard directories.
For a deliberately new store in an already bootstrapped database, the real
noninteractive command is rsc_tool add local-blob-store --root /var/lib/rsc;
restart RSC after adding stores because it activates them at startup.

If a separate write key is needed, generate it with:

  kubectl -n "$RSC_NAMESPACE" exec rsc-admin -- /usr/local/bin/rsc_tool add api-key --desc 'build pods'

Save the selected generated API key, without a trailing newline, in a protected
file. Recreate/apply the same Secret with both entries (no literal keys in YAML):

  kubectl -n "$RSC_NAMESPACE" create secret generic "$RSC_SECRET" \
    --from-file=database-url=/secure/path/database-url \
    --from-file=api-key=/secure/path/api-key --dry-run=client -o yaml | \
    kubectl -n "$RSC_NAMESPACE" apply -f -
  kubectl -n "$RSC_NAMESPACE" delete pod rsc-admin

Authorization is the raw API key in the Authorization header, without 'Bearer '.
Writes require it; cache reads do not. Use trusted network access and an existing
TLS proxy when confidentiality is needed. Only writer clients receive api-key;
the RSC pod receives only database-url. Rotate client pods after Secret changes.

Render and start the cache
--------------------------
Set the UUID from bootstrap/list, a cluster storage class, and storage capacity:

  export RSC_ACTIVE_STORE=the-existing-local-store-uuid
  export RSC_STORAGE_CLASS=your-storage-class
  export RSC_STORAGE_CAPACITY=10Gi
  export RSC_PVC=rsc-cache
  export RSC_PUBLIC_BASE_URL=http://rsc:3002
  python3 deploy/rsc/render.py rsc > /tmp/rsc.yaml

RSC_IMAGE, RSC_STORAGE_CLASS, and RSC_ACTIVE_STORE are required. Other values have
the defaults shown above; RSC_SECRET defaults to rsc-credentials. A literal empty
RSC_STORAGE_CLASS disables dynamic provisioning and requires a matching static
PV. For an existing PVC, select its name with RSC_PVC and keep its storage class,
access mode, and requested capacity compatible; do not create an empty replacement
claim and expect old blobs to appear. Retain the original data and backups.

Review /tmp/rsc.yaml, then validate against your cluster before applying:

  kubectl -n "$RSC_NAMESPACE" apply --dry-run=server -f /tmp/rsc.yaml
  kubectl -n "$RSC_NAMESPACE" apply -f /tmp/rsc.yaml
  kubectl -n "$RSC_NAMESPACE" rollout status deployment/rsc --timeout=180s

Only RSC mounts the cache PVC. fsGroup=10001 lets compatible CSI drivers grant
write access; pre-provisioned files must be readable/writable by this user/group.
For an existing installation, preserve its required ownership instead of blindly
changing it. The PVC is independent of the Deployment; replacing the pod retains
it. Readiness checks HTTP discovery, not all PostgreSQL/storage dependencies.

RSC loads WAKE_RSC_CONFIG_* environment values FIRST, then /work/.config.json
overrides matching keys. The .config file is required by rsc; it is optional for
rsc_tool, whose --database-url additionally overrides both sources. This example
omits database_url from its ConfigMap so the Secret takes effect. Do not add a
placeholder database_url to the file. File values do not interpolate environment
variables. The ConfigMap uses a subPath mount; restart after changing it or the
Secret. Review the sample TTL/load-shedding settings for your retention/workload;
retaining a PVC does not disable normal cache eviction or impose a size quota.

server_address=0.0.0.0:3002 is the socket to LISTEN on. public_base_url is the
HTTP(S) address every build pod must REACH, used for advertised upload and blob
download URLs. http://rsc:3002 works within the same namespace; use a qualified
Service DNS name across namespaces. Do not advertise 0.0.0.0 or localhost.
public_base_url is optional in the server: omitted/null preserves legacy file://
downloads and requires clients to see the old shared filesystem. This example
always sets it for portable HTTP downloads. Inline db:// blobs remain supported.
A public URL may include a path prefix, for example https://cache.example/rsc/;
the existing proxy must strip /rsc before forwarding and may terminate TLS.
Credentials, queries, and fragments are not allowed in public_base_url.

Real Wake build pods connect to the Service without any cache-volume mount.
For same-namespace access set the existing client configuration:

  WAKE_REMOTE_CACHE="pushpull:rsc:3002:YOUR_API_KEY"
  WAKE_REMOTE_CACHE="pull:rsc:3002:"

Inject the write key from a Secret rather than hard-coding it in a pod spec.
These are Wake's policy:domain:port:auth settings, not URLs. The sample uses the
direct HTTP Service for job API calls; advertised blob URLs can use HTTPS through
a proxy. Do not assume an https:// URL with a path prefix can be substituted into
the client's domain field. Client images must supply Wake and their build inputs.

Two-client-pod and persistence smoke check
-----------------------------------------
This is an HTTP API smoke test, NOT a Wake build or Wake artifact-fidelity test.
It uses Python >=3.8 locally and Python in both client pods. Set an alternative
RSC_CLIENT_IMAGE with python3 if needed (default python:3.12-slim). Neither client
has any volume mount; the reader has no API key. Run only against the example
Deployment in a namespace where an intentional brief RSC outage is acceptable:

  export RSC_URL=http://rsc:3002
  bash deploy/rsc/smoke.sh

The first pod uploads a >256KiB binary blob to the filesystem store and publishes
a uniquely keyed job. The second pod matches that job and compares exact HTTP
download bytes. The driver scales RSC to zero, waits for deletion, then scales
to one with the same PostgreSQL and PVC and repeats the second pod's lookup and
download. It uses no BLAKE3 library/content_hash upload field, which the API
permits, and does not prove Wake output restoration, hash checks, permissions,
symlinks, stdout/stderr restoration, or proxy/TLS behavior. Those require the
separate integration regression. Normal TTL eviction eventually removes smoke
entries. The driver deletes only its two client pods on exit; it leaves RSC and
the cache data in place. If interrupted while scaled down, restore service with:

  kubectl -n "$RSC_NAMESPACE" scale deployment/rsc --replicas=1

Rollback and data preservation
------------------------------
Before upgrades save the old rendered manifest/image and a consistent PostgreSQL
backup plus blob-volume backup/snapshot while RSC is stopped. Retain PostgreSQL,
local-store registrations/UUIDs, root paths, PVC, and old blob contents. Never
delete the PVC, namespace, database, or all resources from /tmp/rsc.yaml as an
upgrade/rollback step; a storage class with Delete reclaim policy may delete the
underlying disk if its claim is deleted. Do not remove store records or jobs.

To restore the previous image/config, scale to zero, wait for RSC pods to vanish,
reapply the saved ConfigMap/Deployment (and compatible retained PVC definition),
then wait for readiness. A Deployment rollout undo alone does not restore a
changed ConfigMap/Secret. HTTP transport needs no schema downgrade or relocation.
If returning to a binary/config that emits file:// URLs, also restore clients'
access to the original shared filesystem; portable pods cannot use those URLs.
If unrelated schema migrations were applied, use a compatible binary or restore
the coordinated database/blob backup; do not blindly run migration down.

Validation limits
------------------
The example can be rendered, YAML/schema checked, and reviewed without a cluster.
Cluster admission, image builds, actual CSI permissions/attachment, the two-pod
smoke test, and restart persistence must be verified in the target environment.
No live cluster checks or container image build were run when adding this example.
