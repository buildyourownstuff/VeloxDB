FROM ubuntu:24.04 AS build

RUN apt-get update \
  && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DVELOXDB_BUILD_TESTS=OFF -DVELOXDB_BUILD_BENCHMARKS=OFF \
  && cmake --build build -j

FROM ubuntu:24.04 AS runtime

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

COPY --from=build /src/build/veloxdb /usr/local/bin/veloxdb
COPY veloxdb.toml /etc/veloxdb/veloxdb.toml

EXPOSE 7379
VOLUME ["/data"]

USER veloxdb
WORKDIR /var/lib/veloxdb

CMD ["veloxdb", "--host", "0.0.0.0", "--port", "7379", "--aof-path", "/data/veloxdb.aof", "--snapshot-path", "/data/veloxdb.snapshot"]
