#pragma once
/// @file tiny_test.hpp
/// @brief 100 行以内、零依赖的单元测试框架。
///
/// 为什么不用 gtest: 全仓库零第三方依赖是本项目的卖点之一, 拿到任何机器上
/// `cmake -B build && cmake --build build` 就能跑, 不用联网拉包。
///
/// 用法: 在 tests/*.cpp 里(不要写 main, main 在 tiny_test_main.cpp):
///     #include "tiny_test.hpp"
///     SL_TEST(SpscRing_basic_push_pop) {
///       sublens::SpscRing<int, 8> ring;
///       SL_EXPECT_EQ(ring.push(...), 1u);
///     }

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace tiny_test {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

/// 断言失败时抛出, 由 run_all 捕获并计为失败, 后续用例继续跑。
struct Failure {
  std::string what;
};

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back(Case{name, std::move(fn)});
  }
};

template <typename A, typename B>
std::string describe(const A& a, const B& b) {
  std::ostringstream os;
  os << " 实际: [" << a << "]  期望: [" << b << "]";
  return os.str();
}

inline int run_all(const char* filter = nullptr) {
  int passed = 0;
  int failed = 0;
  for (const auto& c : registry()) {
    if (filter && c.name.find(filter) == std::string::npos) continue;
    try {
      c.fn();
      ++passed;
      std::printf("  [ OK ] %s\n", c.name.c_str());
    } catch (const Failure& f) {
      ++failed;
      std::printf("  [FAIL] %s\n         %s\n", c.name.c_str(), f.what.c_str());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("  [FAIL] %s\n         异常: %s\n", c.name.c_str(), e.what());
    } catch (...) {
      ++failed;
      std::printf("  [FAIL] %s\n         未知异常\n", c.name.c_str());
    }
  }
  std::printf("\n通过 %d 个, 失败 %d 个\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace tiny_test

#define SL_TEST(name)                                                        \
  static void name();                                                        \
  static ::tiny_test::Registrar tiny_test_reg_##name(#name, name);           \
  static void name()

#define SL_FAIL(msg)                                                         \
  do {                                                                       \
    std::ostringstream os_;                                                  \
    os_ << __FILE__ << ":" << __LINE__ << " 断言失败: " << (msg);            \
    throw ::tiny_test::Failure{os_.str()};                                   \
  } while (0)

#define SL_EXPECT(cond)                                                      \
  do {                                                                       \
    if (!(cond)) SL_FAIL(#cond);                                             \
  } while (0)

#define SL_EXPECT_EQ(actual, expected)                                       \
  do {                                                                       \
    const auto& a_ = (actual);                                                \
    const auto& e_ = (expected);                                              \
    if (!(a_ == e_)) {                                                        \
      std::ostringstream os_;                                                 \
      os_ << __FILE__ << ":" << __LINE__ << " 断言失败: " << #actual         \
          << " == " << #expected << "\\n" << ::tiny_test::describe(a_, e_);   \
      throw ::tiny_test::Failure{os_.str()};                                  \
    }                                                                         \
  } while (0)

#define SL_EXPECT_NEAR(actual, expected, eps)                                \
  do {                                                                       \
    const double a_ = static_cast<double>(actual);                            \
    const double e_ = static_cast<double>(expected);                          \
    if (std::fabs(a_ - e_) > (eps)) {                                         \
      std::ostringstream os_;                                                 \
      os_ << __FILE__ << ":" << __LINE__ << " 断言失败: " << #actual          \
          << " ≈ " << #expected << " (容差 " << (eps) << ")\\n"               \
          << ::tiny_test::describe(a_, e_);                                   \
      throw ::tiny_test::Failure{os_.str()};                                  \
    }                                                                         \
  } while (0)

#define SL_EXPECT_THROW(stmt)                                                \
  do {                                                                       \
    bool thrown_ = false;                                                    \
    try {                                                                    \
      stmt;                                                                  \
    } catch (...) {                                                          \
      thrown_ = true;                                                        \
    }                                                                        \
    if (!thrown_) SL_FAIL("期望抛出异常, 但没有: " #stmt);                     \
  } while (0)
