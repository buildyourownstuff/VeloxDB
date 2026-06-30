#include "veloxdb/server/server.h"

#include "veloxdb/net/tcp_server.h"
#include "veloxdb/util/logger.h"

namespace veloxdb {

Server::Server(Config& config, StorageEngine& storage, Metrics& metrics,
               const CommandRegistry& registry, Aof* aof, SnapshotStore* snapshot)
    : config_(config), storage_(storage), metrics_(metrics), registry_(registry), aof_(aof),
      snapshot_(snapshot) {}

Server::~Server() { request_stop(); }

Status Server::run(const std::atomic_bool* external_stop) {
  stopping_.store(false, std::memory_order_relaxed);
  start_expiration_loop();

  TcpServer tcp(config_, storage_, metrics_, registry_, aof_, snapshot_,
                [this] { request_stop(); });
  Status status = tcp.run(external_stop);
  request_stop();
  stop_expiration_loop();
  return status;
}

void Server::request_stop() { stopping_.store(true, std::memory_order_relaxed); }

void Server::start_expiration_loop() {
  if (!config_.expiration.active_enabled) {
    return;
  }
  expiration_thread_ = std::thread([this] {
    log_info("expiration", "active expiration loop started");
    while (!stopping_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(config_.expiration.interval_ms));
      (void)storage_.active_expire(config_.expiration.sample_per_shard);
    }
    log_info("expiration", "active expiration loop stopped");
  });
}

void Server::stop_expiration_loop() {
  stopping_.store(true, std::memory_order_relaxed);
  if (expiration_thread_.joinable()) {
    expiration_thread_.join();
  }
}

} // namespace veloxdb
