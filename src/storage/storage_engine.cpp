#include "veloxdb/storage/storage_engine.h"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>

namespace veloxdb {
namespace {

constexpr size_t kEntryOverhead = 96;

std::optional<int64_t> parse_integer_value(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  int64_t parsed = 0;
  const auto* first = value.data();
  const auto* last = value.data() + value.size();
  const auto result = std::from_chars(first, last, parsed, 10);
  if (result.ec != std::errc() || result.ptr != last) {
    return std::nullopt;
  }
  return parsed;
}

} // namespace

StorageEngine::StorageEngine(StorageConfig config, ProtocolConfig limits)
    : config_(config), limits_(limits) {
  if (config_.shards == 0) {
    config_.shards = 1;
  }
  shards_.reserve(config_.shards);
  for (size_t i = 0; i < config_.shards; ++i) {
    shards_.push_back(std::make_unique<Shard>());
  }
}

StorageEngine::WriteResult StorageEngine::set(std::string key, std::string value,
                                              std::optional<std::chrono::milliseconds> ttl,
                                              SetCondition condition) {
  if (!key_valid(key)) {
    return {WriteCode::KeyTooLarge, 0};
  }
  if (!value_valid(value)) {
    return {WriteCode::ValueTooLarge, 0};
  }

  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  erase_if_expired_locked(selected, key, now);

  auto it = selected.map.find(key);
  const bool exists = it != selected.map.end();
  if ((condition == SetCondition::NX && exists) || (condition == SetCondition::XX && !exists)) {
    return {WriteCode::NotSet, 0};
  }

  const size_t old_memory = exists ? entry_memory(it->first, it->second.value) : 0;
  const size_t new_memory = entry_memory(key, value);
  if (new_memory > old_memory && !reserve_memory(new_memory - old_memory)) {
    return {WriteCode::NoMemory, 0};
  }

  try {
    Entry entry;
    entry.value = std::move(value);
    entry.expiry = ttl.has_value() ? std::optional<TimePoint>(now + *ttl) : std::nullopt;
    entry.last_access = now;
    entry.version = exists ? it->second.version + 1 : 1;

    if (exists) {
      it->second = std::move(entry);
      selected.memory_bytes = selected.memory_bytes - old_memory + new_memory;
    } else {
      selected.map.emplace(std::move(key), std::move(entry));
      selected.memory_bytes += new_memory;
    }
  } catch (...) {
    if (new_memory > old_memory) {
      release_memory(new_memory - old_memory);
    }
    return {WriteCode::NoMemory, 0};
  }

  if (old_memory > new_memory) {
    release_memory(old_memory - new_memory);
  }
  return {WriteCode::Ok, 0};
}

std::optional<std::string> StorageEngine::get(std::string_view key) {
  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it == selected.map.end()) {
    return std::nullopt;
  }
  if (expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }
  it->second.last_access = now;
  return it->second.value;
}

size_t StorageEngine::del(const std::vector<std::string_view>& keys) {
  size_t deleted = 0;
  for (std::string_view key : keys) {
    deleted += del(key);
  }
  return deleted;
}

size_t StorageEngine::del(std::string_view key) {
  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it == selected.map.end()) {
    return 0;
  }
  if (expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  erase_entry_locked(selected, it);
  return 1;
}

size_t StorageEngine::exists(const std::vector<std::string_view>& keys) {
  size_t count = 0;
  for (std::string_view key : keys) {
    if (get(key).has_value()) {
      ++count;
    }
  }
  return count;
}

std::vector<std::optional<std::string>>
StorageEngine::mget(const std::vector<std::string_view>& keys) {
  std::vector<std::optional<std::string>> values;
  values.reserve(keys.size());
  for (std::string_view key : keys) {
    values.push_back(get(key));
  }
  return values;
}

StorageEngine::WriteResult StorageEngine::incr_by(std::string_view key, int64_t delta) {
  if (!key_valid(key)) {
    return {WriteCode::KeyTooLarge, 0};
  }

  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it != selected.map.end() && expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    it = selected.map.end();
  }

  int64_t current = 0;
  if (it != selected.map.end()) {
    const auto parsed = parse_integer_value(it->second.value);
    if (!parsed.has_value()) {
      return {WriteCode::InvalidInteger, 0};
    }
    current = *parsed;
  }

  if ((delta > 0 && current > std::numeric_limits<int64_t>::max() - delta) ||
      (delta < 0 && current < std::numeric_limits<int64_t>::min() - delta)) {
    return {WriteCode::Overflow, 0};
  }

  const int64_t next = current + delta;
  std::string next_value = std::to_string(next);
  if (!value_valid(next_value)) {
    return {WriteCode::ValueTooLarge, 0};
  }

  const size_t old_memory = it == selected.map.end() ? 0 : entry_memory(it->first, it->second.value);
  const size_t new_memory = entry_memory(key, next_value);
  if (new_memory > old_memory && !reserve_memory(new_memory - old_memory)) {
    return {WriteCode::NoMemory, 0};
  }

  try {
    if (it == selected.map.end()) {
      Entry entry;
      entry.value = std::move(next_value);
      entry.last_access = now;
      entry.version = 1;
      selected.map.emplace(std::string(key), std::move(entry));
      selected.memory_bytes += new_memory;
    } else {
      it->second.value = std::move(next_value);
      it->second.expiry.reset();
      it->second.last_access = now;
      ++it->second.version;
      selected.memory_bytes = selected.memory_bytes - old_memory + new_memory;
    }
  } catch (...) {
    if (new_memory > old_memory) {
      release_memory(new_memory - old_memory);
    }
    return {WriteCode::NoMemory, 0};
  }

  if (old_memory > new_memory) {
    release_memory(old_memory - new_memory);
  }
  return {WriteCode::Ok, next};
}

StorageEngine::WriteResult StorageEngine::append(std::string_view key, std::string_view suffix) {
  if (!key_valid(key)) {
    return {WriteCode::KeyTooLarge, 0};
  }

  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it != selected.map.end() && expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    it = selected.map.end();
  }

  std::string next_value = it == selected.map.end() ? std::string() : it->second.value;
  next_value.append(suffix);
  if (!value_valid(next_value)) {
    return {WriteCode::ValueTooLarge, 0};
  }
  const int64_t next_length = static_cast<int64_t>(next_value.size());

  const size_t old_memory = it == selected.map.end() ? 0 : entry_memory(it->first, it->second.value);
  const size_t new_memory = entry_memory(key, next_value);
  if (new_memory > old_memory && !reserve_memory(new_memory - old_memory)) {
    return {WriteCode::NoMemory, 0};
  }

  try {
    if (it == selected.map.end()) {
      Entry entry;
      entry.value = std::move(next_value);
      entry.last_access = now;
      entry.version = 1;
      selected.map.emplace(std::string(key), std::move(entry));
      selected.memory_bytes += new_memory;
    } else {
      it->second.value = std::move(next_value);
      it->second.expiry.reset();
      it->second.last_access = now;
      ++it->second.version;
      selected.memory_bytes = selected.memory_bytes - old_memory + new_memory;
    }
  } catch (...) {
    if (new_memory > old_memory) {
      release_memory(new_memory - old_memory);
    }
    return {WriteCode::NoMemory, 0};
  }

  if (old_memory > new_memory) {
    release_memory(old_memory - new_memory);
  }
  return {WriteCode::Ok, next_length};
}

size_t StorageEngine::strlen(std::string_view key) {
  const auto value = get(key);
  return value.has_value() ? value->size() : 0;
}

bool StorageEngine::expire(std::string_view key, std::chrono::milliseconds ttl) {
  if (ttl.count() <= 0) {
    return del(key) == 1;
  }
  return expire_at(key, Clock::now() + ttl);
}

bool StorageEngine::expire_at(std::string_view key, TimePoint expire_at_tp) {
  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it == selected.map.end()) {
    return false;
  }
  if (expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (expire_at_tp <= now) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  it->second.expiry = expire_at_tp;
  ++it->second.version;
  return true;
}

StorageEngine::TtlResult StorageEngine::ttl(std::string_view key) {
  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it == selected.map.end()) {
    return {TtlState::Missing, 0};
  }
  if (expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return {TtlState::Missing, 0};
  }
  if (!it->second.expiry.has_value()) {
    return {TtlState::NoExpiry, 0};
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(*it->second.expiry - now).count();
  return {TtlState::HasExpiry, std::max<int64_t>(0, remaining)};
}

bool StorageEngine::persist(std::string_view key) {
  Shard& selected = shard(key);
  const TimePoint now = Clock::now();
  std::unique_lock lock(selected.mu);
  auto it = selected.map.find(std::string(key));
  if (it == selected.map.end()) {
    return false;
  }
  if (expired(it->second, now)) {
    erase_entry_locked(selected, it);
    expired_keys_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!it->second.expiry.has_value()) {
    return false;
  }
  it->second.expiry.reset();
  ++it->second.version;
  return true;
}

size_t StorageEngine::dbsize() {
  size_t total = 0;
  const TimePoint now = Clock::now();
  for (auto& selected : shards_) {
    std::unique_lock lock(selected->mu);
    for (auto it = selected->map.begin(); it != selected->map.end();) {
      if (expired(it->second, now)) {
        const size_t memory = entry_memory(it->first, it->second.value);
        release_memory(memory);
        selected->memory_bytes -= memory;
        it = selected->map.erase(it);
        expired_keys_.fetch_add(1, std::memory_order_relaxed);
      } else {
        ++total;
        ++it;
      }
    }
  }
  return total;
}

void StorageEngine::flushdb() {
  size_t released = 0;
  for (auto& selected : shards_) {
    std::unique_lock lock(selected->mu);
    released += selected->memory_bytes;
    selected->memory_bytes = 0;
    selected->map.clear();
  }
  release_memory(released);
}

size_t StorageEngine::active_expire(size_t sample_per_shard) {
  size_t expired_count = 0;
  const TimePoint now = Clock::now();
  for (auto& selected : shards_) {
    size_t sampled = 0;
    std::unique_lock lock(selected->mu);
    if (selected->map.empty()) {
      selected->expire_cursor = 0;
      continue;
    }
    selected->expire_cursor %= selected->map.size();
    auto it = selected->map.begin();
    std::advance(
        it,
        static_cast<std::unordered_map<std::string, Entry>::difference_type>(selected->expire_cursor));

    const size_t target = std::min(sample_per_shard, selected->map.size());
    while (!selected->map.empty() && sampled < target) {
      ++sampled;
      if (expired(it->second, now)) {
        const size_t memory = entry_memory(it->first, it->second.value);
        release_memory(memory);
        selected->memory_bytes -= memory;
        it = selected->map.erase(it);
        ++expired_count;
      } else {
        ++it;
      }
      if (it == selected->map.end()) {
        it = selected->map.begin();
      }
    }
    selected->expire_cursor =
        selected->map.empty() ? 0 : (selected->expire_cursor + sampled) % selected->map.size();
  }
  if (expired_count != 0) {
    expired_keys_.fetch_add(expired_count, std::memory_order_relaxed);
  }
  return expired_count;
}

size_t StorageEngine::shard_count() const { return shards_.size(); }

size_t StorageEngine::shard_for_key(std::string_view key) const {
  return std::hash<std::string_view>{}(key) % shards_.size();
}

size_t StorageEngine::used_memory_estimate() const {
  return used_memory_.load(std::memory_order_relaxed);
}

size_t StorageEngine::expired_keys() const {
  return expired_keys_.load(std::memory_order_relaxed);
}

size_t StorageEngine::expires_count() {
  size_t total = 0;
  const TimePoint now = Clock::now();
  for (auto& selected : shards_) {
    std::unique_lock lock(selected->mu);
    for (auto it = selected->map.begin(); it != selected->map.end();) {
      if (expired(it->second, now)) {
        const size_t memory = entry_memory(it->first, it->second.value);
        release_memory(memory);
        selected->memory_bytes -= memory;
        it = selected->map.erase(it);
        expired_keys_.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (it->second.expiry.has_value()) {
          ++total;
        }
        ++it;
      }
    }
  }
  return total;
}

std::vector<StorageEngine::SnapshotEntry> StorageEngine::snapshot() {
  std::vector<SnapshotEntry> entries;
  const TimePoint now = Clock::now();
  for (auto& selected : shards_) {
    std::unique_lock lock(selected->mu);
    for (auto it = selected->map.begin(); it != selected->map.end();) {
      if (expired(it->second, now)) {
        const size_t memory = entry_memory(it->first, it->second.value);
        release_memory(memory);
        selected->memory_bytes -= memory;
        it = selected->map.erase(it);
        expired_keys_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      SnapshotEntry entry;
      entry.key = it->first;
      entry.value = it->second.value;
      if (it->second.expiry.has_value()) {
        entry.expire_at_unix_ms = unix_ms(*it->second.expiry);
      }
      entries.push_back(std::move(entry));
      ++it;
    }
  }
  return entries;
}

StorageEngine::Shard& StorageEngine::shard(std::string_view key) {
  return *shards_[shard_for_key(key)];
}

const StorageEngine::Shard& StorageEngine::shard(std::string_view key) const {
  return *shards_[shard_for_key(key)];
}

bool StorageEngine::expired(const Entry& entry, TimePoint now) const {
  return entry.expiry.has_value() && *entry.expiry <= now;
}

bool StorageEngine::erase_if_expired_locked(Shard& selected, const std::string& key, TimePoint now) {
  auto it = selected.map.find(key);
  if (it == selected.map.end() || !expired(it->second, now)) {
    return false;
  }
  erase_entry_locked(selected, it);
  expired_keys_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void StorageEngine::erase_entry_locked(Shard& selected,
                                       std::unordered_map<std::string, Entry>::iterator it) {
  const size_t memory = entry_memory(it->first, it->second.value);
  release_memory(memory);
  selected.memory_bytes -= memory;
  selected.map.erase(it);
}

size_t StorageEngine::entry_memory(std::string_view key, std::string_view value) const {
  return kEntryOverhead + key.size() + value.size();
}

bool StorageEngine::reserve_memory(size_t bytes) {
  if (bytes == 0 || config_.max_memory_bytes == 0) {
    return true;
  }
  size_t current = used_memory_.load(std::memory_order_relaxed);
  for (;;) {
    if (current > config_.max_memory_bytes || bytes > config_.max_memory_bytes - current) {
      return false;
    }
    if (used_memory_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
      return true;
    }
  }
}

void StorageEngine::release_memory(size_t bytes) {
  if (bytes == 0) {
    return;
  }
  size_t current = used_memory_.load(std::memory_order_relaxed);
  while (current != 0) {
    const size_t next = bytes > current ? 0 : current - bytes;
    if (used_memory_.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
      return;
    }
  }
}

bool StorageEngine::key_valid(std::string_view key) const {
  return key.size() <= limits_.max_key_size;
}

bool StorageEngine::value_valid(std::string_view value) const {
  return value.size() <= limits_.max_value_size;
}

std::string storage_write_error(StorageEngine::WriteCode code) {
  switch (code) {
  case StorageEngine::WriteCode::Ok:
    return "OK";
  case StorageEngine::WriteCode::NotSet:
    return "condition not met";
  case StorageEngine::WriteCode::NoMemory:
    return "OOM command not allowed when used_memory exceeds maxmemory";
  case StorageEngine::WriteCode::KeyTooLarge:
    return "key exceeds configured maximum size";
  case StorageEngine::WriteCode::ValueTooLarge:
    return "value exceeds configured maximum size";
  case StorageEngine::WriteCode::InvalidInteger:
    return "value is not an integer or out of range";
  case StorageEngine::WriteCode::Overflow:
    return "increment or decrement would overflow";
  }
  return "write failed";
}

int64_t unix_ms(StorageEngine::TimePoint tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

StorageEngine::TimePoint timepoint_from_unix_ms(int64_t milliseconds) {
  return StorageEngine::TimePoint(std::chrono::milliseconds(milliseconds));
}

} // namespace veloxdb
