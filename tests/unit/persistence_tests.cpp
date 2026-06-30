#include "../test_harness.h"

#include "veloxdb/commands/command_registry.h"
#include "veloxdb/persistence/aof.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

std::string temp_path(std::string_view name) {
  return (std::filesystem::temp_directory_path() /
          ("veloxdb_" + std::string(name) + "_" + std::to_string(::getpid()) + ".aof"))
      .string();
}

veloxdb::Status replay_into(const std::string& path, veloxdb::StorageEngine& storage,
                            veloxdb::Config& config) {
  veloxdb::Metrics metrics;
  veloxdb::CommandRegistry registry;
  veloxdb::register_default_commands(registry);
  veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol);
  veloxdb::CommandContext ctx{storage, metrics, config, nullptr, &snapshot, {}, true};
  const auto stats = veloxdb::Aof::replay_file(
      path, config.protocol, [&](const std::vector<std::string>& args) -> veloxdb::Status {
        std::vector<std::string_view> views;
        for (const auto& arg : args) {
          views.emplace_back(arg);
        }
        const auto result = registry.execute(ctx, views);
        if (result.response.rfind("-ERR", 0) == 0) {
          return veloxdb::Status::protocol_error("replay command failed");
        }
        return veloxdb::Status::ok();
      });
  return stats.status;
}

} // namespace

void register_persistence_tests(TestSuite& suite) {
  suite.add("AOF append and replay", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("append_replay");
    config.persistence.fsync_policy = "always";
    {
      veloxdb::Aof aof(config.persistence, config.protocol);
      require(aof.open(), "open aof");
      require(aof.append(std::vector<std::string>{"SET", "persistent", "value"}), "append set");
      aof.close();
    }

    veloxdb::StorageEngine storage(config.storage, config.protocol);
    require(replay_into(config.persistence.aof_path, storage, config), "replay failed");
    require_eq(*storage.get("persistent"), std::string("value"), "replayed value");
    std::filesystem::remove(config.persistence.aof_path);
  });

  suite.add("AOF ignores partial final command", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("partial");
    std::ofstream out(config.persistence.aof_path, std::ios::binary | std::ios::trunc);
    out << veloxdb::resp::encode_command(std::vector<std::string>{"SET", "a", "1"});
    out << "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$";
    out.close();

    bool ignored = false;
    size_t commands = 0;
    const auto stats = veloxdb::Aof::replay_file(
        config.persistence.aof_path, config.protocol,
        [&](const std::vector<std::string>&) -> veloxdb::Status {
          ++commands;
          return veloxdb::Status::ok();
        });
    ignored = stats.ignored_partial_tail;
    require(stats.status, "partial replay status");
    require(ignored, "partial tail should be ignored");
    require_eq(commands, size_t{1}, "only complete command replayed");
    std::filesystem::remove(config.persistence.aof_path);
  });

  suite.add("AOF expired key recovery", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("expired");
    const int64_t past = veloxdb::unix_ms(veloxdb::StorageEngine::Clock::now()) - 1000;
    std::ofstream out(config.persistence.aof_path, std::ios::binary | std::ios::trunc);
    out << veloxdb::resp::encode_command(std::vector<std::string>{"SET", "gone", "value", "PXAT",
                                                                  std::to_string(past)});
    out.close();

    veloxdb::StorageEngine storage(config.storage, config.protocol);
    require(replay_into(config.persistence.aof_path, storage, config), "replay failed");
    require(!storage.get("gone").has_value(), "expired key should not recover");
    std::filesystem::remove(config.persistence.aof_path);
  });
}
