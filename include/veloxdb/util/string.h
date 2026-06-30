#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veloxdb::util {

std::string trim(std::string_view input);
std::string to_upper_ascii(std::string_view input);
std::string to_lower_ascii(std::string_view input);
bool iequals(std::string_view lhs, std::string_view rhs);
std::vector<std::string_view> split_words(std::string_view line);
std::optional<int64_t> parse_i64(std::string_view input);
std::optional<uint64_t> parse_u64(std::string_view input);
std::optional<bool> parse_bool(std::string_view input);
std::optional<size_t> parse_byte_size(std::string_view input);
std::string shell_quote(std::string_view input);

} // namespace veloxdb::util
