#pragma once

#include "veloxdb/util/status.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace veloxdb {

struct ServerConfig {
  std::string host{"0.0.0.0"};
  uint16_t port{7379};
  size_t workers{4};
  size_t max_clients{10000};
};

struct StorageConfig {
  size_t shards{64};
  size_t max_memory_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct PersistenceConfig {
  bool aof_enabled{true};
  std::string aof_path{"./data/veloxdb.aof"};
  std::string fsync_policy{"everysec"};
  std::string snapshot_path{"./data/veloxdb.snapshot"};
  std::string manifest_path{"./data/veloxdb.manifest"};
};

struct ExpirationConfig {
  bool active_enabled{true};
  uint64_t interval_ms{100};
  size_t sample_per_shard{20};
};

struct LoggingConfig {
  std::string level{"info"};
};

struct MetricsConfig {
  bool enabled{true};
};

struct AdminConfig {
  bool shutdown_enabled{false};
};

struct ProtocolConfig {
  size_t max_request_bytes{64ULL * 1024ULL * 1024ULL};
  size_t max_array_elements{1024};
  size_t max_key_size{512ULL * 1024ULL};
  size_t max_value_size{64ULL * 1024ULL * 1024ULL};
  size_t max_output_buffer_bytes{64ULL * 1024ULL * 1024ULL};
};

struct Config {
  ServerConfig server;
  StorageConfig storage;
  PersistenceConfig persistence;
  ExpirationConfig expiration;
  LoggingConfig logging;
  MetricsConfig metrics;
  AdminConfig admin;
  ProtocolConfig protocol;
};

struct ConfigLoadResult {
  Config config;
  Status status{Status::ok()};
  bool show_help{false};
  std::string help_text;
};

class ConfigLoader {
public:
  static ConfigLoadResult load(int argc, char** argv);
  static std::string help_text();
  static Status apply_kv(Config& config, const std::string& key, const std::string& value);

private:
  static Status load_file(Config& config, const std::string& path);
  static Status apply_env(Config& config);
};

} // namespace veloxdb
