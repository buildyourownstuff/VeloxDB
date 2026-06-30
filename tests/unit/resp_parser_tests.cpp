#include "../test_harness.h"

#include "veloxdb/protocol/resp.h"

using veloxdb::resp::ParseStatus;

void register_resp_parser_tests(TestSuite& suite) {
  suite.add("RESP parses simple array command", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("*1\r\n$4\r\nPING\r\n"), "append failed");
    auto result = parser.next_command();
    require_eq(result.status, ParseStatus::Ok, "expected command");
    require_eq(result.command.args.size(), size_t{1}, "arg count");
    require_eq(result.command.args[0], std::string("PING"), "arg value");
  });

  suite.add("RESP handles partial input", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("*2\r\n$4\r\nPING\r\n"), "append failed");
    require_eq(parser.next_command().status, ParseStatus::NeedMore, "expected partial");
    require(parser.append("$5\r\nhello\r\n"), "append failed");
    auto result = parser.next_command();
    require_eq(result.status, ParseStatus::Ok, "expected complete command");
    require_eq(result.command.args[1], std::string("hello"), "message");
  });

  suite.add("RESP handles multiple commands in one packet", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n"), "append failed");
    require_eq(parser.next_command().command.args[0], std::string("PING"), "first");
    auto second = parser.next_command();
    require_eq(second.status, ParseStatus::Ok, "second status");
    require_eq(second.command.args[0], std::string("ECHO"), "second command");
    require_eq(second.command.args[1], std::string("hi"), "second arg");
  });

  suite.add("RESP rejects invalid bulk length", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("$-2\r\n"), "append failed");
    require_eq(parser.next_value().status, ParseStatus::Error, "expected error");
  });

  suite.add("RESP rejects invalid array length", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("*-2\r\n"), "append failed");
    require_eq(parser.next_value().status, ParseStatus::Error, "expected error");
  });

  suite.add("RESP rejects oversized bulk string", [] {
    veloxdb::resp::Parser parser({1024, 16, 4});
    require(parser.append("$5\r\nhello\r\n"), "append failed");
    require_eq(parser.next_value().status, ParseStatus::Error, "expected oversized error");
  });

  suite.add("RESP supports empty array command", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("*0\r\n"), "append failed");
    auto result = parser.next_command();
    require_eq(result.status, ParseStatus::Ok, "expected empty command");
    require(result.command.args.empty(), "expected empty args");
  });

  suite.add("RESP supports inline command input", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("PING hello\r\n"), "append failed");
    auto result = parser.next_command();
    require_eq(result.status, ParseStatus::Ok, "inline status");
    require_eq(result.command.args[0], std::string("PING"), "inline command");
    require_eq(result.command.args[1], std::string("hello"), "inline arg");
  });

  suite.add("RESP rejects malformed bulk CRLF", [] {
    veloxdb::resp::Parser parser({1024, 16, 1024});
    require(parser.append("$3\r\nabc\rx"), "append failed");
    require_eq(parser.next_value().status, ParseStatus::Error, "expected malformed CRLF");
  });
}
