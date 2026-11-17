/// @file tiny_test_main.cpp  测试可执行文件的唯一 main
///
/// 用法: sublens_tests [过滤器]
///   过滤器为用例名子串, 便于只跑某一模块, 例如 `sublens_tests Vad。`
#include <cstdio>
#include <cstring>

#include "tiny_test.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  std::printf("SubLens 单元测试%s%s\n\n", filter ? " (过滤: " : "", filter ? filter : "");
  const int code = tiny_test::run_all(filter);
  if (filter) std::printf("\n");
  return code;
}
