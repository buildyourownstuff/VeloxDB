#pragma once

#include <string>
#include <utility>

namespace veloxdb {

class Status {
public:
  enum class Code {
    Ok,
    InvalidArgument,
    NotFound,
    ResourceExhausted,
    IOError,
    ProtocolError,
    InternalError
  };

  Status() = default;

  static Status ok() { return Status(); }
  static Status invalid_argument(std::string message) {
    return Status(Code::InvalidArgument, std::move(message));
  }
  static Status not_found(std::string message) { return Status(Code::NotFound, std::move(message)); }
  static Status resource_exhausted(std::string message) {
    return Status(Code::ResourceExhausted, std::move(message));
  }
  static Status io_error(std::string message) { return Status(Code::IOError, std::move(message)); }
  static Status protocol_error(std::string message) {
    return Status(Code::ProtocolError, std::move(message));
  }
  static Status internal_error(std::string message) {
    return Status(Code::InternalError, std::move(message));
  }

  [[nodiscard]] bool ok_status() const { return code_ == Code::Ok; }
  [[nodiscard]] explicit operator bool() const { return ok_status(); }
  [[nodiscard]] Code code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return message_; }
  [[nodiscard]] std::string to_string() const;

private:
  Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

  Code code_{Code::Ok};
  std::string message_;
};

} // namespace veloxdb
