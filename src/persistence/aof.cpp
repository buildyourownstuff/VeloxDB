#include "veloxdb/persistence/aof.h"

#include "veloxdb/util/logger.h"
#include "veloxdb/util/string.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace veloxdb {
namespace {

bool path_is_safe(std::string_view path) {
  return !path.empty() && path.find('\0') == std::string_view::npos;
}

Status ensure_parent_dir(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return Status::io_error("failed to create directory " + parent.string() + ": " + ec.message());
    }
  }
  return Status::ok();
}

Status write_all(int fd, std::string_view bytes) {
  const char* data = bytes.data();
  size_t remaining = bytes.size();
  while (remaining != 0) {
    const ssize_t written = ::write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::io_error(std::strerror(errno));
    }
    if (written == 0) {
      return Status::io_error("write returned zero bytes");
    }
    data += written;
    remaining -= static_cast<size_t>(written);
  }
  return Status::ok();
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

Aof::Aof(PersistenceConfig config, ProtocolConfig protocol)
    : config_(std::move(config)), protocol_(protocol), policy_(parse_policy(config_.fsync_policy)) {}

Aof::~Aof() { close(); }

Status Aof::open() {
  if (!config_.aof_enabled) {
    return Status::ok();
  }
  if (!path_is_safe(config_.aof_path)) {
    return Status::invalid_argument("invalid AOF path");
  }
  Status status = ensure_parent_dir(config_.aof_path);
  if (!status) {
    return status;
  }

#ifdef O_CLOEXEC
  const int flags = O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC;
#else
  const int flags = O_CREAT | O_APPEND | O_WRONLY;
#endif
  fd_ = ::open(config_.aof_path.c_str(), flags, 0644);
  if (fd_ < 0) {
    return Status::io_error("failed to open AOF " + config_.aof_path + ": " + std::strerror(errno));
  }

  std::error_code ec;
  current_size_.store(static_cast<size_t>(std::filesystem::file_size(config_.aof_path, ec)),
                      std::memory_order_relaxed);
  if (ec) {
    current_size_.store(0, std::memory_order_relaxed);
  }

  stop_.store(false, std::memory_order_relaxed);
  if (policy_ == FsyncPolicy::EverySec) {
    sync_thread_ = std::thread([this] { sync_loop(); });
  }
  return Status::ok();
}

void Aof::close() {
  stop_.store(true, std::memory_order_relaxed);
  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ >= 0) {
    (void)::fsync(fd_);
    (void)::close(fd_);
    fd_ = -1;
  }
}

Status Aof::append(const std::vector<std::string_view>& args) {
  if (!enabled()) {
    return Status::ok();
  }
  const std::string encoded = resp::encode_command(args);
  std::lock_guard<std::mutex> lock(mu_);
  Status status = write_bytes_locked(encoded);
  if (status && policy_ == FsyncPolicy::Always) {
    if (::fsync(fd_) != 0) {
      status = Status::io_error("fsync failed: " + std::string(std::strerror(errno)));
    }
  }
  if (!status) {
    last_write_ok_.store(false, std::memory_order_relaxed);
    last_error_ = status.message();
    log_error("aof", status.to_string());
    return status;
  }
  last_write_ok_.store(true, std::memory_order_relaxed);
  return Status::ok();
}

Status Aof::append(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.emplace_back(arg);
  }
  return append(views);
}

Status Aof::sync() {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) {
    return Status::ok();
  }
  if (::fsync(fd_) != 0) {
    return Status::io_error("fsync failed: " + std::string(std::strerror(errno)));
  }
  return Status::ok();
}

bool Aof::enabled() const { return config_.aof_enabled && fd_ >= 0; }

size_t Aof::current_size() const { return current_size_.load(std::memory_order_relaxed); }

bool Aof::last_write_ok() const { return last_write_ok_.load(std::memory_order_relaxed); }

std::string Aof::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

Aof::ReplayStats Aof::replay_file(const std::string& path, const ProtocolConfig& protocol,
                                  const ReplayCallback& callback) {
  ReplayStats stats;
  if (!path_is_safe(path) || !std::filesystem::exists(path)) {
    return stats;
  }

  const std::string content = read_file(path);
  if (content.empty()) {
    return stats;
  }

  resp::Parser parser(resp::parser_options_from_config(protocol));
  Status status = parser.append(content);
  if (!status) {
    stats.status = status;
    return stats;
  }

  for (;;) {
    resp::CommandParseResult parsed = parser.next_command();
    if (parsed.status == resp::ParseStatus::NeedMore) {
      stats.ignored_partial_tail = parser.buffered_bytes() != 0;
      return stats;
    }
    if (parsed.status == resp::ParseStatus::Error) {
      stats.status = Status::protocol_error("AOF replay failed: " + parsed.error);
      return stats;
    }
    status = callback(parsed.command.args);
    if (!status) {
      stats.status = status;
      return stats;
    }
    ++stats.commands;
  }
}

Aof::FsyncPolicy Aof::parse_policy(std::string_view policy) {
  const std::string lower = util::to_lower_ascii(policy);
  if (lower == "always") {
    return FsyncPolicy::Always;
  }
  if (lower == "no") {
    return FsyncPolicy::No;
  }
  return FsyncPolicy::EverySec;
}

Status Aof::write_bytes_locked(std::string_view bytes) {
  if (fd_ < 0) {
    return Status::io_error("AOF is not open");
  }
  Status status = write_all(fd_, bytes);
  if (status) {
    current_size_.fetch_add(bytes.size(), std::memory_order_relaxed);
  }
  return status;
}

void Aof::sync_loop() {
  while (!stop_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const Status status = sync();
    if (!status) {
      last_write_ok_.store(false, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = status.message();
    }
  }
}

} // namespace veloxdb
