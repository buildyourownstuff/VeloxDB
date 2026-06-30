#include "veloxdb/net/tcp_server.h"

#include "veloxdb/protocol/resp.h"
#include "veloxdb/util/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/epoll.h>
#endif
#include <unistd.h>

#include <array>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace veloxdb {
namespace {

Status set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return Status::io_error("fcntl(F_GETFL): " + std::string(std::strerror(errno)));
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return Status::io_error("fcntl(F_SETFL): " + std::string(std::strerror(errno)));
  }
  return Status::ok();
}

void close_fd(int& fd) {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

std::string peer_string(const sockaddr_in& addr) {
  std::array<char, INET_ADDRSTRLEN> host{};
  const char* ok = ::inet_ntop(AF_INET, &addr.sin_addr, host.data(), host.size());
  if (ok == nullptr) {
    return "unknown";
  }
  return std::string(host.data()) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace

class TcpServer::Worker {
public:
  Worker(size_t id, Config& config, StorageEngine& storage, Metrics& metrics,
         const CommandRegistry& registry, Aof* aof, SnapshotStore* snapshot,
         std::function<void()> request_shutdown)
      : id_(id), config_(config), storage_(storage), metrics_(metrics), registry_(registry),
        aof_(aof), snapshot_(snapshot), request_shutdown_(std::move(request_shutdown)) {}

  ~Worker() { stop(); }

  Status start() {
    if (::pipe(wakeup_pipe_) != 0) {
      return Status::io_error("pipe: " + std::string(std::strerror(errno)));
    }
    Status status = set_nonblocking(wakeup_pipe_[0]);
    if (!status) {
      close_fd(wakeup_pipe_[0]);
      close_fd(wakeup_pipe_[1]);
      return status;
    }
    status = set_nonblocking(wakeup_pipe_[1]);
    if (!status) {
      close_fd(wakeup_pipe_[0]);
      close_fd(wakeup_pipe_[1]);
      return status;
    }
#if defined(__linux__)
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
      close_fd(wakeup_pipe_[0]);
      close_fd(wakeup_pipe_[1]);
      return Status::io_error("epoll_create1: " + std::string(std::strerror(errno)));
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wakeup_pipe_[0];
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_pipe_[0], &event) != 0) {
      status = Status::io_error("epoll_ctl wakeup: " + std::string(std::strerror(errno)));
      close_fd(epoll_fd_);
      close_fd(wakeup_pipe_[0]);
      close_fd(wakeup_pipe_[1]);
      return status;
    }
#endif
    stopping_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { loop(); });
    return Status::ok();
  }

  void stop() {
    stopping_.store(true, std::memory_order_relaxed);
    wake();
    if (thread_.joinable()) {
      thread_.join();
    }
#if defined(__linux__)
    close_fd(epoll_fd_);
#endif
    close_fd(wakeup_pipe_[0]);
    close_fd(wakeup_pipe_[1]);
  }

  void add_connection(int fd, std::string peer) {
    {
      std::lock_guard<std::mutex> lock(pending_mu_);
      pending_.push_back(PendingConnection{fd, std::move(peer)});
    }
    wake();
  }

private:
  struct PendingConnection {
    int fd{-1};
    std::string peer;
  };

  struct Connection {
    int fd{-1};
    std::string peer;
    resp::Parser parser;
    std::string write_buffer;
    bool close_after_write{false};

    Connection(int conn_fd, std::string conn_peer, const ProtocolConfig& protocol)
        : fd(conn_fd), peer(std::move(conn_peer)),
          parser(resp::parser_options_from_config(protocol)) {}
  };

  void wake() {
    if (wakeup_pipe_[1] >= 0) {
      char byte = 0;
      (void)::write(wakeup_pipe_[1], &byte, 1);
    }
  }

  void drain_wakeup() {
    std::array<char, 64> bytes{};
    while (::read(wakeup_pipe_[0], bytes.data(), bytes.size()) > 0) {
    }
  }

  void install_pending() {
    std::deque<PendingConnection> local;
    {
      std::lock_guard<std::mutex> lock(pending_mu_);
      local.swap(pending_);
    }
    while (!local.empty()) {
      PendingConnection pending = std::move(local.front());
      local.pop_front();
      const int fd = pending.fd;
      auto [it, inserted] =
          connections_.try_emplace(fd, fd, std::move(pending.peer), config_.protocol);
      if (!inserted) {
        int close_me = fd;
        close_fd(close_me);
        continue;
      }
      if (!register_connection(fd)) {
        int close_me = it->second.fd;
        close_fd(close_me);
        connections_.erase(it);
        continue;
      }
      metrics_.connection_opened();
      log_debug("net", "client connected on worker " + std::to_string(id_));
    }
  }

  void loop() {
#if defined(__linux__)
    loop_epoll();
#else
    loop_poll();
#endif
  }

  void loop_poll() {
    while (!stopping_.load(std::memory_order_relaxed)) {
      std::vector<pollfd> fds;
      fds.reserve(connections_.size() + 1);
      fds.push_back(pollfd{wakeup_pipe_[0], POLLIN, 0});
      for (auto& [fd, conn] : connections_) {
        short events = POLLIN;
        if (!conn.write_buffer.empty()) {
          events = static_cast<short>(events | POLLOUT);
        }
        fds.push_back(pollfd{fd, events, 0});
      }

      const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        log_error("net", "poll failed: " + std::string(std::strerror(errno)));
        continue;
      }
      if (ready == 0) {
        continue;
      }

      if ((fds[0].revents & POLLIN) != 0) {
        drain_wakeup();
        install_pending();
      }

      std::vector<int> close_list;
      for (size_t i = 1; i < fds.size(); ++i) {
        const int fd = fds[i].fd;
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
          continue;
        }
        Connection& conn = it->second;
        const short revents = fds[i].revents;
        if ((revents & (POLLERR | POLLNVAL)) != 0) {
          close_list.push_back(fd);
          continue;
        }
        if ((revents & POLLIN) != 0 && !read_from(conn)) {
          close_list.push_back(fd);
          continue;
        }
        if ((revents & POLLOUT) != 0 && !write_to(conn)) {
          close_list.push_back(fd);
          continue;
        }
        if ((revents & POLLHUP) != 0 && conn.write_buffer.empty()) {
          close_list.push_back(fd);
        }
      }

      for (int fd : close_list) {
        close_connection(fd);
      }
    }

    close_all_connections();
  }

#if defined(__linux__)
  void loop_epoll() {
    constexpr int kMaxEvents = 256;
    std::array<epoll_event, kMaxEvents> events{};

    while (!stopping_.load(std::memory_order_relaxed)) {
      const int ready = ::epoll_wait(epoll_fd_, events.data(), kMaxEvents, 100);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        log_error("net", "epoll_wait failed: " + std::string(std::strerror(errno)));
        continue;
      }
      if (ready == 0) {
        continue;
      }

      std::vector<int> close_list;
      for (int i = 0; i < ready; ++i) {
        const int fd = events[static_cast<size_t>(i)].data.fd;
        const uint32_t event_mask = events[static_cast<size_t>(i)].events;

        if (fd == wakeup_pipe_[0]) {
          drain_wakeup();
          install_pending();
          continue;
        }

        auto it = connections_.find(fd);
        if (it == connections_.end()) {
          continue;
        }

        Connection& conn = it->second;
        bool should_close = false;
        if ((event_mask & EPOLLERR) != 0U) {
          should_close = true;
        }
        if (!should_close && (event_mask & EPOLLIN) != 0U && !read_from(conn)) {
          should_close = true;
        }
        if (!should_close && (event_mask & EPOLLOUT) != 0U && !write_to(conn)) {
          should_close = true;
        }
        if (!should_close && (event_mask & (EPOLLHUP | EPOLLRDHUP)) != 0U &&
            conn.write_buffer.empty()) {
          should_close = true;
        }

        if (should_close) {
          close_list.push_back(fd);
          continue;
        }
        update_connection_interest(conn);
      }

      for (int fd : close_list) {
        close_connection(fd);
      }
    }

    close_all_connections();
  }
#endif

  void close_all_connections() {
    for (auto& [fd, conn] : connections_) {
      (void)fd;
      int close_me = conn.fd;
      close_fd(close_me);
      metrics_.connection_closed();
    }
    connections_.clear();
  }

  bool register_connection(int fd) {
#if defined(__linux__)
    if (epoll_fd_ < 0) {
      return true;
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
      log_warn("net", "epoll_ctl add client failed: " + std::string(std::strerror(errno)));
      return false;
    }
#else
    (void)fd;
#endif
    return true;
  }

  void unregister_connection(int fd) {
#if defined(__linux__)
    if (epoll_fd_ >= 0) {
      (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }
#else
    (void)fd;
#endif
  }

  void update_connection_interest(Connection& conn) {
#if defined(__linux__)
    if (epoll_fd_ < 0) {
      return;
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    if (!conn.write_buffer.empty()) {
      event.events |= EPOLLOUT;
    }
    event.data.fd = conn.fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn.fd, &event) != 0) {
      log_warn("net", "epoll_ctl mod client failed: " + std::string(std::strerror(errno)));
    }
#else
    (void)conn;
#endif
  }

  bool read_from(Connection& conn) {
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
      const ssize_t n = ::read(conn.fd, buffer.data(), buffer.size());
      if (n > 0) {
        Status status = conn.parser.append(std::string_view(buffer.data(), static_cast<size_t>(n)));
        if (!status) {
          conn.write_buffer += resp::error("protocol error: " + status.message());
          conn.close_after_write = true;
          return true;
        }
        if (!drain_commands(conn)) {
          return true;
        }
        continue;
      }
      if (n == 0) {
        return false;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
  }

  bool drain_commands(Connection& conn) {
    for (;;) {
      resp::CommandParseResult parsed = conn.parser.next_command();
      if (parsed.status == resp::ParseStatus::NeedMore) {
        return true;
      }
      if (parsed.status == resp::ParseStatus::Error) {
        conn.write_buffer += resp::error("protocol error: " + parsed.error);
        conn.close_after_write = true;
        return false;
      }

      std::vector<std::string_view> views;
      views.reserve(parsed.command.args.size());
      for (const auto& arg : parsed.command.args) {
        views.emplace_back(arg);
      }

      CommandContext ctx{
          storage_,
          metrics_,
          config_,
          aof_,
          snapshot_,
          request_shutdown_,
          false,
      };
      metrics_.command_processed();
      CommandResult result = registry_.execute(ctx, views);
      conn.write_buffer += result.response;
      conn.close_after_write = conn.close_after_write || result.close_after_write;
      if (result.close_after_write) {
        return true;
      }
      if (conn.write_buffer.size() > config_.protocol.max_output_buffer_bytes) {
        conn.write_buffer = resp::error("output buffer limit exceeded");
        conn.close_after_write = true;
        return false;
      }
    }
  }

  bool write_to(Connection& conn) {
    while (!conn.write_buffer.empty()) {
      const ssize_t n = ::write(conn.fd, conn.write_buffer.data(), conn.write_buffer.size());
      if (n > 0) {
        conn.write_buffer.erase(0, static_cast<size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    return !conn.close_after_write;
  }

  void close_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
      return;
    }
    int close_me = it->second.fd;
    unregister_connection(fd);
    close_fd(close_me);
    connections_.erase(it);
    metrics_.connection_closed();
    log_debug("net", "client disconnected from worker " + std::to_string(id_));
  }

  size_t id_;
  Config& config_;
  StorageEngine& storage_;
  Metrics& metrics_;
  const CommandRegistry& registry_;
  Aof* aof_;
  SnapshotStore* snapshot_;
  std::function<void()> request_shutdown_;
  std::atomic_bool stopping_{false};
  int wakeup_pipe_[2]{-1, -1};
#if defined(__linux__)
  int epoll_fd_{-1};
#endif
  std::thread thread_;
  std::mutex pending_mu_;
  std::deque<PendingConnection> pending_;
  std::unordered_map<int, Connection> connections_;
};

TcpServer::TcpServer(Config& config, StorageEngine& storage, Metrics& metrics,
                     const CommandRegistry& registry, Aof* aof, SnapshotStore* snapshot,
                     std::function<void()> request_shutdown)
    : config_(config), storage_(storage), metrics_(metrics), registry_(registry), aof_(aof),
      snapshot_(snapshot), request_shutdown_(std::move(request_shutdown)) {}

TcpServer::~TcpServer() {
  request_stop();
  for (auto& worker : workers_) {
    worker->stop();
  }
  close_listener();
}

Status TcpServer::run(const std::atomic_bool* external_stop) {
  Status status = setup_listener();
  if (!status) {
    return status;
  }

  workers_.reserve(config_.server.workers);
  for (size_t i = 0; i < config_.server.workers; ++i) {
    auto worker = std::make_unique<Worker>(i, config_, storage_, metrics_, registry_, aof_, snapshot_,
                                           [this] {
                                             request_stop();
                                             if (request_shutdown_) {
                                               request_shutdown_();
                                             }
                                           });
    status = worker->start();
    if (!status) {
      request_stop();
      for (auto& started : workers_) {
        started->stop();
      }
      close_listener();
      return status;
    }
    workers_.push_back(std::move(worker));
  }

  log_info("server", "listening on " + config_.server.host + ":" + std::to_string(config_.server.port));

  size_t next_worker = 0;
  status = accept_loop(external_stop, next_worker);

  request_stop();
  for (auto& worker : workers_) {
    worker->stop();
  }
  workers_.clear();
  close_listener();
  log_info("server", "TCP server stopped");
  return status;
}

bool TcpServer::should_stop(const std::atomic_bool* external_stop) const {
  return stopping_.load(std::memory_order_relaxed) ||
         (external_stop != nullptr && external_stop->load(std::memory_order_relaxed));
}

Status TcpServer::accept_loop(const std::atomic_bool* external_stop, size_t& next_worker) {
#if defined(__linux__)
  int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    return Status::io_error("accept epoll_create1: " + std::string(std::strerror(errno)));
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = listen_fd_;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd_, &event) != 0) {
    Status status = Status::io_error("accept epoll_ctl: " + std::string(std::strerror(errno)));
    close_fd(epoll_fd);
    return status;
  }

  std::array<epoll_event, 8> events{};
  while (!should_stop(external_stop)) {
    const int ready = ::epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), 100);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      Status status =
          Status::io_error("accept epoll_wait failed: " + std::string(std::strerror(errno)));
      close_fd(epoll_fd);
      return status;
    }
    for (int i = 0; i < ready; ++i) {
      if ((events[static_cast<size_t>(i)].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0U) {
        accept_ready(next_worker);
      }
    }
  }

  close_fd(epoll_fd);
  return Status::ok();
#else
  while (!should_stop(external_stop)) {
    pollfd fd{listen_fd_, POLLIN, 0};
    const int ready = ::poll(&fd, 1, 100);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::io_error("accept poll failed: " + std::string(std::strerror(errno)));
    }
    if (ready > 0 && (fd.revents & POLLIN) != 0) {
      accept_ready(next_worker);
    }
  }
  return Status::ok();
#endif
}

void TcpServer::request_stop() {
  stopping_.store(true, std::memory_order_relaxed);
}

Status TcpServer::setup_listener() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return Status::io_error("socket: " + std::string(std::strerror(errno)));
  }

  int yes = 1;
  (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  Status status = set_nonblocking(listen_fd_);
  if (!status) {
    return status;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(config_.server.port);
  if (::inet_pton(AF_INET, config_.server.host.c_str(), &addr.sin_addr) != 1) {
    close_listener();
    return Status::invalid_argument("server.host must be an IPv4 address for the MVP");
  }

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    status = Status::io_error("bind: " + std::string(std::strerror(errno)));
    close_listener();
    return status;
  }
  if (::listen(listen_fd_, SOMAXCONN) != 0) {
    status = Status::io_error("listen: " + std::string(std::strerror(errno)));
    close_listener();
    return status;
  }
  return Status::ok();
}

void TcpServer::close_listener() { close_fd(listen_fd_); }

void TcpServer::accept_ready(size_t& next_worker) {
  for (;;) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
      }
      log_warn("net", "accept failed: " + std::string(std::strerror(errno)));
      return;
    }

    Status status = set_nonblocking(fd);
    if (!status) {
      int close_me = fd;
      close_fd(close_me);
      continue;
    }

    if (metrics_.connected_clients() >= config_.server.max_clients || workers_.empty()) {
      int close_me = fd;
      close_fd(close_me);
      continue;
    }

    workers_[next_worker % workers_.size()]->add_connection(fd, peer_string(peer));
    ++next_worker;
  }
}

} // namespace veloxdb
