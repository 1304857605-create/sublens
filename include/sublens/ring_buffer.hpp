#pragma once
/// @file ring_buffer.hpp
/// @brief 无锁单生产者/单消费者环形缓冲(SPSC), 音频采集线程 → VAD 线程的传输通道。
///
/// 为什么自己写而不是用 mutex + queue:
///   这条通道每秒要过 50 个 20ms 音频帧, 而且音频线程一旦被调度器抢占,
///   麦克风回调就会丢帧。SPSC 只用到两条 cache line 上的 atomic,
///   生产者消费者各写各的, 永远不会互相阻塞, 也永远不会在内核里睡眠。
///
/// 用法约束:
///   - 只能有一个生产者线程调用 push*, 一个消费者线程调用 pop*;
///   - Capacity 必须是 2 的幂(用掩码取模, 比 % 快);
///   - push/pop 支持一次搬运多帧, 避免每 20ms 一次系统调用级别的开销。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdint>

namespace sublens {

template <typename T, size_t Capacity>
class SpscRing {
  static_assert(Capacity > 0, "Capacity 必须大于 0");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity 必须是 2 的幂");

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  /// 生产者: 写入最多 n 个元素, 返回实际写入个数。
  ///
  /// 契约: 每个"想写但没写进去"的元素都会计入 dropped()。调用方如果要求
  /// 零丢失, 要么先用 space() 限制本次写入量, 要么检查返回值把剩余部分重试。
  size_t push(const T* src, size_t n) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t free = Capacity - (head - tail);
    const size_t count = n < free ? n : free;
    if (count < n) dropped_.fetch_add(n - count, std::memory_order_relaxed);
    if (count == 0) return 0;
    copy_in(head, src, count);
    head_.store(head + count, std::memory_order_release);
    return count;
  }

  /// 消费者: 读取最多 n 个元素, 返回实际读取个数。
  size_t pop(T* dst, size_t n) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t avail = head - tail;
    const size_t count = n < avail ? n : avail;
    if (count == 0) return 0;
    copy_out(tail, dst, count);
    tail_.store(tail + count, std::memory_order_release);
    return count;
  }

  /// 当前可读元素个数(近似值, 只有消费者一侧是准确的)。
  size_t size() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }
  bool empty() const { return size() == 0; }

  /// 当前可写元素个数(近似值, 只有生产者一侧是准确的)。
  size_t space() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return Capacity - (head - tail);
  }

  static constexpr size_t capacity() { return Capacity; }

  /// 因为写满而丢弃的元素个数(用于观测是否过载)。
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  void clear() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
  }

 private:
  void copy_in(size_t pos, const T* src, size_t count) {
    const size_t start = pos & (Capacity - 1);
    const size_t first = std::min(count, Capacity - start);
    if constexpr (std::is_trivially_copyable_v<T>) {
      std::memcpy(&buf_[start], src, first * sizeof(T));
      if (count > first) std::memcpy(&buf_[0], src + first, (count - first) * sizeof(T));
    } else {
      for (size_t i = 0; i < first; ++i) buf_[start + i] = src[i];
      for (size_t i = first; i < count; ++i) buf_[i - first] = src[i];
    }
  }

  void copy_out(size_t pos, T* dst, size_t count) {
    const size_t start = pos & (Capacity - 1);
    const size_t first = std::min(count, Capacity - start);
    if constexpr (std::is_trivially_copyable_v<T>) {
      std::memcpy(dst, &buf_[start], first * sizeof(T));
      if (count > first) std::memcpy(dst + first, &buf_[0], (count - first) * sizeof(T));
    } else {
      for (size_t i = 0; i < first; ++i) dst[i] = buf_[start + i];
      for (size_t i = first; i < count; ++i) dst[i] = buf_[i - first];
    }
  }

  std::array<T, Capacity> buf_{};
  // 生产者只写 head_, 消费者只写 tail_; 各占一条 cache line, 避免伪共享。
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  alignas(64) std::atomic<uint64_t> dropped_{0};
};

}  // namespace sublens
