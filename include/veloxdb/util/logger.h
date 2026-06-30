#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace veloxdb {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

class Logger {
public:
  static Logger& instance();

  void set_level(LogLevel level);
  [[nodiscard]] LogLevel level() const;
  [[nodiscard]] bool enabled(LogLevel level) const;
  void log(LogLevel level, std::string_view component, std::string_view message);

private:
  Logger() = default;

  std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
  std::mutex write_mu_;
};

LogLevel parse_log_level(std::string_view value);
std::string_view log_level_name(LogLevel level);

void log_trace(std::string_view component, std::string_view message);
void log_debug(std::string_view component, std::string_view message);
void log_info(std::string_view component, std::string_view message);
void log_warn(std::string_view component, std::string_view message);
void log_error(std::string_view component, std::string_view message);

} // namespace veloxdb
