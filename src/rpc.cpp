#include "rpc.hpp"

#include <msgpack.hpp>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace RPCClient {
namespace {

constexpr int request_type = 0;
constexpr int response_type = 1;
constexpr int socket_timeout_seconds = 5;

std::string errno_message(std::string_view context) {
  return std::string(context) + ": " + std::strerror(errno);
}

void validate_socket_path(std::string_view socket_path) {
  sockaddr_un addr{};
  if (socket_path.empty()) {
    throw std::invalid_argument("socket path is empty");
  }
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    throw std::invalid_argument("socket path is too long: " +
                                std::string(socket_path));
  }
}

void set_socket_timeouts(int socket_fd) {
  timeval tv{};
  tv.tv_sec = socket_timeout_seconds;

  if (::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    throw std::runtime_error(errno_message("setsockopt SO_RCVTIMEO failed"));
  }
  if (::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
    throw std::runtime_error(errno_message("setsockopt SO_SNDTIMEO failed"));
  }

#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (::setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) < 0) {
    throw std::runtime_error(errno_message("setsockopt SO_NOSIGPIPE failed"));
  }
#endif
}

void close_fd(int &socket_fd) {
  if (socket_fd >= 0) {
    ::close(socket_fd);
    socket_fd = -1;
  }
}

template <typename Packer> void pack_value(Packer &pk, const Value &value);

template <typename Packer>
void pack_array(Packer &pk, const Value::array_type &values) {
  pk.pack_array(static_cast<uint32_t>(values.size()));
  for (const auto &value : values) {
    pack_value(pk, value);
  }
}

template <typename Packer> void pack_value(Packer &pk, const Value &value) {
  std::visit(
      [&pk](const auto &stored) {
        using Stored = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<Stored, std::monostate>) {
          pk.pack_nil();
        } else if constexpr (std::is_same_v<Stored, Value::array_type>) {
          pack_array(pk, stored);
        } else {
          pk.pack(stored);
        }
      },
      value.storage());
}

std::string object_to_debug_string(const msgpack::object &obj) {
  if (obj.type == msgpack::type::STR) {
    return obj.as<std::string>();
  }

  std::ostringstream out;
  out << obj;
  return out.str();
}

Value object_to_value(const msgpack::object &obj) {
  switch (obj.type) {
  case msgpack::type::NIL:
    return nullptr;
  case msgpack::type::BOOLEAN:
    return obj.as<bool>();
  case msgpack::type::POSITIVE_INTEGER:
    if (obj.via.u64 >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::runtime_error("RPC integer result is out of int64_t range");
    }
    return static_cast<int64_t>(obj.via.u64);
  case msgpack::type::NEGATIVE_INTEGER:
    return obj.as<int64_t>();
  case msgpack::type::FLOAT32:
  case msgpack::type::FLOAT64:
    return obj.as<double>();
  case msgpack::type::STR:
    return obj.as<std::string>();
  case msgpack::type::ARRAY: {
    Value::array_type values;
    values.reserve(obj.via.array.size);
    for (uint32_t i = 0; i < obj.via.array.size; ++i) {
      values.emplace_back(object_to_value(obj.via.array.ptr[i]));
    }
    return values;
  }
  default:
    throw std::runtime_error("Unsupported RPC result type: " +
                             object_to_debug_string(obj));
  }
}

Value json_to_value(const nlohmann::json &value) {
  if (value.is_null()) {
    return nullptr;
  }
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  if (value.is_number_unsigned()) {
    const uint64_t unsigned_value = value.get<uint64_t>();
    if (unsigned_value >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::invalid_argument(
          "$.parameters contains unsigned integer out of int64_t range");
    }
    return static_cast<int64_t>(unsigned_value);
  }
  if (value.is_number_float()) {
    return value.get<double>();
  }
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_array()) {
    Value::array_type converted;
    converted.reserve(value.size());
    for (const auto &entry : value) {
      converted.emplace_back(json_to_value(entry));
    }
    return converted;
  }

  throw std::invalid_argument(
      "$.parameters contains unsupported value type (only null, bool, "
      "number, string, and arrays are allowed)");
}

std::pair<std::string, std::vector<Value>> parse_json_call(
    const nlohmann::json &call_payload) {
  if (!call_payload.is_object()) {
    throw std::invalid_argument("RPC JSON call must be an object");
  }
  if (!call_payload.contains("function")) {
    throw std::invalid_argument("RPC JSON call is missing $.function");
  }
  if (!call_payload["function"].is_string()) {
    throw std::invalid_argument("$.function must be a string");
  }
  if (!call_payload.contains("parameters")) {
    throw std::invalid_argument("RPC JSON call is missing $.parameters");
  }
  if (!call_payload["parameters"].is_array()) {
    throw std::invalid_argument("$.parameters must be an array");
  }

  std::vector<Value> args;
  const auto &parameters = call_payload["parameters"];
  args.reserve(parameters.size());
  for (const auto &parameter : parameters) {
    args.emplace_back(json_to_value(parameter));
  }

  return {call_payload["function"].get<std::string>(), std::move(args)};
}

void send_all(int socket_fd, const char *data, size_t size) {
  size_t bytes_sent = 0;
  while (bytes_sent < size) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t n = ::send(socket_fd, data + bytes_sent, size - bytes_sent, flags);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        throw std::runtime_error("Timed out while sending RPC request");
      }
      throw std::runtime_error(errno_message("Error sending RPC request"));
    }
    if (n == 0) {
      throw std::runtime_error("Socket closed while sending RPC request");
    }
    bytes_sent += static_cast<size_t>(n);
  }
}

msgpack::object_handle receive_message(int socket_fd) {
  msgpack::unpacker unp;

  while (true) {
    unp.reserve_buffer(4096);
    ssize_t n = ::recv(socket_fd, unp.buffer(), unp.buffer_capacity(), 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        throw std::runtime_error("Timed out waiting for RPC response");
      }
      throw std::runtime_error(errno_message("Error receiving RPC response"));
    }
    if (n == 0) {
      throw std::runtime_error("Connection closed before RPC response");
    }

    unp.buffer_consumed(static_cast<size_t>(n));

    msgpack::object_handle oh;
    if (unp.next(oh)) {
      return oh;
    }
  }
}

std::string value_to_string(const Value &value);

std::string array_to_string(const Value::array_type &values) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << value_to_string(values[i]);
  }
  out << "]";
  return out.str();
}

std::string value_to_string(const Value &value) {
  return std::visit(
      [](const auto &stored) -> std::string {
        using Stored = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<Stored, std::monostate>) {
          return "nil";
        } else if constexpr (std::is_same_v<Stored, bool>) {
          return stored ? "true" : "false";
        } else if constexpr (std::is_same_v<Stored, std::string>) {
          return stored;
        } else if constexpr (std::is_same_v<Stored, Value::array_type>) {
          return array_to_string(stored);
        } else {
          std::ostringstream out;
          out << stored;
          return out.str();
        }
      },
      value.storage());
}

} // namespace

Value::Value(const char *value) {
  if (value == nullptr) {
    _value = std::monostate{};
  } else {
    _value = std::string(value);
  }
}

std::ostream &operator<<(std::ostream &out, const Value &value) {
  out << value_to_string(value);
  return out;
}

std::string to_string(const Value &value) { return value_to_string(value); }

Client::Client(std::string_view socket_path) { connect(socket_path); }

Client::~Client() { close(); }

Client::Client(Client &&other) noexcept
    : _socket_fd(std::exchange(other._socket_fd, -1)),
      _next_msgid(other._next_msgid) {}

Client &Client::operator=(Client &&other) noexcept {
  if (this != &other) {
    close();
    _socket_fd = std::exchange(other._socket_fd, -1);
    _next_msgid = other._next_msgid;
  }
  return *this;
}

void Client::close() { close_fd(_socket_fd); }

void Client::connect(std::string_view socket_path) {
  validate_socket_path(socket_path);
  close();

  _socket_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (_socket_fd < 0) {
    throw std::runtime_error(errno_message("Error creating socket"));
  }

  try {
    set_socket_timeouts(_socket_fd);

    sockaddr_un server_addr{};
    server_addr.sun_family = AF_UNIX;
    std::memcpy(server_addr.sun_path, socket_path.data(), socket_path.size());
    server_addr.sun_path[socket_path.size()] = '\0';

    if (::connect(_socket_fd, reinterpret_cast<sockaddr *>(&server_addr),
                  sizeof(server_addr)) < 0) {
      throw std::runtime_error(errno_message("Error connecting to socket"));
    }
  } catch (...) {
    close();
    throw;
  }
}

void Client::adopt_socket(int socket_fd) {
  if (socket_fd < 0) {
    throw std::invalid_argument("Invalid socket file descriptor");
  }

  close();
  _socket_fd = socket_fd;
  try {
    set_socket_timeouts(_socket_fd);
  } catch (...) {
    close();
    throw;
  }
}

Result Client::call(std::string_view method, const std::vector<Value> &args) {
  if (!is_open()) {
    throw std::runtime_error("Socket is not open");
  }
  if (method.empty()) {
    throw std::invalid_argument("RPC method name is empty");
  }

  const uint32_t request_msgid = _next_msgid++;

  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> pk(buffer);
  pk.pack_array(4);
  pk.pack(request_type);
  pk.pack(request_msgid);
  pk.pack(std::string(method));
  pack_array(pk, args);

  send_all(_socket_fd, buffer.data(), buffer.size());

  msgpack::object_handle response_handle = receive_message(_socket_fd);
  msgpack::object response = response_handle.get();
  if (response.type != msgpack::type::ARRAY || response.via.array.size != 4) {
    throw std::runtime_error("Invalid RPC response format");
  }

  const int actual_response_type = response.via.array.ptr[0].as<int>();
  const uint32_t response_msgid = response.via.array.ptr[1].as<uint32_t>();
  if (actual_response_type != response_type ||
      response_msgid != request_msgid) {
    throw std::runtime_error("RPC response does not match request");
  }

  const msgpack::object &error = response.via.array.ptr[2];
  if (error.type != msgpack::type::NIL) {
    throw std::runtime_error("RPC error: " + object_to_debug_string(error));
  }

  return object_to_value(response.via.array.ptr[3]);
}

Result Client::call(std::string_view method,
                    std::initializer_list<Value> args) {
  return call(method, std::vector<Value>(args));
}

Result Client::call(nlohmann::json call_payload) {
  auto [method, args] = parse_json_call(call_payload);
  return call(method, args);
}

} // namespace RPCClient
