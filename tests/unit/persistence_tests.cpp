#include "../test_harness.h"

#include "veloxdb/commands/command_registry.h"
#include "veloxdb/persistence/aof.h"
#include "veloxdb/persistence/snapshot.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {

std::string temp_path(std::string_view name) {
  return (std::filesystem::temp_directory_path() /
          ("veloxdb_" + std::string(name) + "_" + std::to_string(::getpid()) + ".aof"))
      .string();
}

veloxdb::Status replay_into(const std::string& path, veloxdb::StorageEngine& storage,
                            veloxdb::Config& config, size_t start_offset = 0) {
  veloxdb::Metrics metrics;
  veloxdb::CommandRegistry registry;
  veloxdb::register_default_commands(registry);
  veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol,
                                  config.persistence.manifest_path);
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
      },
      start_offset);
  return stats.status;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

  suite.add("AOF rewrite compacts live keyspace", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("rewrite");
    config.persistence.fsync_policy = "always";

    veloxdb::StorageEngine storage(config.storage, config.protocol);
    {
      veloxdb::Aof aof(config.persistence, config.protocol);
      require(aof.open(), "open aof");

      require(storage.set("counter", "0", std::nullopt).code == veloxdb::StorageEngine::WriteCode::Ok,
              "set counter");
      require(aof.append(std::vector<std::string>{"SET", "counter", "0"}), "append set");
      require(storage.incr_by("counter", 1).code == veloxdb::StorageEngine::WriteCode::Ok,
              "incr counter");
      require(aof.append(std::vector<std::string>{"INCR", "counter"}), "append incr");
      require(storage.incr_by("counter", 1).code == veloxdb::StorageEngine::WriteCode::Ok,
              "incr counter again");
      require(aof.append(std::vector<std::string>{"INCR", "counter"}), "append second incr");

      require(storage.set("stale", "value", std::nullopt).code == veloxdb::StorageEngine::WriteCode::Ok,
              "set stale");
      require(aof.append(std::vector<std::string>{"SET", "stale", "value"}), "append stale");
      require(storage.del("stale") == 1, "delete stale");
      require(aof.append(std::vector<std::string>{"DEL", "stale"}), "append delete");

      require(storage.set("ttl", "value", std::nullopt).code == veloxdb::StorageEngine::WriteCode::Ok,
              "set ttl");
      require(aof.append(std::vector<std::string>{"SET", "ttl", "value"}), "append ttl set");
      const auto expire_at = veloxdb::StorageEngine::Clock::now() + std::chrono::hours(1);
      require(storage.expire_at("ttl", expire_at), "expire ttl");
      require(aof.append(std::vector<std::string>{"PEXPIREAT", "ttl",
                                                  std::to_string(veloxdb::unix_ms(expire_at))}),
              "append ttl expiry");

      const size_t old_size = aof.current_size();
      require(aof.rewrite(storage), "rewrite aof");
      require(aof.current_size() < old_size, "rewrite should reduce file size");
      aof.close();
    }

    const std::string compacted = read_file(config.persistence.aof_path);
    require(compacted.find("INCR") == std::string::npos, "rewrite should materialize INCR result");
    require(compacted.find("DEL") == std::string::npos, "rewrite should drop deleted keys");

    veloxdb::StorageEngine recovered(config.storage, config.protocol);
    require(replay_into(config.persistence.aof_path, recovered, config), "replay compacted aof");
    require_eq(*recovered.get("counter"), std::string("2"), "counter recovered once");
    require(!recovered.get("stale").has_value(), "deleted key not recovered");
    require_eq(*recovered.get("ttl"), std::string("value"), "ttl value recovered");
    require(recovered.ttl("ttl").state == veloxdb::StorageEngine::TtlState::HasExpiry,
            "ttl metadata recovered");
    std::filesystem::remove(config.persistence.aof_path);
  });

  suite.add("BGREWRITEAOF command rewrites AOF", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("bgrewrite_command");
    config.persistence.fsync_policy = "always";

    veloxdb::StorageEngine storage(config.storage, config.protocol);
    veloxdb::Metrics metrics;
    veloxdb::CommandRegistry registry;
    veloxdb::register_default_commands(registry);
    veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol);
    veloxdb::Aof aof(config.persistence, config.protocol);
    require(aof.open(), "open aof");

    veloxdb::CommandContext ctx{storage, metrics, config, &aof, &snapshot, {}, false};
    auto exec = [&](std::initializer_list<std::string_view> args) {
      std::vector<std::string_view> vec(args);
      return registry.execute(ctx, vec).response;
    };

    require_eq(exec({"SET", "counter", "0"}), std::string("+OK\r\n"), "SET");
    require_eq(exec({"INCR", "counter"}), std::string(":1\r\n"), "INCR");
    require_eq(exec({"BGREWRITEAOF"}), std::string("+OK\r\n"), "BGREWRITEAOF");
    aof.close();

    const std::string compacted = read_file(config.persistence.aof_path);
    require(compacted.find("INCR") == std::string::npos, "command rewrite should compact INCR");

    veloxdb::StorageEngine recovered(config.storage, config.protocol);
    require(replay_into(config.persistence.aof_path, recovered, config), "replay compacted command aof");
    require_eq(*recovered.get("counter"), std::string("1"), "counter recovered once");
    std::filesystem::remove(config.persistence.aof_path);
  });

  suite.add("snapshot manifest resumes AOF from checkpoint offset", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("manifest_resume");
    config.persistence.snapshot_path = temp_path("manifest_resume_snapshot");
    config.persistence.manifest_path = temp_path("manifest_resume_manifest");
    config.persistence.fsync_policy = "always";

    veloxdb::StorageEngine storage(config.storage, config.protocol);
    veloxdb::Metrics metrics;
    veloxdb::CommandRegistry registry;
    veloxdb::register_default_commands(registry);
    veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol,
                                    config.persistence.manifest_path);
    veloxdb::Aof aof(config.persistence, config.protocol);
    require(aof.open(), "open aof");

    veloxdb::CommandContext ctx{storage, metrics, config, &aof, &snapshot, {}, false};
    auto exec = [&](std::initializer_list<std::string_view> args) {
      std::vector<std::string_view> vec(args);
      return registry.execute(ctx, vec).response;
    };

    require_eq(exec({"SET", "counter", "0"}), std::string("+OK\r\n"), "SET");
    require_eq(exec({"INCR", "counter"}), std::string(":1\r\n"), "INCR before SAVE");
    require_eq(exec({"SAVE"}), std::string("+OK\r\n"), "SAVE writes manifest");
    require(std::filesystem::exists(config.persistence.manifest_path), "manifest should exist");
    const size_t checkpoint_offset = aof.current_size();
    require_eq(exec({"INCR", "counter"}), std::string(":2\r\n"), "INCR after SAVE");
    aof.close();

    const size_t aof_size = static_cast<size_t>(std::filesystem::file_size(config.persistence.aof_path));
    veloxdb::StorageEngine recovered(config.storage, config.protocol);
    const auto loaded = snapshot.load_for_recovery(recovered, config.persistence.aof_path, aof_size);
    require(loaded.status, "load snapshot through manifest");
    require(loaded.loaded_snapshot, "snapshot should load through manifest");
    require_eq(loaded.aof_replay_offset, checkpoint_offset, "manifest checkpoint offset");
    require(replay_into(config.persistence.aof_path, recovered, config, loaded.aof_replay_offset),
            "replay AOF tail");
    require_eq(*recovered.get("counter"), std::string("2"), "counter recovered without double INCR");

    std::filesystem::remove(config.persistence.aof_path);
    std::filesystem::remove(config.persistence.snapshot_path);
    std::filesystem::remove(config.persistence.manifest_path);
  });

  suite.add("AOF rewrite invalidates snapshot manifest offsets", [] {
    veloxdb::Config config;
    config.persistence.aof_path = temp_path("rewrite_manifest");
    config.persistence.snapshot_path = temp_path("rewrite_manifest_snapshot");
    config.persistence.manifest_path = temp_path("rewrite_manifest_marker");
    config.persistence.fsync_policy = "always";

    veloxdb::StorageEngine storage(config.storage, config.protocol);
    veloxdb::Metrics metrics;
    veloxdb::CommandRegistry registry;
    veloxdb::register_default_commands(registry);
    veloxdb::SnapshotStore snapshot(config.persistence.snapshot_path, config.protocol,
                                    config.persistence.manifest_path);
    veloxdb::Aof aof(config.persistence, config.protocol);
    require(aof.open(), "open aof");

    veloxdb::CommandContext ctx{storage, metrics, config, &aof, &snapshot, {}, false};
    auto exec = [&](std::initializer_list<std::string_view> args) {
      std::vector<std::string_view> vec(args);
      return registry.execute(ctx, vec).response;
    };

    require_eq(exec({"SET", "name", "dev"}), std::string("+OK\r\n"), "SET");
    require_eq(exec({"SAVE"}), std::string("+OK\r\n"), "SAVE");
    require(std::filesystem::exists(config.persistence.manifest_path), "manifest should exist");
    require_eq(exec({"BGREWRITEAOF"}), std::string("+OK\r\n"), "BGREWRITEAOF");
    require(!std::filesystem::exists(config.persistence.manifest_path),
            "AOF rewrite should remove stale manifest");
    aof.close();

    veloxdb::StorageEngine recovered(config.storage, config.protocol);
    const size_t aof_size = static_cast<size_t>(std::filesystem::file_size(config.persistence.aof_path));
    const auto loaded = snapshot.load_for_recovery(recovered, config.persistence.aof_path, aof_size);
    require(loaded.status, "stale manifest load status");
    require(!loaded.loaded_snapshot, "snapshot should not load without manifest");
    require(replay_into(config.persistence.aof_path, recovered, config), "replay compacted AOF");
    require_eq(*recovered.get("name"), std::string("dev"), "recovered from rewritten AOF");

    std::filesystem::remove(config.persistence.aof_path);
    std::filesystem::remove(config.persistence.snapshot_path);
    std::filesystem::remove(config.persistence.manifest_path);
  });
}
