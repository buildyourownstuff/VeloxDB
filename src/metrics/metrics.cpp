#include "veloxdb/metrics/metrics.h"

namespace veloxdb {

Metrics::Metrics() : start_(std::chrono::steady_clock::now()) {}

void Metrics::set_enabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }

bool Metrics::enabled() const { return enabled_.load(std::memory_order_relaxed); }

void Metrics::connection_opened() {
  connected_clients_.fetch_add(1, std::memory_order_relaxed);
  total_connections_received_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connection_closed() {
  uint64_t current = connected_clients_.load(std::memory_order_relaxed);
  while (current != 0 &&
         !connected_clients_.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
  }
}

void Metrics::command_processed() {
  if (enabled()) {
    total_commands_processed_.fetch_add(1, std::memory_order_relaxed);
  }
}

void Metrics::aof_write_failed() { aof_last_write_ok_.store(false, std::memory_order_relaxed); }

void Metrics::aof_write_succeeded() { aof_last_write_ok_.store(true, std::memory_order_relaxed); }

uint64_t Metrics::uptime_seconds() const {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_)
          .count());
}

uint64_t Metrics::connected_clients() const {
  return connected_clients_.load(std::memory_order_relaxed);
}

uint64_t Metrics::total_connections_received() const {
  return total_connections_received_.load(std::memory_order_relaxed);
}

uint64_t Metrics::total_commands_processed() const {
  return total_commands_processed_.load(std::memory_order_relaxed);
}

uint64_t Metrics::evicted_keys() const { return evicted_keys_.load(std::memory_order_relaxed); }

bool Metrics::aof_last_write_ok() const {
  return aof_last_write_ok_.load(std::memory_order_relaxed);
}

} // namespace veloxdb
