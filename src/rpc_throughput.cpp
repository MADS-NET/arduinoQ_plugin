// qrpc_throughput - Benchmark the throughput of msgpack RPC calls from the
// CPU (Linux) side of the Arduino Uno Q to the MCU side.
//
// It reads a JSON description of which RPC methods to exercise and how many
// times, runs each method one at a time against the Arduino Router socket,
// measures per-call latency, and reports aggregate statistics.
//
// Input JSON schema (from a file argument or from stdin):
//
//   {
//     "socket": "/var/run/arduino-router.sock",  // optional, overridable by --socket
//     "warmup": 10,                               // optional, default 0
//     "iterations": 1000,                         // optional default for every test
//     "tests": [
//       { "rpc_func": "ping", "rpc_args": [],            "iterations": 2000 },
//       { "rpc_func": "add",  "rpc_args": [3, 4],        "iterations": 1000 },
//       { "rpc_func": "echo", "rpc_args": ["hi", 42],    "iterations": 500, "label": "echo-2" }
//     ]
//   }
//
// Each test is run sequentially. Failing calls are counted but do not abort the
// run; their latencies are excluded from the timing statistics.

#include "rpc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int64_t default_iterations = 1000;

void print_usage(const char *prog) {
  std::cout
      << "Usage: " << prog << " [options] [input.json]\n\n"
      << "Benchmark msgpack RPC throughput from the CPU side of the Arduino\n"
      << "Uno Q. Reads a JSON test description from the given file or stdin.\n\n"
      << "Options:\n"
      << "  --socket PATH   Override the RPC socket path from the JSON input\n"
      << "  --json          Emit machine-readable JSON results on stdout\n"
      << "  -h, --help      Show this help and exit\n";
}

// Convert a JSON value into an RPC Value (mirrors the conversion used by the
// RPC client, kept local so a single parse happens before timing begins).
RPCClient::Value json_to_value(const json &value) {
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
    const uint64_t v = value.get<uint64_t>();
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::invalid_argument("rpc_args contains an integer out of range");
    }
    return static_cast<int64_t>(v);
  }
  if (value.is_number_float()) {
    return value.get<double>();
  }
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_array()) {
    RPCClient::Value::array_type out;
    out.reserve(value.size());
    for (const auto &item : value) {
      out.emplace_back(json_to_value(item));
    }
    return out;
  }
  throw std::invalid_argument(
      "rpc_args supports only null, bool, number, string, and arrays");
}

struct TestSpec {
  std::string label;
  std::string method;
  std::vector<RPCClient::Value> args;
  int64_t iterations = default_iterations;
};

struct Stats {
  std::string label;
  int64_t iterations = 0;
  int64_t ok = 0;
  int64_t errors = 0;
  std::string first_error;
  double total_s = 0.0;     // wall time spent on the timed calls
  double throughput = 0.0;  // successful calls per second
  // Latency statistics, in microseconds.
  double min_us = 0.0;
  double mean_us = 0.0;
  double median_us = 0.0;
  double p90_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  double max_us = 0.0;
  double stddev_us = 0.0;
};

double percentile(const std::vector<double> &sorted, double pct) {
  if (sorted.empty()) {
    return 0.0;
  }
  if (sorted.size() == 1) {
    return sorted.front();
  }
  const double rank = pct / 100.0 * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(rank));
  const size_t hi = static_cast<size_t>(std::ceil(rank));
  const double frac = rank - static_cast<double>(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

Stats compute_stats(const std::string &label, int64_t iterations,
                    int64_t errors, const std::string &first_error,
                    double total_s, std::vector<double> &latencies) {
  Stats s;
  s.label = label;
  s.iterations = iterations;
  s.ok = static_cast<int64_t>(latencies.size());
  s.errors = errors;
  s.first_error = first_error;
  s.total_s = total_s;
  s.throughput = total_s > 0.0 ? static_cast<double>(s.ok) / total_s : 0.0;

  if (latencies.empty()) {
    return s;
  }

  std::sort(latencies.begin(), latencies.end());
  double sum = 0.0;
  for (double v : latencies) {
    sum += v;
  }
  s.mean_us = sum / static_cast<double>(latencies.size());

  double sq = 0.0;
  for (double v : latencies) {
    const double d = v - s.mean_us;
    sq += d * d;
  }
  s.stddev_us = std::sqrt(sq / static_cast<double>(latencies.size()));

  s.min_us = latencies.front();
  s.max_us = latencies.back();
  s.median_us = percentile(latencies, 50.0);
  s.p90_us = percentile(latencies, 90.0);
  s.p95_us = percentile(latencies, 95.0);
  s.p99_us = percentile(latencies, 99.0);
  return s;
}

std::string read_all(std::istream &in) {
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Run a single test against an open client and collect its statistics.
Stats run_test(RPCClient::Client &client, const TestSpec &test,
               int64_t warmup) {
  // Warmup calls are not measured; they prime caches and connections.
  for (int64_t i = 0; i < warmup; ++i) {
    try {
      client.call(test.method, test.args);
    } catch (const std::exception &) {
      // Ignore warmup failures; the timed loop will record real errors.
    }
  }

  std::vector<double> latencies;
  latencies.reserve(static_cast<size_t>(std::max<int64_t>(test.iterations, 0)));
  int64_t errors = 0;
  std::string first_error;

  const auto start = Clock::now();
  for (int64_t i = 0; i < test.iterations; ++i) {
    const auto t0 = Clock::now();
    try {
      client.call(test.method, test.args);
      const auto t1 = Clock::now();
      const double us =
          std::chrono::duration<double, std::micro>(t1 - t0).count();
      latencies.push_back(us);
    } catch (const std::exception &e) {
      ++errors;
      if (first_error.empty()) {
        first_error = e.what();
      }
    }
  }
  const auto end = Clock::now();
  const double total_s =
      std::chrono::duration<double>(end - start).count();

  return compute_stats(test.label, test.iterations, errors, first_error,
                       total_s, latencies);
}

json stats_to_json(const Stats &s) {
  json j;
  j["label"] = s.label;
  j["iterations"] = s.iterations;
  j["ok"] = s.ok;
  j["errors"] = s.errors;
  if (!s.first_error.empty()) {
    j["first_error"] = s.first_error;
  }
  j["total_s"] = s.total_s;
  j["throughput_per_s"] = s.throughput;
  j["latency_us"] = {
      {"min", s.min_us},   {"mean", s.mean_us}, {"median", s.median_us},
      {"p90", s.p90_us},   {"p95", s.p95_us},   {"p99", s.p99_us},
      {"max", s.max_us},   {"stddev", s.stddev_us},
  };
  return j;
}

void print_human(const std::vector<Stats> &results) {
  std::cout << "\nThroughput results\n";
  std::cout << std::string(96, '=') << "\n";
  std::cout << std::left << std::setw(18) << "method" << std::right
            << std::setw(8) << "ok" << std::setw(7) << "err"
            << std::setw(13) << "thr/s" << std::setw(11) << "mean us"
            << std::setw(11) << "median" << std::setw(11) << "p95"
            << std::setw(11) << "max" << "\n";
  std::cout << std::string(96, '-') << "\n";
  std::cout << std::fixed;
  for (const auto &s : results) {
    std::cout << std::left << std::setw(18) << s.label << std::right
              << std::setw(8) << s.ok << std::setw(7) << s.errors
              << std::setprecision(1) << std::setw(13) << s.throughput
              << std::setprecision(2) << std::setw(11) << s.mean_us
              << std::setw(11) << s.median_us << std::setw(11) << s.p95_us
              << std::setw(11) << s.max_us << "\n";
    if (s.errors > 0 && !s.first_error.empty()) {
      std::cout << "    " << std::setw(14) << "" << "first error: "
                << s.first_error << "\n";
    }
  }
  std::cout << std::string(96, '=') << "\n";
}

} // namespace

int main(int argc, char const *argv[]) {
  std::string input_path;
  std::string socket_override;
  bool emit_json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--json") {
      emit_json = true;
    } else if (arg == "--socket") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --socket requires a path argument\n";
        return 1;
      }
      socket_override = argv[++i];
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Error: unknown option '" << arg << "'\n";
      print_usage(argv[0]);
      return 1;
    } else if (input_path.empty()) {
      input_path = arg;
    } else {
      std::cerr << "Error: unexpected extra argument '" << arg << "'\n";
      return 1;
    }
  }

  // Load and parse the JSON test description.
  std::string raw;
  try {
    if (input_path.empty()) {
      raw = read_all(std::cin);
    } else {
      std::ifstream file(input_path);
      if (!file) {
        std::cerr << "Error: cannot open input file '" << input_path << "'\n";
        return 1;
      }
      raw = read_all(file);
    }
  } catch (const std::exception &e) {
    std::cerr << "Error reading input: " << e.what() << "\n";
    return 1;
  }

  json doc;
  try {
    doc = json::parse(raw);
  } catch (const std::exception &e) {
    std::cerr << "Error: invalid JSON input: " << e.what() << "\n";
    return 1;
  }

  if (!doc.is_object() || !doc.contains("tests") || !doc["tests"].is_array()) {
    std::cerr << "Error: input must be an object with a 'tests' array\n";
    return 1;
  }

  std::string socket_path{RPCClient::default_socket_path};
  if (doc.contains("socket") && doc["socket"].is_string()) {
    socket_path = doc["socket"].get<std::string>();
  }
  if (!socket_override.empty()) {
    socket_path = socket_override;
  }

  int64_t warmup = 0;
  if (doc.contains("warmup") && doc["warmup"].is_number_integer()) {
    warmup = std::max<int64_t>(0, doc["warmup"].get<int64_t>());
  }
  int64_t global_iterations = default_iterations;
  if (doc.contains("iterations") && doc["iterations"].is_number_integer()) {
    global_iterations = doc["iterations"].get<int64_t>();
  }

  // Build the list of tests up front so input errors are reported before any
  // RPC traffic is generated.
  std::vector<TestSpec> tests;
  try {
    int index = 0;
    for (const auto &entry : doc["tests"]) {
      ++index;
      if (!entry.is_object() || !entry.contains("rpc_func") ||
          !entry["rpc_func"].is_string()) {
        throw std::invalid_argument(
            "test #" + std::to_string(index) +
            " must be an object with a string 'rpc_func'");
      }
      TestSpec spec;
      spec.method = entry["rpc_func"].get<std::string>();
      spec.label = entry.value("label", spec.method);
      spec.iterations = entry.value("iterations", global_iterations);
      if (spec.iterations <= 0) {
        throw std::invalid_argument("test '" + spec.label +
                                    "' must have a positive iteration count");
      }
      if (entry.contains("rpc_args")) {
        if (!entry["rpc_args"].is_array()) {
          throw std::invalid_argument("test '" + spec.label +
                                      "' rpc_args must be an array");
        }
        for (const auto &a : entry["rpc_args"]) {
          spec.args.emplace_back(json_to_value(a));
        }
      }
      tests.push_back(std::move(spec));
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  if (tests.empty()) {
    std::cerr << "Error: no tests to run\n";
    return 1;
  }

  // Connect once and reuse the connection across all tests.
  RPCClient::Client client;
  try {
    client.connect(socket_path);
  } catch (const std::exception &e) {
    std::cerr << "Error connecting to RPC socket '" << socket_path
              << "': " << e.what() << "\n";
    return 1;
  }

  if (!emit_json) {
    std::cerr << "Connected to " << socket_path << "; running " << tests.size()
              << " test(s), warmup=" << warmup << "\n";
  }

  std::vector<Stats> results;
  results.reserve(tests.size());
  for (const auto &test : tests) {
    if (!emit_json) {
      std::cerr << "  " << test.label << " (" << test.iterations
                << " iterations)..." << std::flush;
    }
    Stats s = run_test(client, test, warmup);
    if (!emit_json) {
      std::cerr << " done (" << std::fixed << std::setprecision(1)
                << s.throughput << " calls/s)\n";
    }
    results.push_back(std::move(s));
  }

  if (emit_json) {
    json out;
    out["socket"] = socket_path;
    out["warmup"] = warmup;
    out["results"] = json::array();
    for (const auto &s : results) {
      out["results"].push_back(stats_to_json(s));
    }
    std::cout << out.dump(2) << "\n";
  } else {
    print_human(results);
  }

  return 0;
}
