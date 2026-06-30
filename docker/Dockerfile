FROM ubuntu:24.04 AS veloxdb-build

RUN apt-get update \
  && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DVELOXDB_BUILD_TESTS=OFF -DVELOXDB_BUILD_BENCHMARKS=OFF \
  && cmake --build build -j

FROM ubuntu:24.04 AS velox-cli-build

RUN apt-get update \
  && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src/velox-cli
COPY --from=velox_cli . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVELOXCLI_BUILD_TESTS=OFF \
  && cmake --build build -j

FROM ubuntu:24.04 AS runtime-base

ARG VERSION="0.1.0"
ARG REVISION="unknown"
ARG SOURCE="https://github.com/buildyourownstuff/VeloxDB"

LABEL org.opencontainers.image.title="VeloxDB" \
      org.opencontainers.image.description="Redis-inspired in-memory key-value database" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}" \
      org.opencontainers.image.source="${SOURCE}" \
      org.opencontainers.image.licenses="MIT"

RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
  && rm -rf /var/lib/apt/lists/* \
  && useradd --system --create-home --home-dir /var/lib/veloxdb --shell /usr/sbin/nologin veloxdb \
  && mkdir -p /data /etc/veloxdb \
  && chown -R veloxdb:veloxdb /data /var/lib/veloxdb

COPY --from=veloxdb-build /src/build/veloxdb /usr/local/bin/veloxdb
COPY veloxdb.toml /etc/veloxdb/veloxdb.toml

EXPOSE 7379
VOLUME ["/data"]

USER veloxdb
WORKDIR /var/lib/veloxdb

CMD ["veloxdb", "--host", "0.0.0.0", "--port", "7379", "--aof-path", "/data/veloxdb.aof", "--snapshot-path", "/data/veloxdb.snapshot"]

FROM runtime-base AS runtime-with-cli

ARG VELOX_CLI_REF="main"
ARG VELOX_CLI_REVISION="unknown"

LABEL org.opencontainers.image.veloxdb.cli.source="https://github.com/buildyourownstuff/velox-cli" \
      org.opencontainers.image.veloxdb.cli.ref="${VELOX_CLI_REF}" \
      org.opencontainers.image.veloxdb.cli.revision="${VELOX_CLI_REVISION}"

USER root
COPY --from=velox-cli-build /src/velox-cli/build/velox-cli /usr/local/bin/velox-cli
RUN chmod 0755 /usr/local/bin/velox-cli
USER veloxdb

FROM runtime-base AS runtime
