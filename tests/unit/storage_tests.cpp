#include "../test_harness.h"

#include "veloxdb/storage/storage_engine.h"

#include <thread>

namespace {

veloxdb::StorageEngine make_storage(size_t shards = 8) {
  veloxdb::StorageConfig storage;
  storage.shards = shards;
  storage.max_memory_bytes = 0;
  veloxdb::ProtocolConfig limits;
  limits.max_key_size = 1024;
  limits.max_value_size = 1024 * 1024;
  return veloxdb::StorageEngine(storage, limits);
}

} // namespace

void register_storage_tests(TestSuite& suite) {
  suite.add("storage SET GET overwrite DEL", [] {
    auto storage = make_storage();
    require(storage.set("name", "dev", std::nullopt).code == veloxdb::StorageEngine::WriteCode::Ok,
            "set failed");
    require_eq(*storage.get("name"), std::string("dev"), "get failed");
    require(storage.set("name", "prod", std::nullopt).code == veloxdb::StorageEngine::WriteCode::Ok,
            "overwrite failed");
    require_eq(*storage.get("name"), std::string("prod"), "overwrite value");
    require_eq(storage.del("name"), size_t{1}, "delete count");
    require(!storage.get("name").has_value(), "deleted key should be missing");
  });

  suite.add("storage EXISTS and MGET", [] {
    auto storage = make_storage();
    (void)storage.set("a", "1", std::nullopt);
    (void)storage.set("b", "2", std::nullopt);
    std::vector<std::string_view> keys{"a", "missing", "b"};
    require_eq(storage.exists(keys), size_t{2}, "exists count");
    auto values = storage.mget(keys);
    require_eq(*values[0], std::string("1"), "mget a");
    require(!values[1].has_value(), "mget missing");
    require_eq(*values[2], std::string("2"), "mget b");
  });

  suite.add("storage INCR DECR and invalid integer", [] {
    auto storage = make_storage();
    require_eq(storage.incr_by("counter", 1).integer, int64_t{1}, "incr missing");
    require_eq(storage.incr_by("counter", -1).integer, int64_t{0}, "decr");
    (void)storage.set("bad", "abc", std::nullopt);
    require(storage.incr_by("bad", 1).code == veloxdb::StorageEngine::WriteCode::InvalidInteger,
            "invalid integer");
  });

  suite.add("storage TTL expiration and PERSIST", [] {
    auto storage = make_storage();
    (void)storage.set("ttl", "value", std::chrono::milliseconds(100));
    auto ttl = storage.ttl("ttl");
    require(ttl.state == veloxdb::StorageEngine::TtlState::HasExpiry, "ttl state");
    require(storage.persist("ttl"), "persist should remove expiry");
    require(storage.ttl("ttl").state == veloxdb::StorageEngine::TtlState::NoExpiry, "persisted ttl");
    require(storage.expire("ttl", std::chrono::milliseconds(10)), "expire should set");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(!storage.get("ttl").has_value(), "expired key should be gone");
  });

  suite.add("storage concurrent reads and writes", [] {
    auto storage = make_storage(16);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&storage, t] {
        for (int i = 0; i < 1000; ++i) {
          const std::string key = "k" + std::to_string((t * 1000) + i);
          (void)storage.set(key, std::to_string(i), std::nullopt);
          (void)storage.get(key);
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    require_eq(storage.dbsize(), size_t{4000}, "concurrent dbsize");
  });

  suite.add("storage shard routing is stable", [] {
    auto storage = make_storage(32);
    const size_t shard = storage.shard_for_key("stable-key");
    require(shard < storage.shard_count(), "shard range");
    require_eq(shard, storage.shard_for_key("stable-key"), "stable shard");
  });
}
