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
#include <filesystem>
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
  veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol,
                                  config.persistence.manifest_path);
  veloxdb::CommandRegistry registry;
  veloxdb::register_default_commands(registry);

  if (!config.persistence.aof_enabled) {
    const veloxdb::Status snapshot_status = snapshot.load(storage);
    if (!snapshot_status) {
      veloxdb::log_warn("snapshot", snapshot_status.to_string());
    }
  }

  if (config.persistence.aof_enabled) {
    size_t aof_replay_offset = 0;
    size_t aof_size = 0;
    std::error_code size_ec;
    if (std::filesystem::exists(config.persistence.aof_path, size_ec) && !size_ec) {
      const auto raw_size = std::filesystem::file_size(config.persistence.aof_path, size_ec);
      if (!size_ec) {
        aof_size = static_cast<size_t>(raw_size);
      }
    }
    const auto snapshot_load =
        snapshot.load_for_recovery(storage, config.persistence.aof_path, aof_size);
    if (!snapshot_load.status) {
      veloxdb::log_error("snapshot", snapshot_load.status.to_string());
      return 3;
    } else if (snapshot_load.loaded_snapshot) {
      aof_replay_offset = snapshot_load.aof_replay_offset;
      veloxdb::log_info("snapshot",
                        "loaded snapshot; replaying AOF from offset " +
                            std::to_string(aof_replay_offset));
    } else if (snapshot_load.found_manifest) {
      veloxdb::log_warn("snapshot", "ignored stale snapshot manifest; replaying AOF from start");
    }

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
        },
        aof_replay_offset);
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
