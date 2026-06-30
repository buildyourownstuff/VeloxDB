#include "veloxdb/commands/command_registry.h"

#include "veloxdb/protocol/resp.h"
#include "veloxdb/util/logger.h"
#include "veloxdb/util/string.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

#ifndef VELOXDB_VERSION
#define VELOXDB_VERSION "0.1.0"
#endif

namespace veloxdb {
namespace {

class FunctionalCommand final : public Command {
public:
  using Fn = std::function<CommandResult(CommandContext&, const std::vector<std::string_view>&)>;
  explicit FunctionalCommand(Fn fn) : fn_(std::move(fn)) {}
  CommandResult execute(CommandContext& ctx, const std::vector<std::string_view>& args) override {
    return fn_(ctx, args);
  }

private:
  Fn fn_;
};

std::unique_ptr<Command> cmd(FunctionalCommand::Fn fn) {
  return std::make_unique<FunctionalCommand>(std::move(fn));
}

CommandResult ok() { return {resp::simple("OK"), false}; }

CommandResult err(std::string_view message) { return {resp::error(message), false}; }

CommandResult wrong_args(std::string_view command) {
  return err("wrong number of arguments for '" + util::to_lower_ascii(command) + "' command");
}

std::vector<std::string> copy_args(const std::vector<std::string_view>& args) {
  std::vector<std::string> copied;
  copied.reserve(args.size());
  for (std::string_view arg : args) {
    copied.emplace_back(arg);
  }
  return copied;
}

Status persist(CommandContext& ctx, const std::vector<std::string>& args) {
  if (ctx.replay || ctx.aof == nullptr || !ctx.aof->enabled()) {
    return Status::ok();
  }
  return ctx.aof->append(args);
}

CommandResult persist_or_ok(CommandContext& ctx, const std::vector<std::string>& args,
                            CommandResult result = ok()) {
  const Status status = persist(ctx, args);
  if (!status) {
    ctx.metrics.aof_write_failed();
    return err("persistence append failed: " + status.message());
  }
  ctx.metrics.aof_write_succeeded();
  return result;
}

std::optional<int64_t> parse_i64_arg(std::string_view arg) { return util::parse_i64(arg); }

std::optional<std::chrono::milliseconds> parse_ttl_ms(std::string_view value,
                                                     int64_t multiplier) {
  const auto parsed = parse_i64_arg(value);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  if (*parsed > 0 && *parsed > (std::numeric_limits<int64_t>::max() / multiplier)) {
    return std::nullopt;
  }
  return std::chrono::milliseconds(*parsed * multiplier);
}

std::string pxat_arg_from_ttl(std::chrono::milliseconds ttl) {
  return std::to_string(unix_ms(StorageEngine::Clock::now() + ttl));
}

std::vector<std::optional<std::string>> bulk_pairs(const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::vector<std::optional<std::string>> values;
  values.reserve(pairs.size() * 2);
  for (const auto& [key, value] : pairs) {
    values.push_back(key);
    values.push_back(value);
  }
  return values;
}

CommandResult ping(CommandContext&, const std::vector<std::string_view>& args) {
  if (args.size() == 1) {
    return {resp::simple("PONG"), false};
  }
  if (args.size() == 2) {
    return {resp::bulk(args[1]), false};
  }
  return wrong_args(args[0]);
}

CommandResult echo(CommandContext&, const std::vector<std::string_view>& args) {
  if (args.size() != 2) {
    return wrong_args(args[0]);
  }
  return {resp::bulk(args[1]), false};
}

CommandResult get(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 2) {
    return wrong_args(args[0]);
  }
  const auto value = ctx.storage.get(args[1]);
  return {value.has_value() ? resp::bulk(*value) : resp::null_bulk(), false};
}

CommandResult set(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() < 3) {
    return wrong_args(args[0]);
  }

  StorageEngine::SetCondition condition = StorageEngine::SetCondition::Always;
  std::optional<std::chrono::milliseconds> ttl;
  std::optional<int64_t> pxat;

  for (size_t i = 3; i < args.size(); ++i) {
    const std::string option = util::to_upper_ascii(args[i]);
    if (option == "NX") {
      if (condition == StorageEngine::SetCondition::XX) {
        return err("syntax error");
      }
      condition = StorageEngine::SetCondition::NX;
    } else if (option == "XX") {
      if (condition == StorageEngine::SetCondition::NX) {
        return err("syntax error");
      }
      condition = StorageEngine::SetCondition::XX;
    } else if (option == "EX" || option == "PX" || option == "EXAT" || option == "PXAT") {
      if (i + 1 >= args.size() || ttl.has_value() || pxat.has_value()) {
        return err("syntax error");
      }
      if (option == "EX") {
        ttl = parse_ttl_ms(args[++i], 1000);
      } else if (option == "PX") {
        ttl = parse_ttl_ms(args[++i], 1);
      } else if (option == "EXAT") {
        const auto parsed = parse_i64_arg(args[++i]);
        if (!parsed.has_value()) {
          return err("invalid expire time in SET");
        }
        pxat = *parsed * 1000;
      } else {
        const auto parsed = parse_i64_arg(args[++i]);
        if (!parsed.has_value()) {
          return err("invalid expire time in SET");
        }
        pxat = *parsed;
      }
      if ((ttl.has_value() && ttl->count() <= 0) || (pxat.has_value() && *pxat <= 0)) {
        return err("invalid expire time in SET");
      }
    } else {
      return err("syntax error");
    }
  }

  if (pxat.has_value()) {
    ttl = std::chrono::duration_cast<std::chrono::milliseconds>(
        timepoint_from_unix_ms(*pxat) - StorageEngine::Clock::now());
  }

  auto result = ctx.storage.set(std::string(args[1]), std::string(args[2]), ttl, condition);
  if (result.code == StorageEngine::WriteCode::NotSet) {
    return {resp::null_bulk(), false};
  }
  if (result.code != StorageEngine::WriteCode::Ok) {
    return err(storage_write_error(result.code));
  }

  std::vector<std::string> persisted{"SET", std::string(args[1]), std::string(args[2])};
  if (ttl.has_value()) {
    persisted.emplace_back("PXAT");
    persisted.emplace_back(pxat.has_value() ? std::to_string(*pxat) : pxat_arg_from_ttl(*ttl));
  }
  return persist_or_ok(ctx, persisted);
}

CommandResult del(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() < 2) {
    return wrong_args(args[0]);
  }
  std::vector<std::string_view> keys(args.begin() + 1, args.end());
  const size_t deleted = ctx.storage.del(keys);
  if (deleted != 0) {
    const Status status = persist(ctx, copy_args(args));
    if (!status) {
      ctx.metrics.aof_write_failed();
      return err("persistence append failed: " + status.message());
    }
  }
  return {resp::integer(static_cast<int64_t>(deleted)), false};
}

CommandResult exists(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() < 2) {
    return wrong_args(args[0]);
  }
  std::vector<std::string_view> keys(args.begin() + 1, args.end());
  return {resp::integer(static_cast<int64_t>(ctx.storage.exists(keys))), false};
}

CommandResult mget(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() < 2) {
    return wrong_args(args[0]);
  }
  std::vector<std::string_view> keys(args.begin() + 1, args.end());
  return {resp::array_of_bulk(ctx.storage.mget(keys)), false};
}

CommandResult mset(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() < 3 || args.size() % 2 == 0) {
    return wrong_args(args[0]);
  }
  for (size_t i = 1; i < args.size(); i += 2) {
    const auto result = ctx.storage.set(std::string(args[i]), std::string(args[i + 1]), std::nullopt);
    if (result.code != StorageEngine::WriteCode::Ok) {
      return err(storage_write_error(result.code));
    }
  }
  return persist_or_ok(ctx, copy_args(args));
}

CommandResult incr_decr(CommandContext& ctx, const std::vector<std::string_view>& args, int64_t delta) {
  if (args.size() != 2) {
    return wrong_args(args[0]);
  }
  const auto result = ctx.storage.incr_by(args[1], delta);
  if (result.code != StorageEngine::WriteCode::Ok) {
    return err(storage_write_error(result.code));
  }
  return persist_or_ok(ctx, copy_args(args), {resp::integer(result.integer), false});
}

CommandResult append(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 3) {
    return wrong_args(args[0]);
  }
  const auto result = ctx.storage.append(args[1], args[2]);
  if (result.code != StorageEngine::WriteCode::Ok) {
    return err(storage_write_error(result.code));
  }
  return persist_or_ok(ctx, copy_args(args), {resp::integer(result.integer), false});
}

CommandResult strlen_cmd(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 2) {
    return wrong_args(args[0]);
  }
  return {resp::integer(static_cast<int64_t>(ctx.storage.strlen(args[1]))), false};
}

CommandResult expire_impl(CommandContext& ctx, const std::vector<std::string_view>& args,
                          int64_t multiplier, std::string_view command_name) {
  if (args.size() != 3) {
    return wrong_args(args[0]);
  }
  const auto ttl = parse_ttl_ms(args[2], multiplier);
  if (!ttl.has_value()) {
    return err("invalid expire time in " + std::string(command_name));
  }
  const bool changed = ctx.storage.expire(args[1], *ttl);
  if (changed) {
    const std::string pxat = pxat_arg_from_ttl(*ttl);
    const Status status = persist(ctx, std::vector<std::string>{"PEXPIREAT", std::string(args[1]), pxat});
    if (!status) {
      ctx.metrics.aof_write_failed();
      return err("persistence append failed: " + status.message());
    }
  }
  return {resp::integer(changed ? 1 : 0), false};
}

CommandResult pexpireat(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 3) {
    return wrong_args(args[0]);
  }
  const auto when = parse_i64_arg(args[2]);
  if (!when.has_value()) {
    return err("invalid expire time in PEXPIREAT");
  }
  const bool changed = ctx.storage.expire_at(args[1], timepoint_from_unix_ms(*when));
  if (changed) {
    const Status status = persist(ctx, copy_args(args));
    if (!status) {
      ctx.metrics.aof_write_failed();
      return err("persistence append failed: " + status.message());
    }
  }
  return {resp::integer(changed ? 1 : 0), false};
}

CommandResult ttl_impl(CommandContext& ctx, const std::vector<std::string_view>& args, bool millis) {
  if (args.size() != 2) {
    return wrong_args(args[0]);
  }
  const auto ttl = ctx.storage.ttl(args[1]);
  if (ttl.state == StorageEngine::TtlState::Missing) {
    return {resp::integer(-2), false};
  }
  if (ttl.state == StorageEngine::TtlState::NoExpiry) {
    return {resp::integer(-1), false};
  }
  if (millis) {
    return {resp::integer(ttl.milliseconds), false};
  }
  return {resp::integer((ttl.milliseconds + 999) / 1000), false};
}

CommandResult persist_cmd(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 2) {
    return wrong_args(args[0]);
  }
  const bool changed = ctx.storage.persist(args[1]);
  if (changed) {
    const Status status = persist(ctx, copy_args(args));
    if (!status) {
      ctx.metrics.aof_write_failed();
      return err("persistence append failed: " + status.message());
    }
  }
  return {resp::integer(changed ? 1 : 0), false};
}

CommandResult dbsize(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 1) {
    return wrong_args(args[0]);
  }
  return {resp::integer(static_cast<int64_t>(ctx.storage.dbsize())), false};
}

CommandResult flushdb(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 1) {
    return wrong_args(args[0]);
  }
  ctx.storage.flushdb();
  return persist_or_ok(ctx, std::vector<std::string>{"FLUSHDB"});
}

CommandResult info(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() > 2) {
    return wrong_args(args[0]);
  }
  std::ostringstream out;
  out << "# Server\r\n";
  out << "veloxdb_version:" << VELOXDB_VERSION << "\r\n";
  out << "uptime_seconds:" << ctx.metrics.uptime_seconds() << "\r\n";
  out << "tcp_port:" << ctx.config.server.port << "\r\n";
  out << "connected_clients:" << ctx.metrics.connected_clients() << "\r\n\r\n";

  out << "# Memory\r\n";
  out << "used_memory_estimate:" << ctx.storage.used_memory_estimate() << "\r\n\r\n";

  out << "# Stats\r\n";
  out << "total_commands_processed:" << ctx.metrics.total_commands_processed() << "\r\n";
  out << "total_connections_received:" << ctx.metrics.total_connections_received() << "\r\n";
  out << "expired_keys:" << ctx.storage.expired_keys() << "\r\n";
  out << "evicted_keys:" << ctx.metrics.evicted_keys() << "\r\n\r\n";

  out << "# Keyspace\r\n";
  out << "db0:keys=" << ctx.storage.dbsize() << ",expires=" << ctx.storage.expires_count()
      << "\r\n\r\n";

  out << "# Persistence\r\n";
  out << "aof_enabled:" << ((ctx.aof != nullptr && ctx.aof->enabled()) ? "1" : "0") << "\r\n";
  out << "aof_current_size:" << (ctx.aof == nullptr ? 0 : ctx.aof->current_size()) << "\r\n";
  out << "aof_last_write_status:" << ((ctx.aof == nullptr || ctx.aof->last_write_ok()) ? "ok" : "err")
      << "\r\n";
  return {resp::bulk(out.str()), false};
}

CommandResult command_cmd(CommandRegistry& registry, const std::vector<std::string_view>& args) {
  if (args.size() != 1) {
    return wrong_args(args[0]);
  }
  std::vector<std::optional<std::string>> names;
  for (const auto& name : registry.command_names()) {
    names.push_back(name);
  }
  return {resp::array_of_bulk(names), false};
}

std::vector<std::pair<std::string, std::string>> config_values(CommandContext& ctx) {
  return {
      {"server.host", ctx.config.server.host},
      {"server.port", std::to_string(ctx.config.server.port)},
      {"server.workers", std::to_string(ctx.config.server.workers)},
      {"server.max_clients", std::to_string(ctx.config.server.max_clients)},
      {"storage.shards", std::to_string(ctx.config.storage.shards)},
      {"storage.max_memory", std::to_string(ctx.config.storage.max_memory_bytes)},
      {"persistence.aof_enabled", ctx.config.persistence.aof_enabled ? "yes" : "no"},
      {"persistence.aof_path", ctx.config.persistence.aof_path},
      {"persistence.fsync_policy", ctx.config.persistence.fsync_policy},
      {"expiration.active_enabled", ctx.config.expiration.active_enabled ? "yes" : "no"},
      {"expiration.interval_ms", std::to_string(ctx.config.expiration.interval_ms)},
      {"logging.level", std::string(log_level_name(Logger::instance().level()))},
      {"metrics.enabled", ctx.metrics.enabled() ? "yes" : "no"},
      {"admin.shutdown_enabled", ctx.config.admin.shutdown_enabled ? "yes" : "no"},
      {"protocol.max_request_bytes", std::to_string(ctx.config.protocol.max_request_bytes)},
      {"protocol.max_key_size", std::to_string(ctx.config.protocol.max_key_size)},
      {"protocol.max_value_size", std::to_string(ctx.config.protocol.max_value_size)},
  };
}

CommandResult config_cmd(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() < 2) {
    return wrong_args(args[0]);
  }
  const std::string sub = util::to_upper_ascii(args[1]);
  if (sub == "GET") {
    if (args.size() != 3) {
      return wrong_args(args[0]);
    }
    const std::string pattern = util::to_lower_ascii(args[2]);
    std::vector<std::pair<std::string, std::string>> matched;
    for (const auto& pair : config_values(ctx)) {
      if (pattern == "*" || pattern == util::to_lower_ascii(pair.first)) {
        matched.push_back(pair);
      }
    }
    return {resp::array_of_bulk(bulk_pairs(matched)), false};
  }
  if (sub == "SET") {
    if (args.size() != 4) {
      return wrong_args(args[0]);
    }
    const std::string key = util::to_lower_ascii(args[2]);
    if (key == "logging.level") {
      Logger::instance().set_level(parse_log_level(args[3]));
      return ok();
    }
    if (key == "metrics.enabled") {
      const auto parsed = util::parse_bool(args[3]);
      if (!parsed.has_value()) {
        return err("invalid bool value");
      }
      ctx.metrics.set_enabled(*parsed);
      return ok();
    }
    return err("CONFIG SET only supports safe runtime values: logging.level, metrics.enabled");
  }
  return err("unsupported CONFIG subcommand");
}

CommandResult save(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() != 1) {
    return wrong_args(args[0]);
  }
  if (ctx.snapshot == nullptr) {
    return err("snapshot store is not configured");
  }
  const Status status = ctx.snapshot->save(ctx.storage);
  if (!status) {
    log_error("snapshot", status.to_string());
    return err(status.message());
  }
  return ok();
}

CommandResult bgsave(CommandContext&, const std::vector<std::string_view>& args) {
  if (args.size() != 1) {
    return wrong_args(args[0]);
  }
  return err("BGSAVE is planned; use SAVE for the MVP snapshot path");
}

CommandResult shutdown(CommandContext& ctx, const std::vector<std::string_view>& args) {
  if (args.size() > 2) {
    return wrong_args(args[0]);
  }
  if (!ctx.config.admin.shutdown_enabled) {
    return err("SHUTDOWN is disabled by configuration");
  }
  if (ctx.aof != nullptr) {
    (void)ctx.aof->sync();
  }
  if (ctx.request_shutdown) {
    ctx.request_shutdown();
  }
  return {resp::simple("OK"), true};
}

} // namespace

void register_default_commands(CommandRegistry& registry) {
  registry.register_command("PING", cmd(ping));
  registry.register_command("ECHO", cmd(echo));
  registry.register_command("GET", cmd(get));
  registry.register_command("SET", cmd(set));
  registry.register_command("DEL", cmd(del));
  registry.register_command("EXISTS", cmd(exists));
  registry.register_command("MGET", cmd(mget));
  registry.register_command("MSET", cmd(mset));
  registry.register_command("INCR", cmd([](CommandContext& ctx, const auto& args) {
    return incr_decr(ctx, args, 1);
  }));
  registry.register_command("DECR", cmd([](CommandContext& ctx, const auto& args) {
    return incr_decr(ctx, args, -1);
  }));
  registry.register_command("APPEND", cmd(append));
  registry.register_command("STRLEN", cmd(strlen_cmd));
  registry.register_command("EXPIRE", cmd([](CommandContext& ctx, const auto& args) {
    return expire_impl(ctx, args, 1000, "EXPIRE");
  }));
  registry.register_command("PEXPIRE", cmd([](CommandContext& ctx, const auto& args) {
    return expire_impl(ctx, args, 1, "PEXPIRE");
  }));
  registry.register_command("PEXPIREAT", cmd(pexpireat));
  registry.register_command("TTL", cmd([](CommandContext& ctx, const auto& args) {
    return ttl_impl(ctx, args, false);
  }));
  registry.register_command("PTTL", cmd([](CommandContext& ctx, const auto& args) {
    return ttl_impl(ctx, args, true);
  }));
  registry.register_command("PERSIST", cmd(persist_cmd));
  registry.register_command("DBSIZE", cmd(dbsize));
  registry.register_command("FLUSHDB", cmd(flushdb));
  registry.register_command("INFO", cmd(info));
  registry.register_command("CONFIG", cmd(config_cmd));
  registry.register_command("SAVE", cmd(save));
  registry.register_command("BGSAVE", cmd(bgsave));
  registry.register_command("SHUTDOWN", cmd(shutdown));
  registry.register_command("COMMAND", cmd([&registry](CommandContext&, const auto& args) {
    return command_cmd(registry, args);
  }));
}

} // namespace veloxdb
