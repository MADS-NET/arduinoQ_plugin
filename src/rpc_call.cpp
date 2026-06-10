#include "rpc.hpp"
#include <iostream>
#include <filesystem>

using namespace std;

int main(int argc, char const *argv[]) {
  string function_name;
  filesystem::path socket_path = RPCClient::default_socket_path;
  if (argc < 2) {
    cout << "Usage: " << argv[0] << " [/path/to/socket] <function> [arg1] [arg2] ..." << endl;
    return 1;
  }

  if (filesystem::exists(argv[1])) {
    socket_path = argv[1];
    if (argc < 3) {
      cout << "Usage: " << argv[0] << " [/path/to/socket] <function> [arg1] [arg2] ..." << endl;
      return 1;
    }
    function_name = argv[2];
  } else {
    function_name = argv[1];
  }
  
  cout << "Connecting to RPC server at " << socket_path << " and calling function '" << function_name << "' with arguments: ";
  for (int i = 2; i < argc; ++i) {
    cout << argv[i] << " ";
  }
  cout << endl;

  RPCClient::FunctionCall function(function_name);
  function.open_socket(socket_path.string());

  for (int i = 2; i < argc; ++i) {
    string arg_str = argv[i];
    try {
      size_t pos = 0;
      int64_t int_arg = stoll(arg_str, &pos);
      if (pos != arg_str.size()) {
        throw invalid_argument("not an integer");
      }
      function << int_arg;
    } catch (const exception &) {
      try {
        size_t pos = 0;
        double double_arg = stod(arg_str, &pos);
        if (pos != arg_str.size()) {
          throw invalid_argument("not a double");
        }
        function << double_arg;
      } catch (const exception &) {
        if (arg_str == "true" || arg_str == "on") {
          function << true;
        } else if (arg_str == "false" || arg_str == "off") {
          function << false;
        } else {
          function << arg_str;
        }
      }
    }
  }

  try {
    auto result = function.execute();
    cout << "RPC call result: ";
    for (const auto &arg : result) {
      std::visit([](const auto &value) { cout << value << " "; }, arg);
    }
    cout << endl;
  } catch (const exception &e) {
    cerr << "Error executing RPC call: " << e.what() << endl;
    return 1;
  }
  return 0;
}
