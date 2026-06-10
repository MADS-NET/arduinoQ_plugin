#include "rpc.hpp"
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

int main(int argc, char const *argv[]) {
  string function_name;
  filesystem::path socket_path = RPCClient::default_socket_path;
  int arg_start = 2;
  if (argc < 2) {
    cout << "Usage: " << argv[0]
         << " [/path/to/socket] <function> [arg1] [arg2] ..." << endl;
    return 1;
  }

  if (filesystem::exists(argv[1])) {
    socket_path = argv[1];
    if (argc < 3) {
      cout << "Usage: " << argv[0]
           << " [/path/to/socket] <function> [arg1] [arg2] ..." << endl;
      return 1;
    }
    function_name = argv[2];
    arg_start = 3;
  } else {
    function_name = argv[1];
  }

  cout << "Connecting to RPC server at " << socket_path
       << " and calling function '" << function_name << "' with arguments: ";
  for (int i = arg_start; i < argc; ++i) {
    cout << argv[i] << " ";
  }
  cout << endl;

  vector<RPCClient::Value> args;
  for (int i = arg_start; i < argc; ++i) {
    string arg_str = argv[i];
    try {
      size_t pos = 0;
      int64_t int_arg = stoll(arg_str, &pos);
      if (pos != arg_str.size()) {
        throw invalid_argument("not an integer");
      }
      args.emplace_back(int_arg);
    } catch (const exception &) {
      try {
        size_t pos = 0;
        double double_arg = stod(arg_str, &pos);
        if (pos != arg_str.size()) {
          throw invalid_argument("not a double");
        }
        args.emplace_back(double_arg);
      } catch (const exception &) {
        if (arg_str == "true" || arg_str == "on") {
          args.emplace_back(true);
        } else if (arg_str == "false" || arg_str == "off") {
          args.emplace_back(false);
        } else {
          args.emplace_back(arg_str);
        }
      }
    }
  }

  try {
    RPCClient::Client client(socket_path.string());
    auto result = client.call(function_name, args);
    cout << "RPC call result: " << result << endl;
  } catch (const exception &e) {
    cerr << "Error executing RPC call: " << e.what() << endl;
    return 1;
  }
  return 0;
}
