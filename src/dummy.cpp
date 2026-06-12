#include <msgpack.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

static volatile std::sig_atomic_t Running = 1;
static std::mutex RegistryMutex;
static std::mutex SendMutex;
static std::map<std::string, int> RegisteredMethods;

static void handle_sigint(int) { Running = 0; }

static bool write_all(int fd, const char *data, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = ::write(fd, data + done, size - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

static bool send_buffer(int fd, const msgpack::sbuffer &buf) {
  std::lock_guard<std::mutex> lock(SendMutex);
  return write_all(fd, buf.data(), buf.size());
}

static bool send_response(int fd, uint32_t id, const std::string *error,
                          const msgpack::object &result) {
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);

  pk.pack_array(4);
  pk.pack(1); // RESPONSE
  pk.pack(id);

  if (error)
    pk.pack(*error);
  else
    pk.pack_nil();

  pk << result;

  return send_buffer(fd, buf);
}

static bool send_error_response(int fd, uint32_t id, const std::string &error) {
  msgpack::object nil;
  nil.type = msgpack::type::NIL;
  return send_response(fd, id, &error, nil);
}

static bool send_packed_result(int fd, uint32_t id,
                               const msgpack::sbuffer &result_buf) {
  msgpack::object_handle rh =
      msgpack::unpack(result_buf.data(), result_buf.size());
  return send_response(fd, id, nullptr, rh.get());
}

static void register_method(int fd, const std::string &method) {
  std::lock_guard<std::mutex> lock(RegistryMutex);
  RegisteredMethods[method] = fd;
  std::cout << "registered method '" << method << "' on fd " << fd << "\n";
}

static void reset_methods(int fd) {
  std::lock_guard<std::mutex> lock(RegistryMutex);
  for (auto it = RegisteredMethods.begin(); it != RegisteredMethods.end();) {
    if (it->second == fd) {
      std::cout << "unregistered method '" << it->first << "' from fd " << fd
                << "\n";
      it = RegisteredMethods.erase(it);
    } else {
      ++it;
    }
  }
}

static int lookup_method(const std::string &method) {
  std::lock_guard<std::mutex> lock(RegistryMutex);
  const auto it = RegisteredMethods.find(method);
  if (it == RegisteredMethods.end()) {
    return -1;
  }
  return it->second;
}

static bool send_notification(int fd, const std::string &method,
                              const msgpack::object &params,
                              uint32_t first_param = 0) {
  if (params.type != msgpack::type::ARRAY ||
      first_param > params.via.array.size) {
    return false;
  }

  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(3);
  pk.pack(2); // NOTIFICATION
  pk.pack(method);
  pk.pack_array(params.via.array.size - first_param);
  for (uint32_t i = first_param; i < params.via.array.size; ++i) {
    pk << params.via.array.ptr[i];
  }

  return send_buffer(fd, buf);
}

static bool route_notification(const std::string &method,
                               const msgpack::object &params,
                               uint32_t first_param = 0) {
  const int target_fd = lookup_method(method);
  if (target_fd < 0) {
    return false;
  }
  return send_notification(target_fd, method, params, first_param);
}

static std::string format_echo_arg(const msgpack::object &arg) {
  std::ostringstream out;

  switch (arg.type) {
  case msgpack::type::BOOLEAN:
    out << "bool: " << (arg.as<bool>() ? "true" : "false");
    break;
  case msgpack::type::POSITIVE_INTEGER:
    if (arg.via.u64 >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::runtime_error("integer argument is out of int64_t range");
    }
    out << "int64_t: " << static_cast<int64_t>(arg.via.u64);
    break;
  case msgpack::type::NEGATIVE_INTEGER:
    out << "int64_t: " << arg.as<int64_t>();
    break;
  case msgpack::type::FLOAT32:
  case msgpack::type::FLOAT64:
    out << "double: " << arg.as<double>();
    break;
  case msgpack::type::STR:
    out << "std::string: " << arg.as<std::string>();
    break;
  default:
    throw std::runtime_error("unsupported echo argument type");
  }

  return out.str();
}

static std::string format_echo_result(const msgpack::object &params) {
  if (params.type != msgpack::type::ARRAY) {
    throw std::runtime_error("echo parameters must be an array");
  }

  std::ostringstream out;
  for (uint32_t i = 0; i < params.via.array.size; ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << format_echo_arg(params.via.array.ptr[i]);
  }
  return out.str();
}

static bool handle_router_request(int fd, uint32_t id,
                                  const std::string &method,
                                  const msgpack::object &params) {
  if (method == "$/register") {
    if (params.type != msgpack::type::ARRAY || params.via.array.size != 1 ||
        params.via.array.ptr[0].type != msgpack::type::STR) {
      return send_error_response(fd, id, "$/register expects one method name");
    }

    register_method(fd, params.via.array.ptr[0].as<std::string>());
    msgpack::sbuffer result_buf;
    msgpack::packer<msgpack::sbuffer> pk(result_buf);
    pk.pack(true);
    return send_packed_result(fd, id, result_buf);
  }

  if (method == "$/reset") {
    reset_methods(fd);
    msgpack::sbuffer result_buf;
    msgpack::packer<msgpack::sbuffer> pk(result_buf);
    pk.pack(true);
    return send_packed_result(fd, id, result_buf);
  }

  return false;
}

static bool handle_builtin_request(int fd, uint32_t id,
                                   const std::string &method,
                                   const msgpack::object &params) {
  msgpack::sbuffer result_buf;
  msgpack::packer<msgpack::sbuffer> pk(result_buf);

  if (method == "ping") {
    pk.pack(std::string("pong"));
  } else if (method == "add") {
    int x = params.via.array.ptr[0].as<int>();
    int y = params.via.array.ptr[1].as<int>();
    pk.pack(x + y);
  } else if (method == "echo") {
    try {
      pk.pack(format_echo_result(params));
    } catch (const std::exception &e) {
      return send_error_response(fd, id, e.what());
    }
  } else if (method == "array") {
    pk.pack_array(3);
    pk.pack(1.1);
    pk.pack(2.2);
    pk.pack(3.3);
  } else if (method == "emit_notify") {
    if (params.type != msgpack::type::ARRAY || params.via.array.size < 1 ||
        params.via.array.ptr[0].type != msgpack::type::STR) {
      return send_error_response(
          fd, id, "emit_notify expects a method name and optional arguments");
    }
    const std::string notify_method =
        params.via.array.ptr[0].as<std::string>();
    const bool routed = route_notification(notify_method, params, 1);
    std::cout << "emit_notify '" << notify_method << "' routed="
              << (routed ? "true" : "false") << "\n";
    pk.pack(routed);
  } else {
    return send_error_response(fd, id, "method not available: " + method);
  }

  return send_packed_result(fd, id, result_buf);
}

static void handle_request(int fd, const msgpack::object &obj) {
  if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 4) {
    return;
  }

  auto *a = obj.via.array.ptr;
  const uint32_t id = a[1].as<uint32_t>();
  const std::string method = a[2].as<std::string>();
  const auto params = a[3];

  if (handle_router_request(fd, id, method, params)) {
    return;
  }
  handle_builtin_request(fd, id, method, params);
}

static void handle_notification(const msgpack::object &obj) {
  if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 3) {
    return;
  }
  const std::string method = obj.via.array.ptr[1].as<std::string>();
  route_notification(method, obj.via.array.ptr[2]);
}

static void client_loop(int fd) {
  msgpack::unpacker unp;

  while (Running) {
    unp.reserve_buffer(4096);
    ssize_t n = ::read(fd, unp.buffer(), unp.buffer_capacity());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      break;
    }
    if (n == 0) {
      break;
    }

    unp.buffer_consumed(static_cast<size_t>(n));

    msgpack::object_handle oh;
    while (unp.next(oh)) {
      auto obj = oh.get();
      if (obj.type != msgpack::type::ARRAY || obj.via.array.size == 0) {
        continue;
      }

      const int type = obj.via.array.ptr[0].as<int>();
      if (type == 0) {
        handle_request(fd, obj);
      } else if (type == 2) {
        handle_notification(obj);
      }
    }
  }

  reset_methods(fd);
  ::close(fd);
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/mads-rpc.sock";
  ::unlink(path);
  std::signal(SIGPIPE, SIG_IGN);

  struct sigaction action {};
  action.sa_handler = handle_sigint;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (::sigaction(SIGINT, &action, nullptr) < 0) {
    perror("sigaction");
    return 1;
  }

  int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

  if (::bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }

  if (::listen(srv, 8) < 0) {
    perror("listen");
    return 1;
  }

  std::cout << "dummy RPC router listening on " << path << "\n";

  while (Running) {
    int fd = ::accept(srv, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("accept");
      return 1;
    }
    std::thread(client_loop, fd).detach();
  }

  std::signal(SIGINT, SIG_DFL);
  std::cout << "shutting down dummy RPC router" << std::endl;

  ::close(srv);
  ::unlink(path);
}
