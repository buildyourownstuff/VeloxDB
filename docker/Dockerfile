FROM ubuntu:24.04

RUN apt-get update \
  && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DVELOXDB_BUILD_TESTS=OFF -DVELOXDB_BUILD_BENCHMARKS=OFF \
  && cmake --build build -j

EXPOSE 6379
VOLUME ["/data"]

CMD ["./build/veloxdb", "--host", "0.0.0.0", "--port", "6379", "--aof-path", "/data/veloxdb.aof"]
