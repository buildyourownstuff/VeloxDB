#include "veloxdb/persistence/aof.h"

#include "veloxdb/storage/storage_engine.h"
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

Status sync_fd(int fd, std::string_view target) {
  if (::fsync(fd) != 0) {
    return Status::io_error("fsync failed for " + std::string(target) + ": " +
                            std::strerror(errno));
  }
  return Status::ok();
}

Status close_fd(int fd, std::string_view target) {
  if (::close(fd) != 0) {
    return Status::io_error("failed to close " + std::string(target) + ": " +
                            std::strerror(errno));
  }
  return Status::ok();
}

void sync_parent_dir_best_effort(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) {
    return;
  }
#ifdef O_DIRECTORY
  const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
#else
  const int dir_fd = ::open(parent.c_str(), O_RDONLY);
#endif
  if (dir_fd >= 0) {
    (void)::fsync(dir_fd);
    (void)::close(dir_fd);
  }
}

std::string rewrite_tmp_path(const std::string& path) {
  return path + ".rewrite." + std::to_string(::getpid()) + ".tmp";
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

Aof::MutationGuard::MutationGuard(std::shared_mutex& mu) : lock_(mu) {}

Aof::CheckpointGuard::CheckpointGuard(Aof& aof)
    : aof_(&aof), lock_(aof.rewrite_mu_), offset_(aof.current_size()) {}

size_t Aof::CheckpointGuard::offset() const { return offset_; }

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

  {
    std::lock_guard<std::mutex> lock(mu_);
    status = reopen_locked();
    if (!status) {
      return status;
    }
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
  return sync_fd(fd_, config_.aof_path);
}

Status Aof::rewrite(StorageEngine& storage) {
  if (!config_.aof_enabled) {
    return Status::invalid_argument("AOF is disabled");
  }
  if (!path_is_safe(config_.aof_path)) {
    return Status::invalid_argument("invalid AOF path");
  }

  std::unique_lock<std::shared_mutex> rewrite_lock(rewrite_mu_);

  Status status = ensure_parent_dir(config_.aof_path);
  if (!status) {
    last_write_ok_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = status.message();
    return status;
  }

  const std::string tmp_path = rewrite_tmp_path(config_.aof_path);
#ifdef O_CLOEXEC
  const int tmp_fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
#else
  const int tmp_fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
#endif
  if (tmp_fd < 0) {
    status = Status::io_error("failed to open AOF rewrite temp file: " + std::string(std::strerror(errno)));
    last_write_ok_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = status.message();
    return status;
  }

  const auto cleanup_tmp = [&] {
    (void)::close(tmp_fd);
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
  };

  for (const auto& entry : storage.snapshot()) {
    status = write_all(tmp_fd, resp::encode_command(std::vector<std::string>{"SET", entry.key, entry.value}));
    if (!status) {
      cleanup_tmp();
      last_write_ok_.store(false, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = status.message();
      return status;
    }
    if (entry.expire_at_unix_ms.has_value()) {
      status = write_all(tmp_fd, resp::encode_command(std::vector<std::string>{
                                     "PEXPIREAT", entry.key, std::to_string(*entry.expire_at_unix_ms)}));
      if (!status) {
        cleanup_tmp();
        last_write_ok_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = status.message();
        return status;
      }
    }
  }

  status = sync_fd(tmp_fd, tmp_path);
  const Status close_status = close_fd(tmp_fd, tmp_path);
  if (status && !close_status) {
    status = close_status;
  }
  if (!status) {
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    last_write_ok_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = status.message();
    return status;
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, config_.aof_path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    status = Status::io_error("failed to install rewritten AOF: " + ec.message());
    last_write_ok_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = status.message();
    return status;
  }
  sync_parent_dir_best_effort(config_.aof_path);
  if (path_is_safe(config_.manifest_path)) {
    std::error_code remove_ec;
    std::filesystem::remove(config_.manifest_path, remove_ec);
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    status = reopen_locked();
    if (!status) {
      last_error_ = status.message();
      last_write_ok_.store(false, std::memory_order_relaxed);
      return status;
    }
    last_error_.clear();
  }
  last_write_ok_.store(true, std::memory_order_relaxed);
  log_info("aof", "rewrite completed; size=" + std::to_string(current_size()));
  return Status::ok();
}

Aof::MutationGuard Aof::mutation_guard() { return MutationGuard(rewrite_mu_); }

Aof::CheckpointGuard Aof::checkpoint_guard() { return CheckpointGuard(*this); }

bool Aof::enabled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return config_.aof_enabled && fd_ >= 0;
}

size_t Aof::current_size() const { return current_size_.load(std::memory_order_relaxed); }

bool Aof::last_write_ok() const { return last_write_ok_.load(std::memory_order_relaxed); }

std::string Aof::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

const std::string& Aof::path() const { return config_.aof_path; }

Aof::ReplayStats Aof::replay_file(const std::string& path, const ProtocolConfig& protocol,
                                  const ReplayCallback& callback, size_t start_offset) {
  ReplayStats stats;
  if (!path_is_safe(path) || !std::filesystem::exists(path)) {
    return stats;
  }

  const std::string content = read_file(path);
  if (content.empty()) {
    return stats;
  }
  if (start_offset > content.size()) {
    stats.status = Status::protocol_error("AOF replay offset is beyond end of file");
    return stats;
  }
  if (start_offset == content.size()) {
    return stats;
  }

  resp::Parser parser(resp::parser_options_from_config(protocol));
  Status status = parser.append(std::string_view(content).substr(start_offset));
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

Status Aof::reopen_locked() {
#ifdef O_CLOEXEC
  const int flags = O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC;
#else
  const int flags = O_CREAT | O_APPEND | O_WRONLY;
#endif
  const int new_fd = ::open(config_.aof_path.c_str(), flags, 0644);
  if (new_fd < 0) {
    return Status::io_error("failed to open AOF " + config_.aof_path + ": " + std::strerror(errno));
  }
  const int old_fd = fd_;
  fd_ = new_fd;
  if (old_fd >= 0) {
    (void)::close(old_fd);
  }

  std::error_code ec;
  current_size_.store(static_cast<size_t>(std::filesystem::file_size(config_.aof_path, ec)),
                      std::memory_order_relaxed);
  if (ec) {
    current_size_.store(0, std::memory_order_relaxed);
  }
  return Status::ok();
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
