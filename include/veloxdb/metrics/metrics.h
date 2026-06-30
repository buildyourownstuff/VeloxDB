#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace veloxdb {

class Metrics {
public:
  Metrics();

  void set_enabled(bool enabled);
  [[nodiscard]] bool enabled() const;

  void connection_opened();
  void connection_closed();
  void command_processed();
  void aof_write_failed();
  void aof_write_succeeded();

  [[nodiscard]] uint64_t uptime_seconds() const;
  [[nodiscard]] uint64_t connected_clients() const;
  [[nodiscard]] uint64_t total_connections_received() const;
  [[nodiscard]] uint64_t total_commands_processed() const;
  [[nodiscard]] uint64_t evicted_keys() const;
  [[nodiscard]] bool aof_last_write_ok() const;

private:
  std::chrono::steady_clock::time_point start_;
  std::atomic<bool> enabled_{true};
  std::atomic<uint64_t> connected_clients_{0};
  std::atomic<uint64_t> total_connections_received_{0};
  std::atomic<uint64_t> total_commands_processed_{0};
  std::atomic<uint64_t> evicted_keys_{0};
  std::atomic<bool> aof_last_write_ok_{true};
};

} // namespace veloxdb
