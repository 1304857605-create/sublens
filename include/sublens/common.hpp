#pragma once
/// @file common.hpp
/// @brief 公共基础设施: 单调时钟、日志、延迟直方图。
///
/// 这些工具只依赖 C++ 标准库, 因此在整个引擎(含 Android NDK)里通用。
/// 设计取向: 热路径只做原子操作, 不做堆分配、不拿互斥锁。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace sublens {

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------
using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

/// 单调递增的毫秒数。只用于测量延迟, 不受系统时间/时区调整影响。
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             SteadyClock::now().time_since_epoch())
      .count();
}

/// 从某个时间点到现在经过的毫秒数。
inline int64_t elapsed_ms(TimePoint t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t)
      .count();
}

// ---------------------------------------------------------------------------
// 日志 (输出到 stderr, 保证 stdout 只留给 HUD 画面)
// ---------------------------------------------------------------------------
enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

void log_write(LogLevel level, const std::string& msg);  // 线程安全
void set_log_level(LogLevel level);
LogLevel log_level();

/// RAII 日志行: `SL_LOG(INFO) << "x=" << x;`
class LogLine {
 public:
  explicit LogLine(LogLevel level) : level_(level) {}
  ~LogLine() {
    if (level_ >= sublens::log_level()) sublens::log_write(level_, os_.str());
  }
  template <typename T>
  LogLine& operator<<(const T& v) {
    os_ << v;
    return *this;
  }
  LogLine& operator<<(std::ostream& (*manip)(std::ostream&)) {
    os_ << manip;
    return *this;
  }

 private:
  LogLevel level_;
  std::ostringstream os_;
};

#define SL_LOG(lv) ::sublens::LogLine(::sublens::LogLevel::k##lv)

// ---------------------------------------------------------------------------
// 延迟直方图
// ---------------------------------------------------------------------------
/// 定宽桶直方图, 用于统计"毫秒/微秒"级延迟分布。
///
/// 为什么不用 vector 存原始样本: 流水线是长跑程序, 要的是 O(1) 记录、
/// 固定内存、无锁读取, p50/p95/p99 够用, 不需要精确的每个样本。
///
/// 桶宽 bucket_width, 共 (max_value / bucket_width + 2) 个桶:
///   第 i 个桶统计 [i*width, (i+1)*width) 的样本, 溢出样本进最后一个桶。
class LatencyHistogram;

/// 配置结构体统一放在命名空间作用域:
/// 嵌套类 + 默认成员初始化器 + 类内默认实参/成员 会触发
/// "default member initializer required before the end of its enclosing class",
/// 所以把它们提到外面, 在类里用 `using Config = HistogramConfig` 保留短写法。
struct HistogramConfig {
  int64_t bucket_width = 1;   ///< 桶宽
  int64_t max_value = 10000;  ///< 最大值(超出计入溢出桶)
  std::string unit = "ms";    ///< 仅用于展示
};

class LatencyHistogram {
 public:
  using Config = HistogramConfig;

  struct Snapshot {
    uint64_t count = 0;
    int64_t min = 0;
    int64_t p50 = 0;
    int64_t p95 = 0;
    int64_t p99 = 0;
    int64_t max = 0;
    double mean = 0.0;
  };

  explicit LatencyHistogram(Config cfg = {});
  LatencyHistogram(const LatencyHistogram&) = delete;
  LatencyHistogram& operator=(const LatencyHistogram&) = delete;

  void record(int64_t value);
  void reset();

  Snapshot snapshot() const;

  /// 单行文本: `count=… p50=… p95=… p99=… max=…`
  std::string to_string(const std::string& name = "") const;

  const Config& config() const { return cfg_; }

 private:
  int bucket_count() const { return static_cast<int>(buckets_.size()); }

  Config cfg_;
  std::vector<std::atomic<uint64_t>> buckets_;
  std::atomic<uint64_t> count_{0};
  std::atomic<int64_t> sum_{0};
  std::atomic<int64_t> min_{INT64_MAX};
  std::atomic<int64_t> max_{0};
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
/// UTF-8 字符串的"显示宽度"近似(中文按 2 列算), HUD 排版要用。
inline size_t display_width(const std::string& s) {
  size_t width = 0;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++width;
      ++i;
    } else if ((c >> 5) == 0x6) {  // 2 字节: 拉丁扩展/希腊等
      ++width;
      i += 2;
    } else if ((c >> 4) == 0xE) {  // 3 字节: 中文/日文/韩文
      width += 2;
      i += 3;
    } else {  // 4 字节: emoji 等
      width += 2;
      i += 4;
    }
  }
  return width;
}

/// 把秒数格式化成 `12.34s`
inline std::string format_seconds(double s) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(2);
  os << s << "s";
  return os.str();
}

}  // namespace sublens
