#include "veloxdb/persistence/snapshot.h"

#include "veloxdb/protocol/resp.h"
#include "veloxdb/util/string.h"

#include <filesystem>
#include <fstream>

namespace veloxdb {
namespace {

bool path_is_safe(std::string_view path) {
  return !path.empty() && path.find('\0') == std::string_view::npos;
}

Status ensure_parent_dir(const std::string& path) {
  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return Status::io_error("failed to create snapshot directory: " + ec.message());
    }
  }
  return Status::ok();
}

} // namespace

SnapshotStore::SnapshotStore(std::string path, ProtocolConfig protocol)
    : path_(std::move(path)), protocol_(protocol) {}

Status SnapshotStore::save(StorageEngine& storage) const {
  if (!path_is_safe(path_)) {
    return Status::invalid_argument("invalid snapshot path");
  }
  Status status = ensure_parent_dir(path_);
  if (!status) {
    return status;
  }

  const std::string tmp_path = path_ + ".tmp";
  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Status::io_error("failed to open snapshot temp file");
  }

  for (const auto& entry : storage.snapshot()) {
    out << resp::encode_command(std::vector<std::string>{"SET", entry.key, entry.value});
    if (entry.expire_at_unix_ms.has_value()) {
      out << resp::encode_command(
          std::vector<std::string>{"PEXPIREAT", entry.key, std::to_string(*entry.expire_at_unix_ms)});
    }
    if (!out) {
      return Status::io_error("failed writing snapshot");
    }
  }
  out.close();
  if (!out) {
    return Status::io_error("failed closing snapshot");
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, path_, ec);
  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return Status::io_error("failed to install snapshot: " + ec.message());
  }
  return Status::ok();
}

Status SnapshotStore::load(StorageEngine& storage) const {
  if (!path_is_safe(path_) || !std::filesystem::exists(path_)) {
    return Status::ok();
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return Status::io_error("failed to open snapshot");
  }
  const std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (content.empty()) {
    return Status::ok();
  }

  resp::Parser parser(resp::parser_options_from_config(protocol_));
  Status status = parser.append(content);
  if (!status) {
    return status;
  }
  for (;;) {
    resp::CommandParseResult parsed = parser.next_command();
    if (parsed.status == resp::ParseStatus::NeedMore) {
      return parser.buffered_bytes() == 0 ? Status::ok()
                                          : Status::protocol_error("partial snapshot command");
    }
    if (parsed.status == resp::ParseStatus::Error) {
      return Status::protocol_error(parsed.error);
    }

    const auto& args = parsed.command.args;
    if (args.size() == 3 && args[0] == "SET") {
      const auto write = storage.set(args[1], args[2], std::nullopt);
      if (write.code != StorageEngine::WriteCode::Ok) {
        return Status::internal_error(storage_write_error(write.code));
      }
    } else if (args.size() == 3 && args[0] == "PEXPIREAT") {
      const auto when = util::parse_i64(args[2]);
      if (!when.has_value()) {
        return Status::protocol_error("invalid PEXPIREAT in snapshot");
      }
      (void)storage.expire_at(args[1], timepoint_from_unix_ms(*when));
    } else {
      return Status::protocol_error("unknown snapshot command");
    }
  }
}

const std::string& SnapshotStore::path() const { return path_; }

} // namespace veloxdb
