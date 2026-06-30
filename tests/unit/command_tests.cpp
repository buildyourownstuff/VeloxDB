#include "../test_harness.h"

#include "veloxdb/commands/command_registry.h"
#include "veloxdb/protocol/resp.h"

namespace {

struct Fixture {
  veloxdb::Config config;
  veloxdb::StorageEngine storage;
  veloxdb::Metrics metrics;
  veloxdb::CommandRegistry registry;
  veloxdb::SnapshotStore snapshot;

  Fixture()
      : storage(config.storage, config.protocol), snapshot(config.persistence.snapshot_path,
                                                           config.protocol) {
    veloxdb::register_default_commands(registry);
  }

  std::string exec(std::initializer_list<std::string_view> args) {
    std::vector<std::string_view> vec(args);
    veloxdb::CommandContext ctx{storage, metrics, config, nullptr, &snapshot, {}, false};
    return registry.execute(ctx, vec).response;
  }
};

} // namespace

void register_command_tests(TestSuite& suite) {
  suite.add("commands PING ECHO", [] {
    Fixture f;
    require_eq(f.exec({"PING"}), std::string("+PONG\r\n"), "PING");
    require_eq(f.exec({"PING", "hi"}), std::string("$2\r\nhi\r\n"), "PING message");
    require_eq(f.exec({"ECHO", "hi"}), std::string("$2\r\nhi\r\n"), "ECHO");
  });

  suite.add("commands SET GET are case-insensitive", [] {
    Fixture f;
    require_eq(f.exec({"set", "name", "dev"}), std::string("+OK\r\n"), "SET");
    require_eq(f.exec({"GET", "name"}), std::string("$3\r\ndev\r\n"), "GET");
  });

  suite.add("commands DEL EXISTS MGET MSET", [] {
    Fixture f;
    require_eq(f.exec({"MSET", "a", "1", "b", "2"}), std::string("+OK\r\n"), "MSET");
    require_eq(f.exec({"EXISTS", "a", "missing", "b"}), std::string(":2\r\n"), "EXISTS");
    require_eq(f.exec({"MGET", "a", "missing", "b"}),
               std::string("*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n"), "MGET");
    require_eq(f.exec({"DEL", "a", "b"}), std::string(":2\r\n"), "DEL");
  });

  suite.add("commands INCR DECR APPEND STRLEN", [] {
    Fixture f;
    require_eq(f.exec({"INCR", "counter"}), std::string(":1\r\n"), "INCR");
    require_eq(f.exec({"DECR", "counter"}), std::string(":0\r\n"), "DECR");
    require_eq(f.exec({"APPEND", "s", "abc"}), std::string(":3\r\n"), "APPEND");
    require_eq(f.exec({"STRLEN", "s"}), std::string(":3\r\n"), "STRLEN");
  });

  suite.add("commands expiration semantics", [] {
    Fixture f;
    require_eq(f.exec({"SET", "name", "dev"}), std::string("+OK\r\n"), "SET");
    require_eq(f.exec({"TTL", "name"}), std::string(":-1\r\n"), "TTL no expiry");
    require_eq(f.exec({"EXPIRE", "name", "5"}), std::string(":1\r\n"), "EXPIRE");
    const std::string ttl = f.exec({"TTL", "name"});
    require(ttl == ":5\r\n" || ttl == ":4\r\n", "TTL remaining");
    require_eq(f.exec({"PERSIST", "name"}), std::string(":1\r\n"), "PERSIST");
  });

  suite.add("commands unknown and wrong arity", [] {
    Fixture f;
    require(f.exec({"NOPE"}).rfind("-ERR unknown command", 0) == 0, "unknown command");
    require(f.exec({"GET"}).rfind("-ERR wrong number", 0) == 0, "wrong arity");
  });

  suite.add("commands INFO and COMMAND", [] {
    Fixture f;
    require(f.exec({"INFO"}).find("veloxdb_version") != std::string::npos, "INFO content");
    require(f.exec({"COMMAND"}).rfind("*", 0) == 0, "COMMAND array");
  });
}
