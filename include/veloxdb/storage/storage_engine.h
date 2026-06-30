#pragma once

#include "veloxdb/config/config.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace veloxdb {

class StorageEngine {
public:
  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;

  enum class SetCondition { Always, NX, XX };
  enum class WriteCode { Ok, NotSet, NoMemory, KeyTooLarge, ValueTooLarge, InvalidInteger, Overflow };

  struct WriteResult {
    WriteCode code{WriteCode::Ok};
    int64_t integer{0};
  };

  enum class TtlState { Missing, NoExpiry, HasExpiry };
  struct TtlResult {
    TtlState state{TtlState::Missing};
    int64_t milliseconds{0};
  };

  struct SnapshotEntry {
    std::string key;
    std::string value;
    std::optional<int64_t> expire_at_unix_ms;
  };

  explicit StorageEngine(StorageConfig config, ProtocolConfig limits = {});

  WriteResult set(std::string key, std::string value, std::optional<std::chrono::milliseconds> ttl,
                  SetCondition condition = SetCondition::Always);
  std::optional<std::string> get(std::string_view key);
  size_t del(const std::vector<std::string_view>& keys);
  size_t del(std::string_view key);
  size_t exists(const std::vector<std::string_view>& keys);
  std::vector<std::optional<std::string>> mget(const std::vector<std::string_view>& keys);
  WriteResult incr_by(std::string_view key, int64_t delta);
  WriteResult append(std::string_view key, std::string_view suffix);
  size_t strlen(std::string_view key);

  bool expire(std::string_view key, std::chrono::milliseconds ttl);
  bool expire_at(std::string_view key, TimePoint expire_at);
  TtlResult ttl(std::string_view key);
  bool persist(std::string_view key);

  size_t dbsize();
  void flushdb();
  size_t active_expire(size_t sample_per_shard);

  [[nodiscard]] size_t shard_count() const;
  [[nodiscard]] size_t shard_for_key(std::string_view key) const;
  [[nodiscard]] size_t used_memory_estimate() const;
  [[nodiscard]] size_t expired_keys() const;
  [[nodiscard]] size_t expires_count();
  [[nodiscard]] std::vector<SnapshotEntry> snapshot();

private:
  struct Entry {
    std::string value;
    std::optional<TimePoint> expiry;
    uint64_t version{0};
    TimePoint last_access{Clock::now()};
  };

  struct Shard {
    mutable std::shared_mutex mu;
    std::unordered_map<std::string, Entry> map;
    size_t memory_bytes{0};
    size_t expire_cursor{0};
  };

  Shard& shard(std::string_view key);
  const Shard& shard(std::string_view key) const;

  [[nodiscard]] bool expired(const Entry& entry, TimePoint now) const;
  bool erase_if_expired_locked(Shard& shard, const std::string& key, TimePoint now);
  void erase_entry_locked(Shard& shard, std::unordered_map<std::string, Entry>::iterator it);
  [[nodiscard]] size_t entry_memory(std::string_view key, std::string_view value) const;
  bool reserve_memory(size_t bytes);
  void release_memory(size_t bytes);
  [[nodiscard]] bool key_valid(std::string_view key) const;
  [[nodiscard]] bool value_valid(std::string_view value) const;

  StorageConfig config_;
  ProtocolConfig limits_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<size_t> used_memory_{0};
  std::atomic<size_t> expired_keys_{0};
};

std::string storage_write_error(StorageEngine::WriteCode code);
int64_t unix_ms(StorageEngine::TimePoint tp);
StorageEngine::TimePoint timepoint_from_unix_ms(int64_t milliseconds);

} // namespace veloxdb
