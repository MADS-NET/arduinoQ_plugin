#include "rpc.hpp"
#include <iostream>

using namespace std;

int main(int argc, char const *argv[]) {
  bool led_state = true;
  if (argc > 1) {
    string arg = argv[1];
    if (arg == "on") {
      led_state = true;
    } else if (arg == "off") {
      led_state = false;
    } else {
      cerr << "Usage: " << argv[0] << " [on|off]" << endl;
      return 1;
    }
  }
  RPCClient::FunctionCall set_led_call("set_led");
  set_led_call.open_socket();
  set_led_call << led_state;
  auto result = set_led_call.execute();
  cout << "RPC call result: ";
  for (const auto &arg : result) {
    std::visit([](const auto &value) { cout << value << " "; }, arg);
  }
  cout << endl;

  return 0;
}
