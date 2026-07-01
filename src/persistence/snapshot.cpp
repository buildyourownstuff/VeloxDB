#include "veloxdb/persistence/snapshot.h"

#include "veloxdb/persistence/aof.h"
#include "veloxdb/protocol/resp.h"
#include "veloxdb/util/string.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

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

std::string default_manifest_path(const std::string& snapshot_path) {
  return snapshot_path + ".manifest";
}

std::string normalize_path_string(const std::string& path) {
  return std::filesystem::path(path).lexically_normal().string();
}

std::string manifest_tmp_path(const std::string& path) {
  return path + ".tmp";
}

std::unordered_map<std::string, std::string> parse_manifest_lines(const std::string& content) {
  std::unordered_map<std::string, std::string> values;
  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    line = util::trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    values.emplace(util::trim(std::string_view(line).substr(0, eq)),
                   util::trim(std::string_view(line).substr(eq + 1)));
  }
  return values;
}

} // namespace

SnapshotStore::SnapshotStore(std::string path, ProtocolConfig protocol, std::string manifest_path)
    : path_(std::move(path)),
      manifest_path_(manifest_path.empty() ? default_manifest_path(path_) : std::move(manifest_path)),
      protocol_(protocol) {}

Status SnapshotStore::save(StorageEngine& storage, Aof* aof) const {
  if (aof != nullptr && aof->enabled()) {
    auto checkpoint = aof->checkpoint_guard();
    Status status = aof->sync();
    if (!status) {
      return status;
    }
    status = save_snapshot_file(storage);
    if (!status) {
      return status;
    }
    return write_manifest(Manifest{
        path_,
        aof->path(),
        checkpoint.offset(),
        unix_ms(StorageEngine::Clock::now()),
    });
  }

  const Status status = save_snapshot_file(storage);
  if (status) {
    remove_manifest();
  }
  return status;
}

Status SnapshotStore::save_snapshot_file(StorageEngine& storage) const {
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

Status SnapshotStore::write_manifest(const Manifest& manifest) const {
  if (!path_is_safe(manifest_path_)) {
    return Status::invalid_argument("invalid snapshot manifest path");
  }
  Status status = ensure_parent_dir(manifest_path_);
  if (!status) {
    return status;
  }

  const std::string tmp_path = manifest_tmp_path(manifest_path_);
  std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Status::io_error("failed to open snapshot manifest temp file");
  }
  out << "format_version=1\n";
  out << "snapshot_path=" << normalize_path_string(manifest.snapshot_path) << "\n";
  out << "aof_path=" << normalize_path_string(manifest.aof_path) << "\n";
  out << "aof_offset=" << manifest.aof_offset << "\n";
  out << "created_unix_ms=" << manifest.created_unix_ms << "\n";
  out.close();
  if (!out) {
    return Status::io_error("failed writing snapshot manifest");
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, manifest_path_, ec);
  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return Status::io_error("failed to install snapshot manifest: " + ec.message());
  }
  return Status::ok();
}

std::optional<SnapshotStore::Manifest> SnapshotStore::read_manifest() const {
  if (!path_is_safe(manifest_path_) || !std::filesystem::exists(manifest_path_)) {
    return std::nullopt;
  }
  std::ifstream in(manifest_path_, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  const std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto values = parse_manifest_lines(content);
  const auto version = values.find("format_version");
  const auto snapshot_path = values.find("snapshot_path");
  const auto aof_path = values.find("aof_path");
  const auto aof_offset = values.find("aof_offset");
  const auto created_unix_ms = values.find("created_unix_ms");
  if (version == values.end() || version->second != "1" || snapshot_path == values.end() ||
      aof_path == values.end() || aof_offset == values.end() || created_unix_ms == values.end()) {
    return std::nullopt;
  }
  const auto parsed_offset = util::parse_u64(aof_offset->second);
  const auto parsed_created = util::parse_i64(created_unix_ms->second);
  if (!parsed_offset.has_value() || !parsed_created.has_value()) {
    return std::nullopt;
  }
  return Manifest{
      snapshot_path->second,
      aof_path->second,
      static_cast<size_t>(*parsed_offset),
      *parsed_created,
  };
}

bool SnapshotStore::manifest_matches(const Manifest& manifest, const std::string& aof_path,
                                     size_t aof_size) const {
  if (manifest.snapshot_path != normalize_path_string(path_)) {
    return false;
  }
  if (manifest.aof_path != normalize_path_string(aof_path)) {
    return false;
  }
  if (manifest.aof_offset > aof_size) {
    return false;
  }
  return std::filesystem::exists(path_);
}

void SnapshotStore::remove_manifest() const {
  if (!path_is_safe(manifest_path_)) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(manifest_path_, ec);
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

SnapshotStore::RecoveryLoadResult
SnapshotStore::load_for_recovery(StorageEngine& storage, const std::string& aof_path,
                                 size_t aof_size) const {
  RecoveryLoadResult result;
  const auto manifest = read_manifest();
  if (!manifest.has_value()) {
    return result;
  }
  result.found_manifest = true;
  if (!manifest_matches(*manifest, aof_path, aof_size)) {
    return result;
  }

  result.status = load(storage);
  if (!result.status) {
    return result;
  }
  result.loaded_snapshot = true;
  result.aof_replay_offset = manifest->aof_offset;
  return result;
}

const std::string& SnapshotStore::path() const { return path_; }

const std::string& SnapshotStore::manifest_path() const { return manifest_path_; }

} // namespace veloxdb
