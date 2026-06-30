#include "veloxdb/commands/command_registry.h"

#include "veloxdb/protocol/resp.h"
#include "veloxdb/util/string.h"

#include <algorithm>

namespace veloxdb {

void CommandRegistry::register_command(std::string name, std::unique_ptr<Command> command) {
  commands_[util::to_upper_ascii(name)] = std::move(command);
}

CommandResult CommandRegistry::execute(CommandContext& ctx,
                                       const std::vector<std::string_view>& args) const {
  if (args.empty()) {
    return {resp::error("empty command"), false};
  }

  const std::string command_name = util::to_upper_ascii(args[0]);
  const auto it = commands_.find(command_name);
  if (it == commands_.end()) {
    return {resp::error("unknown command '" + std::string(args[0]) + "'"), false};
  }
  return it->second->execute(ctx, args);
}

std::vector<std::string> CommandRegistry::command_names() const {
  std::vector<std::string> names;
  names.reserve(commands_.size());
  for (const auto& [name, _] : commands_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

} // namespace veloxdb
