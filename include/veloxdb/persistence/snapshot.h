#pragma once

#include "veloxdb/config/config.h"
#include "veloxdb/storage/storage_engine.h"
#include "veloxdb/util/status.h"

#include <string>

namespace veloxdb {

class SnapshotStore {
public:
  SnapshotStore(std::string path, ProtocolConfig protocol);

  Status save(StorageEngine& storage) const;
  Status load(StorageEngine& storage) const;
  [[nodiscard]] const std::string& path() const;

private:
  std::string path_;
  ProtocolConfig protocol_;
};

} // namespace veloxdb
