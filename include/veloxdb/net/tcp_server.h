#pragma once

#include "veloxdb/commands/command_registry.h"
#include "veloxdb/config/config.h"
#include "veloxdb/metrics/metrics.h"
#include "veloxdb/persistence/aof.h"
#include "veloxdb/persistence/snapshot.h"
#include "veloxdb/storage/storage_engine.h"
#include "veloxdb/util/status.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace veloxdb {

class TcpServer {
public:
  TcpServer(Config& config, StorageEngine& storage, Metrics& metrics,
            const CommandRegistry& registry, Aof* aof, SnapshotStore* snapshot,
            std::function<void()> request_shutdown);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  Status run(const std::atomic_bool* external_stop);
  void request_stop();

private:
  class Worker;

  Status setup_listener();
  void close_listener();
  void accept_ready(size_t& next_worker);
  [[nodiscard]] bool should_stop(const std::atomic_bool* external_stop) const;
  Status accept_loop(const std::atomic_bool* external_stop, size_t& next_worker);

  Config& config_;
  StorageEngine& storage_;
  Metrics& metrics_;
  const CommandRegistry& registry_;
  Aof* aof_;
  SnapshotStore* snapshot_;
  std::function<void()> request_shutdown_;
  std::atomic_bool stopping_{false};
  int listen_fd_{-1};
  std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace veloxdb
