/// @file stats.cpp  LatencyHistogram 的实现
#include "sublens/common.hpp"

#include <cmath>
#include <cstdio>

namespace sublens {

LatencyHistogram::LatencyHistogram(Config cfg) : cfg_(std::move(cfg)) {
  if (cfg_.bucket_width <= 0) cfg_.bucket_width = 1;
  if (cfg_.max_value < cfg_.bucket_width) cfg_.max_value = cfg_.bucket_width;
  const size_t n = static_cast<size_t>(cfg_.max_value / cfg_.bucket_width) + 2;
  buckets_ = std::vector<std::atomic<uint64_t>>(n);
  for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
}

void LatencyHistogram::record(int64_t value) {
  if (value < 0) value = 0;  // 时钟回拨等异常样本按 0 处理, 不让直方图失真

  size_t idx = static_cast<size_t>(value / cfg_.bucket_width);
  if (idx >= buckets_.size()) idx = buckets_.size() - 1;
  buckets_[idx].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_.fetch_add(value, std::memory_order_relaxed);

  // min/max 用 CAS 循环, 无锁且不会丢更新
  int64_t cur = min_.load(std::memory_order_relaxed);
  while (value < cur && !min_.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
  }
  cur = max_.load(std::memory_order_relaxed);
  while (value > cur && !max_.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
  }
}

void LatencyHistogram::reset() {
  for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
  count_.store(0, std::memory_order_relaxed);
  sum_.store(0, std::memory_order_relaxed);
  min_.store(INT64_MAX, std::memory_order_relaxed);
  max_.store(0, std::memory_order_relaxed);
}

LatencyHistogram::Snapshot LatencyHistogram::snapshot() const {
  Snapshot s;
  s.count = count_.load(std::memory_order_relaxed);
  if (s.count == 0) return s;

  s.min = min_.load(std::memory_order_relaxed);
  if (s.min == INT64_MAX) s.min = 0;
  s.max = max_.load(std::memory_order_relaxed);
  s.mean = static_cast<double>(sum_.load(std::memory_order_relaxed)) / static_cast<double>(s.count);

  // 分位数: 找累计计数刚好越过目标值的桶, 用桶下界作为估计(保守、不会高报)
  const auto percentile = [this](double p) -> int64_t {
    const uint64_t total = count_.load(std::memory_order_relaxed);
    const uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(total)));
    uint64_t acc = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      acc += buckets_[i].load(std::memory_order_relaxed);
      if (acc >= target) return static_cast<int64_t>(i) * cfg_.bucket_width;
    }
    return static_cast<int64_t>(buckets_.size() - 1) * cfg_.bucket_width;
  };
  s.p50 = percentile(0.50);
  s.p95 = percentile(0.95);
  s.p99 = percentile(0.99);
  return s;
}

std::string LatencyHistogram::to_string(const std::string& name) const {
  const Snapshot s = snapshot();
  char buf[512];
  if (s.count == 0) {
    std::snprintf(buf, sizeof(buf), "%-22s (无样本)", name.c_str());
    return buf;
  }
  std::snprintf(buf, sizeof(buf),
                "%-22s n=%-6llu p50=%lld%s p95=%lld%s p99=%lld%s max=%lld%s mean=%.1f%s",
                name.c_str(), static_cast<unsigned long long>(s.count),
                static_cast<long long>(s.p50), cfg_.unit.c_str(),
                static_cast<long long>(s.p95), cfg_.unit.c_str(),
                static_cast<long long>(s.p99), cfg_.unit.c_str(),
                static_cast<long long>(s.max), cfg_.unit.c_str(), s.mean,
                cfg_.unit.c_str());
  return buf;
}

}  // namespace sublens
