#include "veloxdb/util/logger.h"

#include "veloxdb/util/string.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace veloxdb {

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::set_level(LogLevel level) { level_.store(static_cast<int>(level), std::memory_order_relaxed); }

LogLevel Logger::level() const {
  return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
}

bool Logger::enabled(LogLevel level) const {
  return static_cast<int>(level) >= level_.load(std::memory_order_relaxed) && level != LogLevel::Off;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
  if (!enabled(level)) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch() % std::chrono::seconds(1));

  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif

  std::ostringstream line;
  line << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
       << std::setfill('0') << millis.count() << "Z";
  line << " level=" << log_level_name(level);
  line << " component=" << component;
  line << " msg=\"" << message << '"';

  std::lock_guard<std::mutex> lock(write_mu_);
  std::cerr << line.str() << '\n';
}

LogLevel parse_log_level(std::string_view value) {
  const std::string lower = util::to_lower_ascii(value);
  if (lower == "trace") {
    return LogLevel::Trace;
  }
  if (lower == "debug") {
    return LogLevel::Debug;
  }
  if (lower == "warn" || lower == "warning") {
    return LogLevel::Warn;
  }
  if (lower == "error") {
    return LogLevel::Error;
  }
  if (lower == "off") {
    return LogLevel::Off;
  }
  return LogLevel::Info;
}

std::string_view log_level_name(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return "trace";
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  case LogLevel::Off:
    return "off";
  }
  return "info";
}

void log_trace(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::Trace, component, message);
}

void log_debug(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::Debug, component, message);
}

void log_info(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::Info, component, message);
}

void log_warn(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::Warn, component, message);
}

void log_error(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::Error, component, message);
}

} // namespace veloxdb
