#pragma once

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

class TestSuite {
public:
  using Fn = std::function<void()>;

  void add(std::string name, Fn fn) { tests_.push_back(Test{std::move(name), std::move(fn)}); }

  int run() {
    size_t passed = 0;
    for (const auto& test : tests_) {
      try {
        test.fn();
        ++passed;
        std::cout << "[PASS] " << test.name << '\n';
      } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
        return 1;
      }
    }
    std::cout << passed << " tests passed\n";
    return 0;
  }

private:
  struct Test {
    std::string name;
    Fn fn;
  };
  std::vector<Test> tests_;
};

template <typename T>
void require(const T& condition, std::string_view message) {
  if (!static_cast<bool>(condition)) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename L, typename R>
void require_eq(const L& lhs, const R& rhs, std::string_view message) {
  if (!(lhs == rhs)) {
    throw std::runtime_error(std::string(message));
  }
}

void register_resp_parser_tests(TestSuite& suite);
void register_storage_tests(TestSuite& suite);
void register_command_tests(TestSuite& suite);
void register_persistence_tests(TestSuite& suite);
