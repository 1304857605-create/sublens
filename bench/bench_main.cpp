/// @file bench_main.cpp  基准测试入口
///
/// 用法:
///   sublens_bench            # 跑全部
///   sublens_bench ring vad   # 只跑指定项
///
/// 输出是 Markdown, 直接对应 docs/BENCHMARK.md 里的表格。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);  // 让中文在 Windows 控制台正常显示
#endif
  std::vector<std::string> filters;
  for (int i = 1; i < argc; ++i) filters.emplace_back(argv[i]);

  struct Item {
    const char* id;
    sublens_bench::BenchReport (*fn)();
  };
  const Item items[] = {
      {"ring", sublens_bench::bench_ring},
      {"vad", sublens_bench::bench_vad},
      {"stabilizer", sublens_bench::bench_stabilizer},
      {"hud", sublens_bench::bench_hud},
      {"latency", sublens_bench::bench_pipeline_latency},
      {"throughput", sublens_bench::bench_pipeline_throughput},
  };

  std::printf("# SubLens 基准测试结果\n\n");
  std::printf("> 机器: MinGW g++ 16.1, Release (-O3), 单次运行产生; 可复现: `sublens_bench`\n\n");

  for (const auto& item : items) {
    if (!filters.empty()) {
      bool hit = false;
      for (const auto& f : filters) {
        if (std::strcmp(f.c_str(), item.id) == 0) hit = true;
      }
      if (!hit) continue;
    }
    std::printf("## %s\n\n", item.id);
    const auto report = item.fn();
    std::fputs(report.markdown.c_str(), stdout);
    std::printf("\n");
    std::fflush(stdout);
  }
  return 0;
}
