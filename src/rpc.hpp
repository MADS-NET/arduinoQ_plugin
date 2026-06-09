#pragma once

#define MSGPACK_USE_STD_VARIANT_ADAPTOR
#define MSGPACK_NO_BOOST
#include <msgpack.hpp>
#include <sstream>
#include <variant>
#include <vector>
#include <filesystem>

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
  std::stringstream buffer;
  std::string data;
  try {
    msgpack::pack(buffer, std::make_tuple(type, msgid++, _name, _args));
    data = buffer.str();
  } catch (const std::exception &e) {
    throw std::runtime_error("Error packing data: " + std::string(e.what()));
  }
  ssize_t bytes_sent = send(_socket_fd, data.data(), data.size(), 0);
  if (bytes_sent < 0) {
    throw std::runtime_error("Error sending data: " + std::string(strerror(errno)));
  }

  // Read response with dynamic buffer that grows as needed
  std::vector<char> response_buffer;
  response_buffer.reserve(4096);
  
  char read_buf[4096];
  ssize_t bytes_received;
  do {
    ssize_t n = recv(_socket_fd, read_buf, sizeof(read_buf), 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No more data available right now, break out
        break;
      }
      throw std::runtime_error("Error receiving response: " + std::string(strerror(errno)));
    }
    if (n == 0) {
      // Connection closed by peer
      break;
    }
    response_buffer.insert(response_buffer.end(), read_buf, read_buf + n);
  } while (true);

  if (response_buffer.empty()) {
    throw std::runtime_error("No response received");
  }

  msgpack::object_handle oh = msgpack::unpack(response_buffer.data(), response_buffer.size());
  msgpack::object obj = oh.get();
  if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 4) {
    throw std::runtime_error("Invalid response format");
  }
  int response_type = obj.via.array.ptr[0].as<int>();
  uint32_t response_msgid = obj.via.array.ptr[1].as<uint32_t>();
  std::string response_function = obj.via.array.ptr[2].as<std::string>();
  std::vector<msgpack_variant> response_args = obj.via.array.ptr[3].as<std::vector<msgpack_variant>>();
  if (response_type != 1 || response_msgid != msgid - 1 || response_function != _name) {
    throw std::runtime_error("Response does not match the request");
  }


  return response_args; // Return the parsed response arguments
}


private:
  std::string _name;
  std::stringstream _buffer;
  int _socket_fd;
  std::vector<msgpack_variant> _args;
}; // class FunctionCall
} // namespace RPCClient