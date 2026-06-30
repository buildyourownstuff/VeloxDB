#pragma once

#include "veloxdb/config/config.h"
#include "veloxdb/util/status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veloxdb::resp {

enum class Type { SimpleString, Error, Integer, BulkString, Array };

struct Value {
  Type type{Type::BulkString};
  bool null{false};
  std::string text;
  int64_t integer{0};
  std::vector<Value> array;
};

struct Command {
  std::vector<std::string> args;
  std::string raw;
};

enum class ParseStatus { Ok, NeedMore, Error };

struct ValueParseResult {
  ParseStatus status{ParseStatus::NeedMore};
  Value value;
  std::string raw;
  std::string error;
};

struct CommandParseResult {
  ParseStatus status{ParseStatus::NeedMore};
  Command command;
  std::string error;
};

struct ParserOptions {
  size_t max_request_bytes{64ULL * 1024ULL * 1024ULL};
  size_t max_array_elements{1024};
  size_t max_bulk_bytes{64ULL * 1024ULL * 1024ULL};
};

ParserOptions parser_options_from_config(const ProtocolConfig& config);

class Parser {
public:
  explicit Parser(ParserOptions options);

  Status append(std::string_view bytes);
  [[nodiscard]] size_t buffered_bytes() const;
  void reset();

  ValueParseResult next_value();
  CommandParseResult next_command();

private:
  enum class InternalStatus { Ok, NeedMore, Error };

  struct InternalResult {
    InternalStatus status{InternalStatus::NeedMore};
    Value value;
    size_t consumed{0};
    std::string error;
  };

  InternalResult parse_at(size_t pos, size_t depth) const;
  InternalResult parse_inline(size_t pos) const;
  [[nodiscard]] std::optional<size_t> find_crlf(size_t pos) const;
  void discard(size_t bytes);

  ParserOptions options_;
  std::string buffer_;
};

std::string simple(std::string_view value);
std::string error(std::string_view value);
std::string integer(int64_t value);
std::string bulk(std::string_view value);
std::string null_bulk();
std::string array_header(size_t count);
std::string array_of_bulk(const std::vector<std::optional<std::string>>& values);
std::string encode_command(const std::vector<std::string_view>& args);
std::string encode_command(const std::vector<std::string>& args);

} // namespace veloxdb::resp
