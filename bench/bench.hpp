#pragma once
/// @file bench.hpp
/// @brief 基准测试的公共设施。
///
/// 为什么基准要单独写: 作品集里"延迟 300ms"这种数字如果没有测量口径和
/// 测量代码, 就是吹牛。这里的每个数都能用 `sublens_bench` 一条命令复现,
/// 并且注明是"本机实测"还是"注入时延模型"。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sublens/common.hpp"

namespace sublens_bench {

/// 一个基准项的输出
struct BenchReport {
  std::string id;        ///< 简短标识, 如 "ring"
  std::string title;     ///< 中文标题
  std::string markdown;  ///< Markdown 正文(表格 + 说明)
};

/// 记一段代码的耗时(纳秒)
class Stopwatch {
 public:
  Stopwatch() : begin_(std::chrono::steady_clock::now()) {}
  int64_t ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - begin_)
        .count();
  }
  double ms() const { return static_cast<double>(ns()) / 1e6; }

 private:
  std::chrono::steady_clock::time_point begin_;
};

using Snapshot = sublens::LatencyHistogram::Snapshot;

/// 延迟表头(单位写在标题里)
inline const char* kLatencyHeader() {
  return "| 指标 | 样本数 | mean | p50 | p95 | p99 | max |\n"
         "|---|---|---|---|---|---|---|\n";
}

/// 一行延迟数据
inline std::string latency_row(const std::string& name, const Snapshot& s) {
  char buf[512];
  if (s.count == 0) {
    std::snprintf(buf, sizeof(buf), "| %s | 0 | - | - | - | - | - |\n", name.c_str());
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "| %s | %llu | %.1f | %lld | %lld | %lld | %lld |\n",
                name.c_str(), static_cast<unsigned long long>(s.count), s.mean,
                static_cast<long long>(s.p50), static_cast<long long>(s.p95),
                static_cast<long long>(s.p99), static_cast<long long>(s.max));
  return buf;
}

/// 把若干延迟指标拼成一张表(单位由调用方写进标题)
inline std::string latency_table(const std::string& unit,
                                 const std::vector<std::pair<std::string, Snapshot>>& rows) {
  std::string md = "单位: " + unit + "\n\n";
  md += kLatencyHeader();
  for (const auto& [name, s] : rows) md += latency_row(name, s);
  return md;
}

/// 从原始样本求百分位(小数据量、非直方图场景用)
inline int64_t percentile(std::vector<int64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
  return v[idx];
}

// 各基准项
BenchReport bench_ring();
BenchReport bench_vad();
BenchReport bench_stabilizer();
BenchReport bench_hud();
BenchReport bench_pipeline_latency();
BenchReport bench_pipeline_throughput();

}  // namespace sublens_bench
