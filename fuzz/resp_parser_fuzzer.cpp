#include "veloxdb/protocol/resp.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

constexpr size_t kMaxDrainIterations = 128;
constexpr size_t kMaxChunkBytes = 32;

veloxdb::resp::ParserOptions fuzz_options() {
  return veloxdb::resp::ParserOptions{
      4096,
      64,
      2048,
  };
}

std::string_view bytes_view(const uint8_t* data, size_t size) {
  return std::string_view(reinterpret_cast<const char*>(data), size);
}

void drain_values(veloxdb::resp::Parser& parser) {
  for (size_t i = 0; i < kMaxDrainIterations; ++i) {
    veloxdb::resp::ValueParseResult result = parser.next_value();
    if (result.status == veloxdb::resp::ParseStatus::NeedMore) {
      return;
    }
    if (result.status == veloxdb::resp::ParseStatus::Error) {
      parser.reset();
      return;
    }
  }
  parser.reset();
}

void drain_commands(veloxdb::resp::Parser& parser) {
  for (size_t i = 0; i < kMaxDrainIterations; ++i) {
    veloxdb::resp::CommandParseResult result = parser.next_command();
    if (result.status == veloxdb::resp::ParseStatus::NeedMore) {
      return;
    }
    if (result.status == veloxdb::resp::ParseStatus::Error) {
      parser.reset();
      return;
    }

    (void)veloxdb::resp::encode_command(result.command.args);
  }
  parser.reset();
}

template <typename Drain>
void feed_incrementally(const uint8_t* data, size_t size, Drain drain) {
  veloxdb::resp::Parser parser(fuzz_options());
  size_t offset = 0;
  while (offset < size) {
    const size_t wanted = 1 + static_cast<size_t>(data[offset] % kMaxChunkBytes);
    const size_t chunk = std::min(wanted, size - offset);
    veloxdb::Status status = parser.append(bytes_view(data + offset, chunk));
    if (!status) {
      parser.reset();
      return;
    }
    drain(parser);
    offset += chunk;
  }
  drain(parser);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  feed_incrementally(data, size, drain_values);
  feed_incrementally(data, size, drain_commands);
  return 0;
}
