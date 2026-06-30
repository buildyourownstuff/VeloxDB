#include "veloxdb/util/status.h"

namespace veloxdb {

std::string Status::to_string() const {
  if (ok_status()) {
    return "OK";
  }
  const char* code = "unknown";
  switch (code_) {
  case Code::Ok:
    code = "ok";
    break;
  case Code::InvalidArgument:
    code = "invalid argument";
    break;
  case Code::NotFound:
    code = "not found";
    break;
  case Code::ResourceExhausted:
    code = "resource exhausted";
    break;
  case Code::IOError:
    code = "io error";
    break;
  case Code::ProtocolError:
    code = "protocol error";
    break;
  case Code::InternalError:
    code = "internal error";
    break;
  }
  return std::string(code) + ": " + message_;
}

} // namespace veloxdb
