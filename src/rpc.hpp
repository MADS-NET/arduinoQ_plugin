#pragma once

#define MSGPACK_USE_STD_VARIANT_ADAPTOR
#define MSGPACK_NO_BOOST
#include <msgpack.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/var/run/arduino-router.sock"

using msgpack_variant = std::variant<bool, int64_t, double, std::string>;

namespace RPCClient {
const std::string default_socket_path = SOCKET_PATH;

class FunctionCall {
public:
FunctionCall(const std::string &function_name) : _name(function_name) {}
~FunctionCall() {
  if (_socket_fd >= 0) {
    close(_socket_fd);
  }
};

FunctionCall(const FunctionCall&) = delete;
FunctionCall& operator=(const FunctionCall&) = delete;

void open_socket(std::string socket_path) {
  open_socket(socket_path.c_str());
}

void open_socket(const char *socket_path = SOCKET_PATH) {
  _socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (_socket_fd < 0) {
    throw std::runtime_error("Error creating socket: " + std::string(strerror(errno)));
  }
  struct timeval tv;
  tv.tv_sec = 5;  // 5 second timeout
  tv.tv_usec = 0;
  setsockopt(_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, socket_path, sizeof(server_addr.sun_path) - 1);

  if (connect(_socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    close(_socket_fd);
    _socket_fd = -1; // Indicate an error
    throw std::runtime_error("Error connecting to socket: " + std::string(strerror(errno)));
  }
}

void set_socket(int socket_fd) {
  if (socket_fd < 0) {
    throw std::invalid_argument("Invalid socket file descriptor.");
  }
  _socket_fd = socket_fd;
}

void set_args(const std::vector<msgpack_variant> &args) {
  _args = args;
}

void clean_args() {
  _args.clear();
}

void push_arg(const msgpack_variant &arg) {
  _args.push_back(arg);
}

template <typename T>
FunctionCall& operator<<(const T &arg) {
  push_arg(msgpack_variant(arg));
  return *this;
}

FunctionCall& args(const std::vector<msgpack_variant> &args) {
  for (const auto &arg : args) {
    push_arg(arg);
  }
  return *this;
}

template <typename... Args>
FunctionCall& args(Args &&...arg) {
  (push_arg(msgpack_variant(std::forward<Args>(arg))), ...);
  return *this;
}

std::vector<msgpack_variant> execute(msgpack_variant &args) {
  set_args({args});
  return execute();
}

std::vector<msgpack_variant> execute() {
  if (_socket_fd < 0) {
    throw std::runtime_error("Socket is not open. Call open_socket() before executing.");
  }
  int type = 0; // REQUEST
  static uint32_t msgid = 0;
  uint32_t request_msgid = msgid++;
  msgpack::sbuffer buffer;
  try {
    msgpack::packer<msgpack::sbuffer> pk(buffer);
    pk.pack_array(4);
    pk.pack(type);
    pk.pack(request_msgid);
    pk.pack(_name);
    pack_args(pk);
  } catch (const std::exception &e) {
    throw std::runtime_error("Error packing data: " + std::string(e.what()));
  }

  size_t bytes_sent = 0;
  while (bytes_sent < buffer.size()) {
    ssize_t n = send(_socket_fd, buffer.data() + bytes_sent,
                     buffer.size() - bytes_sent, 0);
    if (n < 0) {
      throw std::runtime_error("Error sending data: " +
                               std::string(strerror(errno)));
    }
    if (n == 0) {
      throw std::runtime_error("Socket closed while sending request");
    }
    bytes_sent += static_cast<size_t>(n);
  }

  msgpack::unpacker unp;
  msgpack::object_handle oh;
  while (true) {
    unp.reserve_buffer(4096);
    ssize_t n = recv(_socket_fd, unp.buffer(), unp.buffer_capacity(), 0);
    if (n < 0) {
      throw std::runtime_error("Error receiving response: " + std::string(strerror(errno)));
    }
    if (n == 0) {
      throw std::runtime_error("Connection closed before response was received");
    }
    unp.buffer_consumed(static_cast<size_t>(n));
    if (unp.next(oh)) {
      break;
    }
  }

  msgpack::object obj = oh.get();
  if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 4) {
    throw std::runtime_error("Invalid response format");
  }
  int response_type = obj.via.array.ptr[0].as<int>();
  uint32_t response_msgid = obj.via.array.ptr[1].as<uint32_t>();
  if (response_type != 1 || response_msgid != request_msgid) {
    throw std::runtime_error("Response does not match the request");
  }

  const msgpack::object &error = obj.via.array.ptr[2];
  if (error.type != msgpack::type::NIL) {
    throw std::runtime_error("RPC error: " + object_to_string(error));
  }

  return object_to_result(obj.via.array.ptr[3]);
}


private:
  template <typename Packer>
  static void pack_arg(Packer &pk, const msgpack_variant &arg) {
    std::visit([&pk](const auto &value) { pk.pack(value); }, arg);
  }

  template <typename Packer>
  void pack_args(Packer &pk) const {
    pk.pack_array(static_cast<uint32_t>(_args.size()));
    for (const auto &arg : _args) {
      pack_arg(pk, arg);
    }
  }

  static std::string object_to_string(const msgpack::object &obj) {
    if (obj.type == msgpack::type::STR) {
      return obj.as<std::string>();
    }

    std::ostringstream out;
    out << obj;
    return out.str();
  }

  static msgpack_variant object_to_variant(const msgpack::object &obj) {
    switch (obj.type) {
    case msgpack::type::BOOLEAN:
      return obj.as<bool>();
    case msgpack::type::POSITIVE_INTEGER:
      if (obj.via.u64 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw std::runtime_error("RPC integer result is out of range");
      }
      return static_cast<int64_t>(obj.via.u64);
    case msgpack::type::NEGATIVE_INTEGER:
      return obj.as<int64_t>();
    case msgpack::type::FLOAT32:
    case msgpack::type::FLOAT64:
      return obj.as<double>();
    case msgpack::type::STR:
      return obj.as<std::string>();
    default:
      throw std::runtime_error("Unsupported RPC result type: " +
                               object_to_string(obj));
    }
  }

  static std::vector<msgpack_variant> object_to_result(const msgpack::object &obj) {
    if (obj.type == msgpack::type::NIL) {
      return {};
    }
    if (obj.type != msgpack::type::ARRAY) {
      return {object_to_variant(obj)};
    }

    std::vector<msgpack_variant> result;
    result.reserve(obj.via.array.size);
    for (uint32_t i = 0; i < obj.via.array.size; ++i) {
      result.push_back(object_to_variant(obj.via.array.ptr[i]));
    }
    return result;
  }

  std::string _name;
  std::stringstream _buffer;
  int _socket_fd = -1;
  std::vector<msgpack_variant> _args;
}; // class FunctionCall
} // namespace RPCClient
