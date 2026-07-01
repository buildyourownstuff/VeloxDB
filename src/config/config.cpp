#include "veloxdb/config/config.h"

#include "veloxdb/util/string.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace veloxdb {
namespace {

std::string strip_quotes(std::string value) {
  value = util::trim(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::string remove_comment(std::string_view line) {
  bool in_string = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') {
      in_string = !in_string;
    }
    if (!in_string && line[i] == '#') {
      return std::string(line.substr(0, i));
    }
  }
  return std::string(line);
}

Status set_u16(uint16_t& out, std::string_view value, std::string_view name) {
  const auto parsed = util::parse_u64(value);
  if (!parsed.has_value() || *parsed > std::numeric_limits<uint16_t>::max()) {
    return Status::invalid_argument("invalid uint16 value for " + std::string(name));
  }
  out = static_cast<uint16_t>(*parsed);
  return Status::ok();
}

Status set_size(size_t& out, std::string_view value, std::string_view name) {
  const auto parsed = util::parse_u64(value);
  if (!parsed.has_value() || *parsed > std::numeric_limits<size_t>::max()) {
    return Status::invalid_argument("invalid size value for " + std::string(name));
  }
  out = static_cast<size_t>(*parsed);
  return Status::ok();
}

Status set_bool(bool& out, std::string_view value, std::string_view name) {
  const auto parsed = util::parse_bool(value);
  if (!parsed.has_value()) {
    return Status::invalid_argument("invalid bool value for " + std::string(name));
  }
  out = *parsed;
  return Status::ok();
}

Status set_bytes(size_t& out, std::string_view value, std::string_view name) {
  const auto parsed = util::parse_byte_size(value);
  if (!parsed.has_value()) {
    return Status::invalid_argument("invalid byte size for " + std::string(name));
  }
  out = *parsed;
  return Status::ok();
}

std::vector<std::string> args_to_vector(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return args;
}

Status apply_cli_pair(Config& config, std::string_view name, const std::string& value) {
  static const std::unordered_map<std::string, std::string> aliases{
      {"host", "server.host"},
      {"port", "server.port"},
      {"workers", "server.workers"},
      {"max-clients", "server.max_clients"},
      {"storage-shards", "storage.shards"},
      {"max-memory", "storage.max_memory"},
      {"aof-enabled", "persistence.aof_enabled"},
      {"aof-path", "persistence.aof_path"},
      {"fsync-policy", "persistence.fsync_policy"},
      {"snapshot-path", "persistence.snapshot_path"},
      {"manifest-path", "persistence.manifest_path"},
      {"log-level", "logging.level"},
      {"shutdown-enabled", "admin.shutdown_enabled"},
      {"max-request-bytes", "protocol.max_request_bytes"},
      {"max-key-size", "protocol.max_key_size"},
      {"max-value-size", "protocol.max_value_size"},
      {"max-output-buffer-bytes", "protocol.max_output_buffer_bytes"},
      {"expiration-interval-ms", "expiration.interval_ms"},
      {"expiration-active", "expiration.active_enabled"},
  };

  const std::string clean_name = std::string(name);
  const auto it = aliases.find(clean_name);
  if (it == aliases.end()) {
    return Status::invalid_argument("unknown option --" + clean_name);
  }
  return ConfigLoader::apply_kv(config, std::string(it->second), value);
}

} // namespace

ConfigLoadResult ConfigLoader::load(int argc, char** argv) {
  ConfigLoadResult result;
  result.help_text = help_text();

  const std::vector<std::string> args = args_to_vector(argc, argv);
  std::string config_path;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--help" || args[i] == "-h") {
      result.show_help = true;
      return result;
    }
    if (args[i] == "--config") {
      if (i + 1 >= args.size()) {
        result.status = Status::invalid_argument("--config requires a path");
        return result;
      }
      config_path = args[++i];
    } else if (args[i].rfind("--config=", 0) == 0) {
      config_path = args[i].substr(std::string("--config=").size());
    }
  }

  if (!config_path.empty()) {
    result.status = load_file(result.config, config_path);
    if (!result.status) {
      return result;
    }
  }

  result.status = apply_env(result.config);
  if (!result.status) {
    return result;
  }

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--config") {
      ++i;
      continue;
    }
    if (arg.rfind("--config=", 0) == 0) {
      continue;
    }
    if (arg.rfind("--", 0) != 0) {
      result.status = Status::invalid_argument("unexpected positional argument: " + arg);
      return result;
    }

    std::string name;
    std::string value;
    const size_t eq = arg.find('=');
    if (eq != std::string::npos) {
      name = arg.substr(2, eq - 2);
      value = arg.substr(eq + 1);
    } else {
      name = arg.substr(2);
      if (i + 1 >= args.size()) {
        result.status = Status::invalid_argument("--" + name + " requires a value");
        return result;
      }
      value = args[++i];
    }

    result.status = apply_cli_pair(result.config, name, value);
    if (!result.status) {
      return result;
    }
  }

  if (result.config.server.workers == 0) {
    result.config.server.workers = 1;
  }
  if (result.config.storage.shards == 0) {
    result.config.storage.shards = 1;
  }
  if (result.config.expiration.interval_ms == 0) {
    result.config.expiration.interval_ms = 1;
  }
  return result;
}

std::string ConfigLoader::help_text() {
  return R"(VeloxDB 0.1.0

Usage:
  veloxdb [--config ./veloxdb.toml] [options]

Options:
  --host <addr>                         Listen address (default 0.0.0.0)
  --port <port>                         Listen port (default 7379)
  --workers <n>                         Worker event loops
  --max-clients <n>                     Connection limit
  --storage-shards <n>                  Storage shard count
  --max-memory <bytes|kb|mb|gb>         No-eviction write limit
  --aof-enabled <true|false>            Enable append-only persistence
  --aof-path <path>                     AOF path
  --fsync-policy <always|everysec|no>   AOF fsync policy
  --snapshot-path <path>                SAVE snapshot path
  --manifest-path <path>                Snapshot/AOF manifest path
  --log-level <trace|debug|info|warn|error|off>
  --shutdown-enabled <true|false>       Enable SHUTDOWN command
  --max-request-bytes <n>
  --max-key-size <bytes|kb|mb|gb>
  --max-value-size <bytes|kb|mb|gb>
  --expiration-active <true|false>
  --expiration-interval-ms <n>
)";
}

Status ConfigLoader::load_file(Config& config, const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return Status::io_error("failed to open config file: " + path);
  }

  std::string section;
  std::string line;
  size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    line = util::trim(remove_comment(line));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      section = util::trim(std::string_view(line).substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      return Status::invalid_argument("invalid config line " + std::to_string(line_no));
    }
    const std::string key = section.empty()
                                ? util::trim(std::string_view(line).substr(0, eq))
                                : section + "." + util::trim(std::string_view(line).substr(0, eq));
    const std::string value = strip_quotes(std::string(std::string_view(line).substr(eq + 1)));
    const Status status = apply_kv(config, key, value);
    if (!status) {
      return Status::invalid_argument("config line " + std::to_string(line_no) + ": " +
                                      status.message());
    }
  }
  return Status::ok();
}

Status ConfigLoader::apply_env(Config& config) {
  struct EnvMap {
    const char* env;
    const char* key;
  };
  static constexpr EnvMap entries[] = {
      {"VELOXDB_HOST", "server.host"},
      {"VELOXDB_PORT", "server.port"},
      {"VELOXDB_WORKERS", "server.workers"},
      {"VELOXDB_MAX_CLIENTS", "server.max_clients"},
      {"VELOXDB_STORAGE_SHARDS", "storage.shards"},
      {"VELOXDB_MAX_MEMORY", "storage.max_memory"},
      {"VELOXDB_AOF_ENABLED", "persistence.aof_enabled"},
      {"VELOXDB_AOF_PATH", "persistence.aof_path"},
      {"VELOXDB_FSYNC_POLICY", "persistence.fsync_policy"},
      {"VELOXDB_SNAPSHOT_PATH", "persistence.snapshot_path"},
      {"VELOXDB_MANIFEST_PATH", "persistence.manifest_path"},
      {"VELOXDB_LOG_LEVEL", "logging.level"},
      {"VELOXDB_SHUTDOWN_ENABLED", "admin.shutdown_enabled"},
      {"VELOXDB_EXPIRATION_ACTIVE", "expiration.active_enabled"},
      {"VELOXDB_EXPIRATION_INTERVAL_MS", "expiration.interval_ms"},
  };

  for (const auto& entry : entries) {
    const char* value = std::getenv(entry.env);
    if (value == nullptr) {
      continue;
    }
    const Status status = apply_kv(config, entry.key, value);
    if (!status) {
      return Status::invalid_argument(std::string(entry.env) + ": " + status.message());
    }
  }
  return Status::ok();
}

Status ConfigLoader::apply_kv(Config& config, const std::string& key, const std::string& value) {
  const std::string clean_value = strip_quotes(value);

  if (key == "server.host") {
    config.server.host = clean_value;
    return Status::ok();
  }
  if (key == "server.port") {
    return set_u16(config.server.port, clean_value, key);
  }
  if (key == "server.workers") {
    return set_size(config.server.workers, clean_value, key);
  }
  if (key == "server.max_clients") {
    return set_size(config.server.max_clients, clean_value, key);
  }
  if (key == "storage.shards") {
    return set_size(config.storage.shards, clean_value, key);
  }
  if (key == "storage.max_memory") {
    return set_bytes(config.storage.max_memory_bytes, clean_value, key);
  }
  if (key == "storage.max_memory_bytes") {
    return set_size(config.storage.max_memory_bytes, clean_value, key);
  }
  if (key == "persistence.aof_enabled") {
    return set_bool(config.persistence.aof_enabled, clean_value, key);
  }
  if (key == "persistence.aof_path") {
    config.persistence.aof_path = clean_value;
    return Status::ok();
  }
  if (key == "persistence.fsync_policy") {
    const std::string policy = util::to_lower_ascii(clean_value);
    if (policy != "always" && policy != "everysec" && policy != "no") {
      return Status::invalid_argument("fsync policy must be always, everysec, or no");
    }
    config.persistence.fsync_policy = policy;
    return Status::ok();
  }
  if (key == "persistence.snapshot_path") {
    config.persistence.snapshot_path = clean_value;
    return Status::ok();
  }
  if (key == "persistence.manifest_path") {
    config.persistence.manifest_path = clean_value;
    return Status::ok();
  }
  if (key == "expiration.active_enabled") {
    return set_bool(config.expiration.active_enabled, clean_value, key);
  }
  if (key == "expiration.interval_ms") {
    const auto parsed = util::parse_u64(clean_value);
    if (!parsed.has_value()) {
      return Status::invalid_argument("invalid interval");
    }
    config.expiration.interval_ms = *parsed;
    return Status::ok();
  }
  if (key == "expiration.sample_per_shard") {
    return set_size(config.expiration.sample_per_shard, clean_value, key);
  }
  if (key == "logging.level") {
    config.logging.level = util::to_lower_ascii(clean_value);
    return Status::ok();
  }
  if (key == "metrics.enabled") {
    return set_bool(config.metrics.enabled, clean_value, key);
  }
  if (key == "admin.shutdown_enabled") {
    return set_bool(config.admin.shutdown_enabled, clean_value, key);
  }
  if (key == "protocol.max_request_bytes") {
    return set_bytes(config.protocol.max_request_bytes, clean_value, key);
  }
  if (key == "protocol.max_array_elements") {
    return set_size(config.protocol.max_array_elements, clean_value, key);
  }
  if (key == "protocol.max_key_size") {
    return set_bytes(config.protocol.max_key_size, clean_value, key);
  }
  if (key == "protocol.max_value_size") {
    return set_bytes(config.protocol.max_value_size, clean_value, key);
  }
  if (key == "protocol.max_output_buffer_bytes") {
    return set_bytes(config.protocol.max_output_buffer_bytes, clean_value, key);
  }
  return Status::invalid_argument("unknown config key: " + key);
}

} // namespace veloxdb
