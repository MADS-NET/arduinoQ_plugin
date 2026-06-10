#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>

namespace RPCClient {

inline constexpr std::string_view default_socket_path =
    "/var/run/arduino-router.sock";

class Value {
public:
  using array_type = std::vector<Value>;
  using storage_type = std::variant<std::monostate, bool, int64_t, double,
                                    std::string, array_type>;

  Value() = default;
  Value(std::nullptr_t) : _value(std::monostate{}) {}
  Value(bool value) : _value(value) {}
  Value(const char *value);
  Value(std::string value) : _value(std::move(value)) {}
  Value(std::string_view value) : _value(std::string(value)) {}
  Value(const array_type &value) : _value(value) {}
  Value(array_type &&value) : _value(std::move(value)) {}

  template <typename T,
            typename = std::enable_if_t<std::is_integral_v<std::decay_t<T>> &&
                                        !std::is_same_v<std::decay_t<T>, bool>>>
  Value(T value) : _value(checked_integer(value)) {}

  template <
      typename T,
      typename = std::enable_if_t<std::is_floating_point_v<std::decay_t<T>>>,
      typename = void>
  Value(T value) : _value(static_cast<double>(value)) {}

  const storage_type &storage() const { return _value; }

private:
  template <typename T> static int64_t checked_integer(T value) {
    if constexpr (std::is_unsigned_v<T>) {
      if (value > static_cast<T>(std::numeric_limits<int64_t>::max())) {
        throw std::out_of_range("integer value is out of int64_t range");
      }
    }
    return static_cast<int64_t>(value);
  }

  storage_type _value;
};

using msgpack_variant = Value;
using Result = Value;

std::ostream &operator<<(std::ostream &out, const Value &value);
std::string to_string(const Value &value);

class Client {
public:
  Client() = default;
  explicit Client(std::string_view socket_path);
  ~Client();

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  Client(Client &&other) noexcept;
  Client &operator=(Client &&other) noexcept;

  bool is_open() const { return _socket_fd >= 0; }
  void close();

  void connect(std::string_view socket_path = default_socket_path);
  void adopt_socket(int socket_fd);

  Result call(std::string_view method,
              const std::vector<Value> &args = std::vector<Value>{});
  Result call(std::string_view method, std::initializer_list<Value> args);
  Result call(nlohmann::json call);

private:
  int _socket_fd = -1;
  uint32_t _next_msgid = 0;
};

} // namespace RPCClient
