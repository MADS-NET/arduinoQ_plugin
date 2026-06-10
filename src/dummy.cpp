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
#include <sstream>
#include <stdexcept>
#include <string>

static volatile std::sig_atomic_t Running = 1;

static void handle_sigint(int) { Running = 0; }

static void write_all(int fd, const char *data, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = ::write(fd, data + done, size - done);
    if (n <= 0)
      std::exit(1);
    done += static_cast<size_t>(n);
  }
}

static void send_response(int fd, uint32_t id, const std::string *error,
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

  write_all(fd, buf.data(), buf.size());
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

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/mads-rpc.sock";
  ::unlink(path);

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

  std::cout << "dummy RPC server listening on " << path << "\n";

  while (Running) {
    int fd = ::accept(srv, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("accept");
      return 1;
    }
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

        auto *a = obj.via.array.ptr;
        uint32_t id = a[1].as<uint32_t>();
        std::string method = a[2].as<std::string>();
        auto params = a[3];

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
            msgpack::object nil;
            nil.type = msgpack::type::NIL;
            std::string err = e.what();
            send_response(fd, id, &err, nil);
            continue;
          }
        } else if (method == "array") {
          pk.pack_array(3);
          pk.pack(1.1);
          pk.pack(2.2);
          pk.pack(3.3);
        } else {
          msgpack::zone z;
          msgpack::object nil;
          nil.type = msgpack::type::NIL;
          std::string err = "method not available: " + method;
          send_response(fd, id, &err, nil);
          continue;
        }

        msgpack::object_handle rh =
            msgpack::unpack(result_buf.data(), result_buf.size());

        send_response(fd, id, nullptr, rh.get());
      }
    }
    ::close(fd);
  }

  signal(SIGINT, SIG_DFL);
  std::cout << "shutting down dummy RPC server" << std::endl;

  ::close(srv);
  ::unlink(path);
}
