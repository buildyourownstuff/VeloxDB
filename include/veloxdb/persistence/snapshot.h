#pragma once

#include "veloxdb/config/config.h"
#include "veloxdb/storage/storage_engine.h"
#include "veloxdb/util/status.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace veloxdb {

class Aof;

class SnapshotStore {
public:
  struct Manifest {
    std::string snapshot_path;
    std::string aof_path;
    size_t aof_offset{0};
    int64_t created_unix_ms{0};
  };

  struct RecoveryLoadResult {
    Status status{Status::ok()};
    bool loaded_snapshot{false};
    bool found_manifest{false};
    size_t aof_replay_offset{0};
  };

  SnapshotStore(std::string path, ProtocolConfig protocol, std::string manifest_path = {});

  Status save(StorageEngine& storage, Aof* aof = nullptr) const;
  Status load(StorageEngine& storage) const;
  RecoveryLoadResult load_for_recovery(StorageEngine& storage, const std::string& aof_path,
                                       size_t aof_size) const;
  [[nodiscard]] const std::string& path() const;
  [[nodiscard]] const std::string& manifest_path() const;

private:
  Status save_snapshot_file(StorageEngine& storage) const;
  Status write_manifest(const Manifest& manifest) const;
  [[nodiscard]] std::optional<Manifest> read_manifest() const;
  [[nodiscard]] bool manifest_matches(const Manifest& manifest, const std::string& aof_path,
                                      size_t aof_size) const;
  void remove_manifest() const;

  std::string path_;
  std::string manifest_path_;
  ProtocolConfig protocol_;
};

} // namespace veloxdb
