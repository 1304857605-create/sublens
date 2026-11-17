/// @file latency_injecting_mt.cpp
/// @brief 时延注入包装: 给任意 MT 后端叠加"云侧往返"时延(base + jitter)。
///
/// 用途: 单机上复现"云端 MT 往返 180~320ms"的场景, 用来验证延迟预算与调度策略。
/// 注入的是**串行等待**, 不占 CPU, 因此不会污染 CPU 开销统计; 内层后端仍然
/// 走它自己的线程模型, 结果原样转发。
///
/// 线程模型:
///   submit() --push--> [包装队列] --工作线程 sleep(base + jitter)--> inner->submit()
///   inner 的回调 --转发--> 上层回调(原样透传 utt_id/text/draft)
///
/// 为什么要多一层队列而不是直接 sleep 后转发: submit() 必须非阻塞(调用方可能
/// 是 HUD/调度线程), 而且睡醒之后要保证投递顺序与提交顺序一致 —— 单工作线程
/// 天然满足 FIFO, 与真实云端"按序返回"的行为一致。

#include "sublens/mt.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace sublens {

struct LatencyInjectingMt::Impl {
  Impl(std::unique_ptr<MtBackend> in, LatencyInjectConfig c)
      : inner(std::move(in)), cfg(c), rng(c.seed) {
    if (inner) {
      // 构造时就挂转发回调: 上层可能先 start() 后 set_callback(), 也可能相反,
      // 这样两种顺序都不会漏掉第一次翻译结果。
      inner->set_callback([this](const MtResult& r) { forward(r); });
    }
  }

  ~Impl() { stop(); }

  std::unique_ptr<MtBackend> inner;
  LatencyInjectConfig cfg;
  std::mt19937 rng;  ///< 只在工作线程里用

  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<MtRequest> queue;
  Callback cb;
  std::thread worker;
  bool started = false;
  bool stopping = false;
  bool busy = false;

  // ---- 生命周期 ---------------------------------------------------------

  void set_callback(Callback c) {
    std::lock_guard<std::mutex> lk(mu);
    cb = std::move(c);
  }

  bool start() {
    if (!inner) {
      SL_LOG(Warn) << "latency-injecting-mt: 内层 MT 后端为空, 拒绝启动";
      return false;
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return false;
      if (started) return true;
    }
    // 先启动内层, 失败就不启动包装线程, 免得队列里的请求永远没人消费
    if (!inner->start()) {
      SL_LOG(Warn) << "latency-injecting-mt: 内层后端 " << inner->name() << " 启动失败";
      return false;
    }
    std::lock_guard<std::mutex> lk(mu);
    if (stopping) return false;
    started = true;
    worker = std::thread([this] { run(); });
    return true;
  }

  void submit(MtRequest req) {
    bool need_start = false;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return;
      need_start = !started;
    }
    if (need_start && !start()) return;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return;
      queue.push_back(std::move(req));
    }
    cv.notify_one();
  }

  void flush() {
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [this] { return stopping || (queue.empty() && !busy); });
    }
    // 包装队列排空只说明"都已转发给 inner", 还要等 inner 自己的队列排空,
    // 否则调用方以为 flush 之后结果都到了, 其实还在内层等待。
    if (inner) inner->flush();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu);
      stopping = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
    {
      std::lock_guard<std::mutex> lk(mu);
      std::deque<MtRequest> dropped;
      dropped.swap(queue);
      busy = false;
    }
    // inner->stop() 会 join 内层工作线程; 在此之前本对象保持存活(unique_ptr 成员)。
    if (inner) inner->stop();
  }

  // ---- 工作线程 ---------------------------------------------------------

  void run() {
    for (;;) {
      MtRequest req;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return stopping || !queue.empty(); });
        if (stopping) return;
        req = std::move(queue.front());
        queue.pop_front();
        busy = true;
      }

      // 注入"云侧往返": base_ms + [0, jitter_ms] 的纯等待
      const int jitter = cfg.jitter_ms > 0
                             ? static_cast<int>(rng() % static_cast<unsigned>(cfg.jitter_ms + 1))
                             : 0;
      const bool alive = sleep_ms(cfg.base_ms + jitter);
      if (alive && inner) inner->submit(std::move(req));  // 透传给内层, 保持 FIFO

      {
        std::lock_guard<std::mutex> lk(mu);
        busy = false;
      }
      cv.notify_all();
      if (!alive) return;
    }
  }

  /// 可被 stop() 打断的等待; 用 wait_until 配循环, 避免假唤醒导致注入时延变短。
  bool sleep_ms(int ms) {
    if (ms <= 0) {
      std::lock_guard<std::mutex> lk(mu);
      return !stopping;
    }
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> lk(mu);
    while (!stopping) {
      if (cv.wait_until(lk, deadline) == std::cv_status::timeout) return !stopping;
    }
    return false;
  }

  /// inner 的回调转发。停止流程中直接丢弃, 保证 stop() 返回后上层不再收到回调。
  void forward(const MtResult& r) {
    Callback c;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return;
      c = cb;
    }
    if (c) c(r);
  }
};

LatencyInjectingMt::LatencyInjectingMt(std::unique_ptr<MtBackend> inner, Config cfg)
    : impl_(std::make_unique<Impl>(std::move(inner), cfg)) {}

LatencyInjectingMt::~LatencyInjectingMt() = default;

void LatencyInjectingMt::set_callback(Callback cb) { impl_->set_callback(std::move(cb)); }

bool LatencyInjectingMt::start() { return impl_->start(); }

void LatencyInjectingMt::submit(MtRequest req) { impl_->submit(std::move(req)); }

void LatencyInjectingMt::flush() { impl_->flush(); }

void LatencyInjectingMt::stop() { impl_->stop(); }

}  // namespace sublens
