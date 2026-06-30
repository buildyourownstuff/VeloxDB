#include "veloxdb/util/string.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>

namespace veloxdb::util {

std::string trim(std::string_view input) {
  auto begin = input.begin();
  auto end = input.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
    ++begin;
  }
  while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
    --end;
  }
  return std::string(begin, end);
}

std::string to_upper_ascii(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string to_lower_ascii(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

std::vector<std::string_view> split_words(std::string_view line) {
  std::vector<std::string_view> words;
  size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
      ++pos;
    }
    const size_t begin = pos;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) == 0) {
      ++pos;
    }
    if (begin != pos) {
      words.emplace_back(line.substr(begin, pos - begin));
    }
  }
  return words;
}

std::optional<int64_t> parse_i64(std::string_view input) {
  const std::string trimmed = trim(input);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int64_t value = 0;
  const auto* first = trimmed.data();
  const auto* last = trimmed.data() + trimmed.size();
  auto result = std::from_chars(first, last, value, 10);
  if (result.ec != std::errc() || result.ptr != last) {
    return std::nullopt;
  }
  return value;
}

std::optional<uint64_t> parse_u64(std::string_view input) {
  const std::string trimmed = trim(input);
  if (trimmed.empty() || trimmed.front() == '-') {
    return std::nullopt;
  }
  uint64_t value = 0;
  const auto* first = trimmed.data();
  const auto* last = trimmed.data() + trimmed.size();
  auto result = std::from_chars(first, last, value, 10);
  if (result.ec != std::errc() || result.ptr != last) {
    return std::nullopt;
  }
  return value;
}

std::optional<bool> parse_bool(std::string_view input) {
  const std::string value = to_lower_ascii(trim(input));
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return std::nullopt;
}

std::optional<size_t> parse_byte_size(std::string_view input) {
  std::string value = to_lower_ascii(trim(input));
  if (value.empty()) {
    return std::nullopt;
  }

  uint64_t multiplier = 1;
  auto strip_suffix = [&](std::string_view suffix, uint64_t factor) {
    if (value.size() >= suffix.size() &&
        std::string_view(value).substr(value.size() - suffix.size()) == suffix) {
      value.resize(value.size() - suffix.size());
      multiplier = factor;
      return true;
    }
    return false;
  };

  (void)(strip_suffix("kb", 1024ULL) || strip_suffix("k", 1024ULL) ||
         strip_suffix("mb", 1024ULL * 1024ULL) || strip_suffix("m", 1024ULL * 1024ULL) ||
         strip_suffix("gb", 1024ULL * 1024ULL * 1024ULL) ||
         strip_suffix("g", 1024ULL * 1024ULL * 1024ULL));

  const auto parsed = parse_u64(value);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  if (*parsed > std::numeric_limits<size_t>::max() / multiplier) {
    return std::nullopt;
  }
  return static_cast<size_t>(*parsed * multiplier);
}

std::string shell_quote(std::string_view input) {
  std::string out = "'";
  for (char ch : input) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

} // namespace veloxdb::util
