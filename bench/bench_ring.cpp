/// @file bench_ring.cpp  无锁环形队列基准
///
/// 测的是音频采集线程 → VAD 线程这条通道的极限:
///   - 单次 push+pop 的原子操作开销(决定每帧 20ms 预算里能剩多少);
///   - 双线程真实搬运吞吐(生产者全速写, 消费者全速读);
///   - 块从入队到出队的延迟分布(这是音频链路引入的附加延迟)。

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include "bench.hpp"
#include "sublens/ring_buffer.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sublens_bench {
namespace {

constexpr size_t kBlockSamples = 320;  ///< 20ms @16kHz

/// 一个"音频块"的传输单元(时间戳 + 320 个样本)
struct Block {
  int64_t ts_ns = 0;
  std::array<int16_t, kBlockSamples> pcm{};
};

}  // namespace

BenchReport bench_ring() {
  using namespace sublens;
  BenchReport r;
  r.id = "ring";
  r.title = "无锁环形缓冲(SPSC)";

  std::string md;
  md += "### 1. 单次 push + pop 的原子开销\n\n";

  {
    SpscRing<int16_t, 32768> ring;
    const int kIters = 5'000'000;
    int16_t v = 7;
    // 让每次循环的数据依赖上一次 pop 的结果, 防止编译器把 push/pop 对
    // 优化掉(否则测出来的 1ns 是假数字)。
    int64_t acc = 0;
    Stopwatch sw;
    for (int i = 0; i < kIters; ++i) {
      ring.push(&v, 1);
      ring.pop(&v, 1);
      acc += v;
      v = static_cast<int16_t>(acc & 1);
    }
    const double ns_per = static_cast<double>(sw.ns()) / kIters;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "| 指标 | 数值 |\n|---|---|\n"
                  "| 单次 push+pop | %.1f ns |\n"
                  "| 占 20ms 帧预算 | %.4f%% |\n"
                  "| 累计校验和(防止优化) | %lld |\n\n",
                  ns_per, ns_per / 20'000'000.0 * 100.0, static_cast<long long>(acc));
    md += buf;
  }

  md += "### 2. 双线程搬运吞吐 + 块延迟\n\n";

  {
    // 128 个块的容量 = 2.56 秒音频; 生产者全速写, 消费者全速读
    SpscRing<Block, 128> ring;
    const int kBlocks = 200'000;  // 200k * 20ms = 4000 秒音频
    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> pushed{0};
    std::atomic<uint64_t> dropped{0};
    std::vector<int64_t> latencies_us;
    latencies_us.reserve(kBlocks);

    Stopwatch total;
    std::thread producer([&] {
      for (int i = 0; i < kBlocks; ++i) {
        Block b;
        b.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
        while (ring.push(&b, 1) == 0) {
          // 真实采集线程在这里会丢帧 —— 记录一下, 这里只是让出 CPU 重试
          dropped.fetch_add(1, std::memory_order_relaxed);
        }
        pushed.fetch_add(1, std::memory_order_relaxed);
      }
      producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
      Block b;
      while (true) {
        if (ring.pop(&b, 1) == 0) {
          if (producer_done.load(std::memory_order_acquire) && ring.empty()) break;
          continue;
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        latencies_us.push_back((now - b.ts_ns) / 1000);
      }
    });

    producer.join();
    consumer.join();
    const double secs = static_cast<double>(total.ns()) / 1e9;
    const double msamples = static_cast<double>(pushed.load()) * kBlockSamples / 1e6;

    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "| 指标 | 数值 |\n|---|---|\n"
                  "| 搬运块数 | %llu |\n"
                  "| 总音频时长(等效) | %.0f 秒 |\n"
                  "| 耗时 | %.3f 秒 |\n"
                  "| 吞吐 | %.1f Msamples/s |\n"
                  "| 生产者满队列重试次数 | %llu |\n"
                  "| 块延迟 p50 | %lld us |\n"
                  "| 块延迟 p95 | %lld us |\n"
                  "| 块延迟 p99 | %lld us |\n"
                  "| 块延迟 max | %lld us |\n\n",
                  static_cast<unsigned long long>(pushed.load()),
                  static_cast<double>(pushed.load()) * 20.0 / 1000.0, secs, msamples,
                  static_cast<unsigned long long>(dropped.load()),
                  static_cast<long long>(percentile(latencies_us, 0.50)),
                  static_cast<long long>(percentile(latencies_us, 0.95)),
                  static_cast<long long>(percentile(latencies_us, 0.99)),
                  static_cast<long long>(percentile(latencies_us, 1.0)));
    md += buf;
  }

  md += "> 结论: 单次入队出队开销在几十纳秒量级, 对 20ms 帧预算是万分之几; "
        "双线程吞吐远超实时需求(16kHz 单声道 = 0.016 Msamples/s), 这条通道不是瓶颈。\n";
  r.markdown = md;
  return r;
}

}  // namespace sublens_bench
