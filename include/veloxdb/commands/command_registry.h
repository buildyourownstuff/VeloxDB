#pragma once

#include "veloxdb/config/config.h"
#include "veloxdb/metrics/metrics.h"
#include "veloxdb/persistence/aof.h"
#include "veloxdb/persistence/snapshot.h"
#include "veloxdb/storage/storage_engine.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace veloxdb {

struct CommandResult {
  std::string response;
  bool close_after_write{false};
};

struct CommandContext {
  StorageEngine& storage;
  Metrics& metrics;
  Config& config;
  Aof* aof{nullptr};
  SnapshotStore* snapshot{nullptr};
  std::function<void()> request_shutdown;
  bool replay{false};
};

class Command {
public:
  virtual ~Command() = default;
  virtual CommandResult execute(CommandContext& ctx, const std::vector<std::string_view>& args) = 0;
};

class CommandRegistry {
public:
  void register_command(std::string name, std::unique_ptr<Command> command);
  CommandResult execute(CommandContext& ctx, const std::vector<std::string_view>& args) const;
  [[nodiscard]] std::vector<std::string> command_names() const;

private:
  std::unordered_map<std::string, std::unique_ptr<Command>> commands_;
};

void register_default_commands(CommandRegistry& registry);

} // namespace veloxdb
