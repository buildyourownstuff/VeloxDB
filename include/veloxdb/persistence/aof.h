#pragma once

#include "veloxdb/config/config.h"
#include "veloxdb/protocol/resp.h"
#include "veloxdb/util/status.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace veloxdb {

class StorageEngine;

class Aof {
public:
  enum class FsyncPolicy { Always, EverySec, No };

  struct ReplayStats {
    size_t commands{0};
    bool ignored_partial_tail{false};
    Status status{Status::ok()};
  };

  using ReplayCallback = std::function<Status(const std::vector<std::string>&)>;

  class MutationGuard {
  public:
    MutationGuard() = default;

  private:
    friend class Aof;
    explicit MutationGuard(std::shared_mutex& mu);

    std::shared_lock<std::shared_mutex> lock_;
  };

  class CheckpointGuard {
  public:
    CheckpointGuard() = default;
    [[nodiscard]] size_t offset() const;

  private:
    friend class Aof;
    explicit CheckpointGuard(Aof& aof);

    Aof* aof_{nullptr};
    std::unique_lock<std::shared_mutex> lock_;
    size_t offset_{0};
  };

  Aof(PersistenceConfig config, ProtocolConfig protocol);
  ~Aof();

  Aof(const Aof&) = delete;
  Aof& operator=(const Aof&) = delete;

  Status open();
  void close();
  Status append(const std::vector<std::string_view>& args);
  Status append(const std::vector<std::string>& args);
  Status sync();
  Status rewrite(StorageEngine& storage);
  [[nodiscard]] MutationGuard mutation_guard();
  [[nodiscard]] CheckpointGuard checkpoint_guard();

  [[nodiscard]] bool enabled() const;
  [[nodiscard]] size_t current_size() const;
  [[nodiscard]] bool last_write_ok() const;
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] const std::string& path() const;

  static ReplayStats replay_file(const std::string& path, const ProtocolConfig& protocol,
                                 const ReplayCallback& callback, size_t start_offset = 0);
  static FsyncPolicy parse_policy(std::string_view policy);

private:
  Status write_bytes_locked(std::string_view bytes);
  Status reopen_locked();
  void sync_loop();

  PersistenceConfig config_;
  ProtocolConfig protocol_;
  FsyncPolicy policy_{FsyncPolicy::EverySec};
  int fd_{-1};
  mutable std::mutex mu_;
  std::shared_mutex rewrite_mu_;
  std::atomic<bool> stop_{false};
  std::thread sync_thread_;
  std::atomic<size_t> current_size_{0};
  std::atomic<bool> last_write_ok_{true};
  std::string last_error_;
};

} // namespace veloxdb
