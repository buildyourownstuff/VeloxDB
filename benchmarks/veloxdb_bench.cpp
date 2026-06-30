#include "veloxdb/commands/command_registry.h"
#include "veloxdb/protocol/resp.h"
#include "veloxdb/storage/storage_engine.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename Fn>
void run_bench(std::string_view name, size_t iterations, Fn fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
      std::chrono::steady_clock::now() - start);
  const double ops = static_cast<double>(iterations) / elapsed.count();
  std::cout << name << ": " << iterations << " ops in " << elapsed.count() << "s, " << ops
            << " ops/s\n";
}

} // namespace

int main() {
  constexpr size_t iterations = 200000;
  veloxdb::Config config;
  config.storage.max_memory_bytes = 0;

  run_bench("resp_parse_set", iterations, [&] {
    const std::string cmd = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    for (size_t i = 0; i < iterations; ++i) {
      veloxdb::resp::Parser parser(veloxdb::resp::parser_options_from_config(config.protocol));
      (void)parser.append(cmd);
      (void)parser.next_command();
    }
  });

  run_bench("storage_set", iterations, [&] {
    veloxdb::StorageEngine storage(config.storage, config.protocol);
    for (size_t i = 0; i < iterations; ++i) {
      (void)storage.set("key:" + std::to_string(i), "value", std::nullopt);
    }
  });

  run_bench("storage_get", iterations, [&] {
    veloxdb::StorageEngine storage(config.storage, config.protocol);
    for (size_t i = 0; i < iterations; ++i) {
      (void)storage.set("key:" + std::to_string(i), "value", std::nullopt);
    }
    for (size_t i = 0; i < iterations; ++i) {
      (void)storage.get("key:" + std::to_string(i));
    }
  });

  run_bench("command_set_get_mixed", iterations, [&] {
    veloxdb::StorageEngine storage(config.storage, config.protocol);
    veloxdb::Metrics metrics;
    veloxdb::CommandRegistry registry;
    veloxdb::register_default_commands(registry);
    veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol);
    veloxdb::CommandContext ctx{storage, metrics, config, nullptr, &snapshot, {}, false};
    for (size_t i = 0; i < iterations; ++i) {
      const std::string key = "key:" + std::to_string(i);
      std::vector<std::string_view> set_args{"SET", key, "value"};
      (void)registry.execute(ctx, set_args);
      std::vector<std::string_view> get_args{"GET", key};
      (void)registry.execute(ctx, get_args);
    }
  });
}
