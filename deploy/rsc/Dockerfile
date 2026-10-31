# Build from the repository root: docker build -f deploy/rsc/Dockerfile -t IMAGE .
ARG RUST_IMAGE=rust:1-bookworm
ARG RUNTIME_IMAGE=debian:bookworm-slim
FROM ${RUST_IMAGE} AS build
RUN apt-get update && apt-get install -y --no-install-recommends pkg-config libssl-dev libsqlite3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY rust/rsc rust/rsc
COPY rust/entity rust/entity
COPY rust/migration rust/migration
ENV CARGO_TARGET_DIR=/build/target
RUN cargo build --locked --release --manifest-path rust/rsc/Cargo.toml --bins
RUN cargo build --locked --release --manifest-path rust/migration/Cargo.toml --bin migration

FROM ${RUNTIME_IMAGE}
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libssl3 libsqlite3-0 && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/target/release/rsc /build/target/release/rsc_tool /build/target/release/migration /usr/local/bin/
RUN mkdir -p /work /var/lib/rsc && chown 10001:10001 /work /var/lib/rsc
WORKDIR /work
USER 10001:10001
EXPOSE 3002
CMD ["/usr/local/bin/rsc"]
