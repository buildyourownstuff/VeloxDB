#pragma once

#include "veloxdb/commands/command_registry.h"
#include "veloxdb/config/config.h"
#include "veloxdb/metrics/metrics.h"
#include "veloxdb/persistence/aof.h"
#include "veloxdb/persistence/snapshot.h"
#include "veloxdb/storage/storage_engine.h"
#include "veloxdb/util/status.h"

#include <atomic>
#include <memory>
#include <thread>

namespace veloxdb {

class Server {
public:
  Server(Config& config, StorageEngine& storage, Metrics& metrics, const CommandRegistry& registry,
         Aof* aof, SnapshotStore* snapshot);
  ~Server();

  Status run(const std::atomic_bool* external_stop);
  void request_stop();

private:
  void start_expiration_loop();
  void stop_expiration_loop();

  Config& config_;
  StorageEngine& storage_;
  Metrics& metrics_;
  const CommandRegistry& registry_;
  Aof* aof_;
  SnapshotStore* snapshot_;
  std::atomic_bool stopping_{false};
  std::thread expiration_thread_;
};

} // namespace veloxdb
