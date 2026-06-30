#include "veloxdb/commands/command_registry.h"
#include "veloxdb/config/config.h"
#include "veloxdb/metrics/metrics.h"
#include "veloxdb/persistence/aof.h"
#include "veloxdb/persistence/snapshot.h"
#include "veloxdb/server/server.h"
#include "veloxdb/storage/storage_engine.h"
#include "veloxdb/util/logger.h"

#include <atomic>
#include <csignal>
#include <iostream>

namespace {

std::atomic_bool g_stop_requested{false};

void signal_handler(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

} // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGPIPE, SIG_IGN);

  veloxdb::ConfigLoadResult loaded = veloxdb::ConfigLoader::load(argc, argv);
  if (loaded.show_help) {
    std::cout << loaded.help_text;
    return 0;
  }
  if (!loaded.status) {
    std::cerr << "config error: " << loaded.status.to_string() << '\n';
    return 2;
  }

  veloxdb::Config config = std::move(loaded.config);
  veloxdb::Logger::instance().set_level(veloxdb::parse_log_level(config.logging.level));
  veloxdb::log_info("server", "VeloxDB starting");

  veloxdb::StorageEngine storage(config.storage, config.protocol);
  veloxdb::Metrics metrics;
  metrics.set_enabled(config.metrics.enabled);
  veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol);
  veloxdb::CommandRegistry registry;
  veloxdb::register_default_commands(registry);

  if (!config.persistence.aof_enabled) {
    const veloxdb::Status snapshot_status = snapshot.load(storage);
    if (!snapshot_status) {
      veloxdb::log_warn("snapshot", snapshot_status.to_string());
    }
  }

  if (config.persistence.aof_enabled) {
    veloxdb::CommandContext replay_ctx{
        storage,
        metrics,
        config,
        nullptr,
        &snapshot,
        {},
        true,
    };
    const auto stats = veloxdb::Aof::replay_file(
        config.persistence.aof_path, config.protocol,
        [&](const std::vector<std::string>& args) -> veloxdb::Status {
          std::vector<std::string_view> views;
          views.reserve(args.size());
          for (const auto& arg : args) {
            views.emplace_back(arg);
          }
          const veloxdb::CommandResult result = registry.execute(replay_ctx, views);
          if (result.response.rfind("-ERR", 0) == 0) {
            return veloxdb::Status::protocol_error("AOF command failed during replay");
          }
          return veloxdb::Status::ok();
        });
    if (!stats.status) {
      veloxdb::log_error("aof", stats.status.to_string());
      return 3;
    }
    veloxdb::log_info("aof", "replayed " + std::to_string(stats.commands) + " commands");
    if (stats.ignored_partial_tail) {
      veloxdb::log_warn("aof", "ignored partial final command during recovery");
    }
  }

  veloxdb::Aof aof(config.persistence, config.protocol);
  const veloxdb::Status aof_open = aof.open();
  if (!aof_open) {
    veloxdb::log_error("aof", aof_open.to_string());
    return 4;
  }

  veloxdb::Server server(config, storage, metrics, registry, &aof, &snapshot);
  const veloxdb::Status status = server.run(&g_stop_requested);
  aof.close();

  if (!status) {
    veloxdb::log_error("server", status.to_string());
    return 1;
  }
  veloxdb::log_info("server", "VeloxDB stopped cleanly");
  return 0;
}
