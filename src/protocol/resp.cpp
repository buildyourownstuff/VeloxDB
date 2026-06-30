#include "veloxdb/protocol/resp.h"

#include "veloxdb/util/string.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace veloxdb::resp {
namespace {

constexpr size_t kMaxDepth = 8;

std::optional<int64_t> parse_number_line(std::string_view line) {
  if (line.empty()) {
    return std::nullopt;
  }
  int64_t value = 0;
  const auto* first = line.data();
  const auto* last = line.data() + line.size();
  const auto result = std::from_chars(first, last, value, 10);
  if (result.ec != std::errc() || result.ptr != last) {
    return std::nullopt;
  }
  return value;
}

bool is_resp_prefix(char ch) {
  return ch == '+' || ch == '-' || ch == ':' || ch == '$' || ch == '*';
}

std::string bulk_string_fragment(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 32);
  out.push_back('$');
  out += std::to_string(value.size());
  out += "\r\n";
  out.append(value);
  out += "\r\n";
  return out;
}

} // namespace

ParserOptions parser_options_from_config(const ProtocolConfig& config) {
  return ParserOptions{
      config.max_request_bytes,
      config.max_array_elements,
      config.max_value_size,
  };
}

Parser::Parser(ParserOptions options) : options_(options) {}

Status Parser::append(std::string_view bytes) {
  if (bytes.size() > options_.max_request_bytes ||
      buffer_.size() > options_.max_request_bytes - bytes.size()) {
    return Status::resource_exhausted("request buffer exceeded limit");
  }
  buffer_.append(bytes);
  return Status::ok();
}

size_t Parser::buffered_bytes() const { return buffer_.size(); }

void Parser::reset() { buffer_.clear(); }

ValueParseResult Parser::next_value() {
  ValueParseResult result;
  if (buffer_.empty()) {
    return result;
  }

  InternalResult parsed = is_resp_prefix(buffer_[0]) ? parse_at(0, 0) : parse_inline(0);
  if (parsed.status == InternalStatus::NeedMore) {
    return result;
  }
  if (parsed.status == InternalStatus::Error) {
    result.status = ParseStatus::Error;
    result.error = std::move(parsed.error);
    return result;
  }
  if (parsed.consumed > options_.max_request_bytes) {
    result.status = ParseStatus::Error;
    result.error = "request exceeded maximum size";
    return result;
  }
  result.status = ParseStatus::Ok;
  result.raw = buffer_.substr(0, parsed.consumed);
  result.value = std::move(parsed.value);
  discard(parsed.consumed);
  return result;
}

CommandParseResult Parser::next_command() {
  CommandParseResult result;
  ValueParseResult parsed = next_value();
  if (parsed.status == ParseStatus::NeedMore) {
    return result;
  }
  if (parsed.status == ParseStatus::Error) {
    result.status = ParseStatus::Error;
    result.error = std::move(parsed.error);
    return result;
  }

  if (parsed.value.type != Type::Array || parsed.value.null) {
    result.status = ParseStatus::Error;
    result.error = "expected RESP array command";
    return result;
  }

  result.command.raw = std::move(parsed.raw);
  result.command.args.reserve(parsed.value.array.size());
  for (const Value& item : parsed.value.array) {
    if (item.null) {
      result.status = ParseStatus::Error;
      result.error = "command arguments cannot be null";
      return result;
    }
    switch (item.type) {
    case Type::BulkString:
    case Type::SimpleString:
    case Type::Error:
      result.command.args.push_back(item.text);
      break;
    case Type::Integer:
      result.command.args.push_back(std::to_string(item.integer));
      break;
    case Type::Array:
      result.status = ParseStatus::Error;
      result.error = "nested arrays are not valid command arguments";
      return result;
    }
  }

  result.status = ParseStatus::Ok;
  return result;
}

Parser::InternalResult Parser::parse_at(size_t pos, size_t depth) const {
  if (depth > kMaxDepth) {
    return {InternalStatus::Error, {}, 0, "RESP nesting depth exceeded"};
  }
  if (pos >= buffer_.size()) {
    return {InternalStatus::NeedMore, {}, 0, {}};
  }

  const char prefix = buffer_[pos];
  const size_t line_start = pos + 1;
  const auto crlf = find_crlf(line_start);
  if (!crlf.has_value()) {
    if (buffer_.size() - pos > options_.max_request_bytes) {
      return {InternalStatus::Error, {}, 0, "line exceeded maximum request size"};
    }
    return {InternalStatus::NeedMore, {}, 0, {}};
  }

  const std::string_view line(buffer_.data() + line_start, *crlf - line_start);
  const size_t after_line = *crlf + 2;

  switch (prefix) {
  case '+': {
    Value value;
    value.type = Type::SimpleString;
    value.text = std::string(line);
    return {InternalStatus::Ok, std::move(value), after_line - pos, {}};
  }
  case '-': {
    Value value;
    value.type = Type::Error;
    value.text = std::string(line);
    return {InternalStatus::Ok, std::move(value), after_line - pos, {}};
  }
  case ':': {
    const auto number = parse_number_line(line);
    if (!number.has_value()) {
      return {InternalStatus::Error, {}, 0, "invalid integer"};
    }
    Value value;
    value.type = Type::Integer;
    value.integer = *number;
    return {InternalStatus::Ok, std::move(value), after_line - pos, {}};
  }
  case '$': {
    const auto length = parse_number_line(line);
    if (!length.has_value() || *length < -1) {
      return {InternalStatus::Error, {}, 0, "invalid bulk string length"};
    }
    Value value;
    value.type = Type::BulkString;
    if (*length == -1) {
      value.null = true;
      return {InternalStatus::Ok, std::move(value), after_line - pos, {}};
    }
    const uint64_t ulen = static_cast<uint64_t>(*length);
    if (ulen > options_.max_bulk_bytes ||
        ulen > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return {InternalStatus::Error, {}, 0, "bulk string exceeded maximum size"};
    }
    const size_t len = static_cast<size_t>(ulen);
    if (after_line > options_.max_request_bytes || len > options_.max_request_bytes - after_line) {
      return {InternalStatus::Error, {}, 0, "bulk string exceeded maximum request size"};
    }
    const size_t end = after_line + len;
    if (buffer_.size() < end + 2) {
      return {InternalStatus::NeedMore, {}, 0, {}};
    }
    if (buffer_[end] != '\r' || buffer_[end + 1] != '\n') {
      return {InternalStatus::Error, {}, 0, "malformed bulk string CRLF"};
    }
    value.text.assign(buffer_.data() + after_line, len);
    return {InternalStatus::Ok, std::move(value), end + 2 - pos, {}};
  }
  case '*': {
    const auto count = parse_number_line(line);
    if (!count.has_value() || *count < -1) {
      return {InternalStatus::Error, {}, 0, "invalid array length"};
    }
    Value value;
    value.type = Type::Array;
    if (*count == -1) {
      value.null = true;
      return {InternalStatus::Ok, std::move(value), after_line - pos, {}};
    }
    const uint64_t ucount = static_cast<uint64_t>(*count);
    if (ucount > options_.max_array_elements ||
        ucount > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return {InternalStatus::Error, {}, 0, "array exceeded maximum element count"};
    }
    value.array.reserve(static_cast<size_t>(ucount));
    size_t cursor = after_line;
    for (size_t i = 0; i < static_cast<size_t>(ucount); ++i) {
      InternalResult child = parse_at(cursor, depth + 1);
      if (child.status != InternalStatus::Ok) {
        return child;
      }
      cursor += child.consumed;
      if (cursor - pos > options_.max_request_bytes) {
        return {InternalStatus::Error, {}, 0, "request exceeded maximum size"};
      }
      value.array.push_back(std::move(child.value));
    }
    return {InternalStatus::Ok, std::move(value), cursor - pos, {}};
  }
  default:
    return {InternalStatus::Error, {}, 0, "unknown RESP type"};
  }
}

Parser::InternalResult Parser::parse_inline(size_t pos) const {
  const auto crlf = find_crlf(pos);
  if (!crlf.has_value()) {
    if (buffer_.size() - pos > options_.max_request_bytes) {
      return {InternalStatus::Error, {}, 0, "inline command exceeded maximum size"};
    }
    return {InternalStatus::NeedMore, {}, 0, {}};
  }

  const std::string_view line(buffer_.data() + pos, *crlf - pos);
  Value value;
  value.type = Type::Array;
  const std::vector<std::string_view> words = util::split_words(line);
  value.array.reserve(words.size());
  for (std::string_view word : words) {
    if (word.size() > options_.max_bulk_bytes) {
      return {InternalStatus::Error, {}, 0, "inline argument exceeded maximum size"};
    }
    Value item;
    item.type = Type::BulkString;
    item.text = std::string(word);
    value.array.push_back(std::move(item));
  }
  return {InternalStatus::Ok, std::move(value), *crlf + 2 - pos, {}};
}

std::optional<size_t> Parser::find_crlf(size_t pos) const {
  const size_t found = buffer_.find("\r\n", pos);
  if (found == std::string::npos) {
    return std::nullopt;
  }
  return found;
}

void Parser::discard(size_t bytes) {
  if (bytes >= buffer_.size()) {
    buffer_.clear();
    return;
  }
  buffer_.erase(0, bytes);
}

std::string simple(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 3);
  out.push_back('+');
  out.append(value);
  out += "\r\n";
  return out;
}

std::string error(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 7);
  out += "-ERR ";
  out.append(value);
  out += "\r\n";
  return out;
}

std::string integer(int64_t value) { return ":" + std::to_string(value) + "\r\n"; }

std::string bulk(std::string_view value) { return bulk_string_fragment(value); }

std::string null_bulk() { return "$-1\r\n"; }

std::string array_header(size_t count) { return "*" + std::to_string(count) + "\r\n"; }

std::string array_of_bulk(const std::vector<std::optional<std::string>>& values) {
  std::string out = array_header(values.size());
  for (const auto& value : values) {
    if (value.has_value()) {
      out += bulk(*value);
    } else {
      out += null_bulk();
    }
  }
  return out;
}

std::string encode_command(const std::vector<std::string_view>& args) {
  std::string out = array_header(args.size());
  for (std::string_view arg : args) {
    out += bulk_string_fragment(arg);
  }
  return out;
}

std::string encode_command(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const std::string& arg : args) {
    views.emplace_back(arg);
  }
  return encode_command(views);
}

} // namespace veloxdb::resp
