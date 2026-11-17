/// @file pipeline.cpp  4 线程流水线编排: 采集 → VAD/端点 → ASR → 稳定调度 → HUD
///
/// 线程模型见 pipeline.hpp。这里额外说明三个实现层面的取舍:
///
/// 1) 反压而不是丢帧: 采集线程遇到环形缓冲满时, 把"已写进去的部分"记下来,
///    剩余样本 sleep 1ms 后重试。丢 20ms 音频等于吞掉半个字, 对字幕是硬伤;
///    宁可让采集线程慢下来(音频源本身也是按真实节奏产出的)。
/// 2) 收尾顺序固定: 采集 EOF → VAD 线程 detector.flush() → asr.flush() →
///    mt.flush() → 补渲染几帧 → 停 HUD。必须严格按这个顺序, 否则最后半句的
///    回调可能落在 HUD 线程退出之后, 统计和画面都会缺一块。
/// 3) 统计全部用原子/无锁直方图记录, 热路径上不拿锁; 只有 HudState 的
///    极短临界区是互斥的(它内部已经保证)。

#include "sublens/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sublens/common.hpp"
#include "sublens/ring_buffer.hpp"
#include "sublens/sched.hpp"
#include "sublens/vad.hpp"

namespace sublens {
namespace {

using Clock = SteadyClock;

/// 环形缓冲容量: 32768 样本 ≈ 2 秒音频。取 2 的幂(SPSC 用掩码取模),
/// 2 秒足以吸收桌面调度抖动, 又不会把"管道里积压的音频"藏得太久。
constexpr size_t kRingCapacity = 32768;

// ---------------------------------------------------------------------------
// 文本/表格小工具
// ---------------------------------------------------------------------------

/// 按显示宽度补空格(中文算 2 列), 用于纯文本表格对齐。
std::string pad_display(const std::string& s, size_t width) {
  const size_t w = display_width(s);
  if (w >= width) return s;
  return s + std::string(width - w, ' ');
}

std::string fmt_fixed(double v, int precision) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(precision);
  os << v;
  return os.str();
}

/// 一行直方图: `n=…  p50=…  p95=…  p99=…  max=…  mean=…`。
std::string fmt_hist(const LatencyHistogram::Snapshot& h, const char* unit) {
  if (h.count == 0) return "(无样本)";
  std::ostringstream os;
  os << "n=" << h.count << "  p50=" << h.p50 << unit << "  p95=" << h.p95 << unit
     << "  p99=" << h.p99 << unit << "  max=" << h.max << unit
     << "  mean=" << fmt_fixed(h.mean, 1) << unit;
  return os.str();
}

/// 两列表格的公共渲染: 先算两列最大显示宽度, 再逐行补空格。
/// 不用固定宽度是因为指标名是中英混排, 固定宽度要么中文错位要么英文浪费空间。
std::string render_two_column_table(const std::string& title,
                                    const std::vector<std::pair<std::string, std::string>>& rows) {
  size_t key_w = display_width("指标");
  size_t val_w = display_width("值");
  for (const auto& [k, v] : rows) {
    key_w = std::max(key_w, display_width(k));
    val_w = std::max(val_w, display_width(v));
  }
  const std::string sep(key_w + 2 + val_w, '-');

  std::ostringstream os;
  os << title << '\n' << sep << '\n';
  os << pad_display("指标", key_w) << "  " << "值" << '\n';
  os << sep << '\n';
  for (const auto& [k, v] : rows) {
    os << pad_display(k, key_w) << "  " << v << '\n';
  }
  os << sep;
  return os.str();
}

/// 把 PipelineStats 拍平成表格行(纯文本表与 Markdown 表共用, 保证口径一致)。
std::vector<std::pair<std::string, std::string>> stats_rows(const PipelineStats& s) {
  std::vector<std::pair<std::string, std::string>> rows;
  rows.reserve(24);

  rows.emplace_back("音频帧数 (20ms/帧)", std::to_string(s.audio_frames));
  rows.emplace_back("音频时长", format_seconds(s.audio_s));
  rows.emplace_back("墙钟耗时", format_seconds(s.elapsed_s));
  rows.emplace_back("实时系数 (音频/墙钟)", fmt_fixed(s.realtime_factor, 2) + "x");
  rows.emplace_back("语句数 (VAD 切句)", std::to_string(s.utterances));
  rows.emplace_back("ASR partial 数", std::to_string(s.asr_partials));
  rows.emplace_back("正式提交 / 草稿更新", std::to_string(s.commits) + " / " + std::to_string(s.drafts));
  rows.emplace_back("HUD 帧数", std::to_string(s.hud_frames));
  rows.emplace_back("HUD 草稿整行替换", std::to_string(s.hud_revisions));
  rows.emplace_back("HUD 草稿节流丢弃", std::to_string(s.hud_drafts_dropped));
  rows.emplace_back("丢样本 / 陈旧草稿",
                    std::to_string(s.dropped_samples) + " / " + std::to_string(s.stale_drafts));
  rows.emplace_back("speech_end_to_first_partial", fmt_hist(s.speech_end_to_first_partial, "ms"));
  rows.emplace_back("speech_end_to_first_commit", fmt_hist(s.speech_end_to_first_commit, "ms"));
  rows.emplace_back("speech_end_to_hud (端到端)", fmt_hist(s.speech_end_to_hud, "ms"));
  rows.emplace_back("mt_roundtrip", fmt_hist(s.mt_roundtrip, "ms"));
  rows.emplace_back("utterance_len", fmt_hist(s.utterance_len, "ms"));
  rows.emplace_back("vad_frame (单帧开销)", fmt_hist(s.vad_frame, "us"));
  return rows;
}

// ---------------------------------------------------------------------------
// MT 往返计时包装
// ---------------------------------------------------------------------------
/// MtBackend 的回调只有 emit_ms, 拿不到"提交时刻", 而 stats 需要 mt_roundtrip。
/// 所以在调度器和真实后端之间夹一层: submit 时记时间, 结果回来时算差值再转发。
/// 只加一次 map 查找, 相对模拟后端/网络本身的耗时可以忽略。
class TimedMt final : public MtBackend {
 public:
  using RoundtripFn = std::function<void(int64_t)>;

  TimedMt(MtBackend& inner, RoundtripFn on_roundtrip)
      : inner_(inner), on_roundtrip_(std::move(on_roundtrip)) {}

  void set_callback(Callback cb) override {
    user_cb_ = std::move(cb);
    inner_.set_callback([this](const MtResult& r) {
      int64_t submitted_ms = 0;
      const uint64_t key = key_of(r.utt_id, r.draft);
      {
        std::lock_guard<std::mutex> lock(mu_);
        // 同一句可能有多条请求在飞(前缀重译 + 定稿重译), 必须按 FIFO 配对,
        // 否则会把"后来请求的提交时刻"配给"先返回的结果", 往返时延被算小。
        const auto it = pending_.find(key);
        if (it != pending_.end() && !it->second.empty()) {
          submitted_ms = it->second.front();
          it->second.pop_front();
          if (it->second.empty()) pending_.erase(it);
        }
      }
      // 先记统计再转发: 保证统计不会因为下游抛异常/退出而丢样本
      if (submitted_ms > 0 && on_roundtrip_) {
        const int64_t done_ms = r.emit_ms > 0 ? r.emit_ms : now_ms();
        on_roundtrip_(done_ms - submitted_ms);
      }
      if (user_cb_) user_cb_(r);
    });
  }

  bool start() override { return inner_.start(); }

  void submit(MtRequest req) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_[key_of(req.utt_id, req.draft)].push_back(now_ms());
    }
    inner_.submit(std::move(req));
  }

  void flush() override { inner_.flush(); }
  void stop() override { inner_.stop(); }
  const char* name() const override { return inner_.name(); }

 private:
  /// utt_id + draft 拼成一个 key: 同一句的正式翻译和草稿翻译可能同时在飞。
  static uint64_t key_of(uint64_t utt_id, bool draft) { return (utt_id << 1) | (draft ? 1u : 0u); }

  MtBackend& inner_;
  RoundtripFn on_roundtrip_;
  Callback user_cb_;
  std::mutex mu_;
  std::unordered_map<uint64_t, std::deque<int64_t>> pending_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Pipeline::Impl
// ---------------------------------------------------------------------------

struct Pipeline::Impl {
  Impl(PipelineConfig c, Deps d);
  ~Impl();

  void run();
  void request_stop();
  PipelineStats collect_stats() const;

  // --- 线程体 ---
  void capture_loop();
  void vad_loop();
  void hud_loop();

  // --- 回调(分别跑在 VAD / ASR / MT 线程上) ---
  void on_utterance(Utterance&& u);
  void on_asr_partial(const AsrPartial& p);
  void on_hud_update(const HudUpdate& u);

  PipelineConfig cfg;
  Deps deps;

  // 依赖注入后才构造的编排件(在 run() 里装配, 方便处理 start() 失败)
  std::unique_ptr<TimedMt> timed_mt;
  std::unique_ptr<TranslationScheduler> scheduler;
  std::unique_ptr<UtteranceDetector> detector;

  SpscRing<int16_t, kRingCapacity> ring;
  HudState hud_state;

  std::thread capture_thread;
  std::thread vad_thread;
  std::thread hud_thread;

  // --- 线程间信号 ---
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> audio_eof{false};
  std::atomic<bool> vad_done{false};
  std::atomic<bool> hud_stop{false};
  std::atomic<bool> ran{false};

  // --- 计数(热路径只做原子自增) ---
  std::atomic<uint64_t> audio_frames{0};
  std::atomic<uint64_t> total_samples{0};
  std::atomic<uint64_t> utterances{0};
  std::atomic<uint64_t> asr_partials{0};
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> drafts{0};
  std::atomic<uint64_t> hud_frames{0};
  std::atomic<uint64_t> dropped_samples{0};

  // 首件事件去重: 同一个 utt 只记一次.first_partial / first_commit
  std::atomic<uint64_t> last_partial_utt{0};
  std::atomic<uint64_t> last_commit_utt{0};

  // VAD 音频域时钟(只在 VAD 线程读写; on_utterance 在 push_frame/flush 内同步回调)
  int64_t audio_ms_now = 0;

  // --- 延迟直方图(内部全部是原子操作, 可从任意线程 record) ---
  LatencyHistogram hist_first_partial{HistogramConfig{1, 10000, "ms"}};
  LatencyHistogram hist_first_commit{HistogramConfig{1, 10000, "ms"}};
  LatencyHistogram hist_e2e{HistogramConfig{1, 10000, "ms"}};
  LatencyHistogram hist_mt_roundtrip{HistogramConfig{1, 10000, "ms"}};
  LatencyHistogram hist_utterance_len{HistogramConfig{1, 20000, "ms"}};
  LatencyHistogram hist_vad_frame{HistogramConfig{1, 20000, "us"}};

  double wall_elapsed_s = 0.0;  ///< run() 结束前写入, 之后只读
};

Pipeline::Impl::Impl(PipelineConfig c, Deps d) : cfg(std::move(c)), deps(std::move(d)) {}

Pipeline::Impl::~Impl() {
  // 兜底: 如果 run() 中途异常/提前返回, 线程可能还活着, 不能让它们跑出对象生命周期
  stop_requested.store(true, std::memory_order_relaxed);
  if (capture_thread.joinable()) capture_thread.join();
  if (vad_thread.joinable()) vad_thread.join();
  hud_stop.store(true, std::memory_order_relaxed);
  if (hud_thread.joinable()) hud_thread.join();
}

void Pipeline::Impl::run() {
  if (ran.exchange(true)) return;  // run() 只允许跑一次
  if (!deps.audio || !deps.asr || !deps.mt || !deps.hud) {
    SL_LOG(Error) << "流水线依赖不完整, 拒绝启动(audio/asr/mt/hud 都必须注入)";
    return;
  }

  const auto wall_start = Clock::now();

  // ---- 装配: 先建计时包装与调度器, 再挂回调 ----
  timed_mt = std::make_unique<TimedMt>(*deps.mt, [this](int64_t ms) {
    hist_mt_roundtrip.record(ms);
  });

  SchedulerConfig sched_cfg;
  sched_cfg.draft_enabled = cfg.draft_enabled;        // --no-draft 从这里透传进来
  sched_cfg.draft_throttle_ms = cfg.draft_throttle_ms;
  sched_cfg.target_latency_ms = cfg.target_latency_ms;
  scheduler = std::make_unique<TranslationScheduler>(*timed_mt, sched_cfg);
  scheduler->set_sink([this](const HudUpdate& u) { on_hud_update(u); });
  timed_mt->set_callback([this](const MtResult& r) { scheduler->on_mt_result(r); });
  detector = std::make_unique<UtteranceDetector>(
      [this](Utterance&& u) { on_utterance(std::move(u)); }, DetectorConfig{});
  deps.asr->set_callback([this](const AsrPartial& p) { on_asr_partial(p); });

  if (cfg.verbose) {
    SL_LOG(Info) << "流水线启动: " << cfg.name << " | HUD " << cfg.hud_fps << "fps | 实时节奏="
                 << (cfg.realtime_pacing ? "on" : "off") << " | 草稿=" << (cfg.draft_enabled ? "on" : "off");
  }

  // ---- 启动所有后端与渲染器 ----
  if (!deps.audio->start()) {
    SL_LOG(Error) << "音频源启动失败: " << deps.audio->name();
    return;
  }
  if (!deps.asr->start()) {
    SL_LOG(Error) << "ASR 后端启动失败: " << deps.asr->name();
    deps.audio->stop();
    return;
  }
  if (!timed_mt->start()) {
    SL_LOG(Error) << "MT 后端启动失败: " << deps.mt->name();
    deps.asr->stop();
    deps.audio->stop();
    return;
  }
  deps.hud->start();  // 渲染器失败不致命(降级为不显示), 继续跑统计

  // ---- 三条本进程线程(ASR/MT 的工作线程由后端自己管理) ----
  capture_thread = std::thread(&Impl::capture_loop, this);
  vad_thread = std::thread(&Impl::vad_loop, this);
  hud_thread = std::thread(&Impl::hud_loop, this);

  // ---- 收尾顺序是契约的一部分, 不要调整 ----
  capture_thread.join();  // 1) 音频结束(或 Ctrl+C): 生产者退出
  vad_thread.join();      // 2) VAD 线程排空环形缓冲 + detector.flush() 交出最后半句
  deps.asr->flush();      // 3) 等 ASR 把已提交语句全部吐完(会触发最后的 partial)
  timed_mt->flush();      // 4) 等 MT 把在飞翻译全部回完(会触发最后的 kCommitted)

  // flush 之后回调理论上都已完成, 但回调可能在我们统计的瞬间才进入 sink;
  // 用 flush_grace_ms 作为上限等"提交数 100ms 内不再增长", 保证统计不漏最后一条。
  if (cfg.flush_grace_ms > 0) {
    const int64_t deadline = now_ms() + cfg.flush_grace_ms;
    uint64_t seen = commits.load(std::memory_order_relaxed);
    int64_t stable_since = now_ms();
    while (now_ms() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const uint64_t cur = commits.load(std::memory_order_relaxed);
      if (cur != seen) {
        seen = cur;
        stable_since = now_ms();
      } else if (now_ms() - stable_since >= 100) {
        break;
      }
    }
  }

  // 5) 停 HUD 线程, 再由本线程补渲染 3 帧:
  //    线程已经 join 过, 这里访问渲染器是单线程的; 补帧让最后一句译文在屏上停留一下,
  //    录屏时"收尾"才看得清楚, 而不是停在一句读不完的草稿上。
  hud_stop.store(true, std::memory_order_relaxed);
  if (hud_thread.joinable()) hud_thread.join();
  for (int i = 0; i < 3; ++i) {
    deps.hud->render(hud_state.snapshot(), now_ms());
    hud_frames.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
  }
  deps.hud->stop();  // 恢复光标/颜色, 之后 stdout 才能继续打印统计表

  deps.asr->stop();
  timed_mt->stop();
  deps.audio->stop();

  const auto wall_end = Clock::now();
  wall_elapsed_s =
      std::chrono::duration_cast<std::chrono::microseconds>(wall_end - wall_start).count() / 1e6;

  if (cfg.verbose) {
    const double audio_s = static_cast<double>(total_samples.load()) / kSampleRate;
    const double rtf = wall_elapsed_s > 0.0 ? audio_s / wall_elapsed_s : 0.0;
    SL_LOG(Info) << "流水线结束: 墙钟 " << fmt_fixed(wall_elapsed_s, 2) << "s | 音频 "
                 << fmt_fixed(audio_s, 2) << "s | 实时系数 " << fmt_fixed(rtf, 2)
                 << "x | HUD 草稿替换 " << hud_state.revisions() << " 次 | 节流丢弃 "
                 << hud_state.drafts_dropped() << " 次";
  }
}

void Pipeline::Impl::request_stop() {
  stop_requested.store(true, std::memory_order_relaxed);
  hud_stop.store(true, std::memory_order_relaxed);  // HUD 线程不必等到音频结束
}

// ---------------------------------------------------------------------------
// 线程体
// ---------------------------------------------------------------------------

void Pipeline::Impl::capture_loop() {
  std::vector<int16_t> frame(kFrameSamples);
  while (!stop_requested.load(std::memory_order_relaxed)) {
    const int n = deps.audio->read(frame.data(), kFrameSamples);
    if (n < 0) {  // -1: 音频流结束
      audio_eof.store(true, std::memory_order_relaxed);
      return;
    }
    if (n == 0) {  // 暂时没数据(真实麦克风会这样): 让出 CPU 再来
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    audio_frames.fetch_add(1, std::memory_order_relaxed);
    total_samples.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

    // 反压: 环形缓冲满时把剩余样本拆成小块重试, 绝不丢样本。
    // (环的 push 返回"这次写进去多少", 没写进去的部分我们记住偏移接着推。)
    size_t written = 0;
    const size_t total = static_cast<size_t>(n);
    while (written < total && !stop_requested.load(std::memory_order_relaxed)) {
      written += ring.push(frame.data() + written, total - written);
      if (written < total) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (written < total) {
      // 只有 Ctrl+C 强行中止时才会走到这里, 其余情况一定写满
      dropped_samples.fetch_add(total - written, std::memory_order_relaxed);
    }
  }
}

void Pipeline::Impl::vad_loop() {
  std::vector<int16_t> pcm(kFrameSamples);
  // VAD 的时间戳必须是**音频域**的: hangover(320ms) 是"这段音频里静了多久"的语义。
  // 全速模式(realtime_pacing=false)下音频比墙钟走得快, 若传 now_ms(), 脚本里 550ms 的
  // 句间静音在墙钟上只有几十毫秒, 端点永远判不出来, 只能靠 max_utterance 强切。
  // 这里按已处理样本数换算音频时长; 交给 ASR 前再换算回墙钟(on_utterance 里做)。
  int64_t processed_samples = 0;
  while (true) {
    const size_t got = ring.pop(pcm.data(), kFrameSamples);
    if (got > 0) {
      processed_samples += static_cast<int64_t>(got);
      audio_ms_now = processed_samples * 1000 / kSampleRate;  // 音频域时间戳
      // VAD 单帧耗时是"端侧开销"的核心指标: 用它验证 320 样本一帧只花几微秒,
      // 这样才敢在眼镜这种低功耗 SoC 上实时跑。
      const auto t0 = Clock::now();
      detector->push_frame(pcm.data(), static_cast<int>(got), audio_ms_now);
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0);
      hist_vad_frame.record(us.count());
      continue;  // 缓冲里可能还有积压, 先不睡
    }
    if (audio_eof.load(std::memory_order_relaxed)) break;  // 生产者结束且已排空
    if (stop_requested.load(std::memory_order_relaxed) && ring.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 音频结束: 把 detector 里还攒着的半句强制收尾(会触发最后一个 Utterance 回调),
  // 否则最后一句永远等不到 hangover 判据, 直接丢掉。
  detector->flush(audio_ms_now);
  vad_done.store(true, std::memory_order_relaxed);
}

void Pipeline::Impl::hud_loop() {
  const int fps = std::max(1, cfg.hud_fps);
  const auto period = std::chrono::milliseconds(1000 / fps);
  auto next = Clock::now();
  while (!hud_stop.load(std::memory_order_relaxed)) {
    // 固定节拍渲染: 眼镜的刷新率是恒定的, 渲染节奏跟着 HUD 走而不是跟着事件走,
    // 这样画面更新频率稳定, 不会因为 ASR 包乱序而忽快忽慢。
    deps.hud->render(hud_state.snapshot(), now_ms());
    hud_frames.fetch_add(1, std::memory_order_relaxed);
    next += period;
    const auto now = Clock::now();
    if (next > now) {
      std::this_thread::sleep_until(next);
    } else {
      next = now;  // 已经落后就不补帧, 避免"追帧"时连续重绘把画面刷花
    }
  }
}

// ---------------------------------------------------------------------------
// 回调
// ---------------------------------------------------------------------------

void Pipeline::Impl::on_utterance(Utterance&& u) {
  utterances.fetch_add(1, std::memory_order_relaxed);
  hist_utterance_len.record(u.speech_duration_ms);

  // detector 给的是音频域时间戳(见 vad_loop), 这里换算回墙钟:
  //   墙钟时刻 ≈ 现在 - (当前音频位置 - 该事件对应的音频位置)
  // 实时模式下音频位置和墙钟同步, 换算前后几乎一样; 全速模式下这样得到的
  // speech_end_ms 是"这一帧音频真正被处理完的墙钟时刻", 端到端延迟仍然按真实时间算。
  const int64_t wall_now = now_ms();
  u.speech_end_ms = wall_now - (audio_ms_now - u.speech_end_ms);
  u.speech_start_ms = wall_now - (audio_ms_now - u.speech_start_ms);

  // submit 是非阻塞的; 语句音频的所有权移交给 ASR 后端
  deps.asr->submit(std::move(u));
}

void Pipeline::Impl::on_asr_partial(const AsrPartial& p) {
  asr_partials.fetch_add(1, std::memory_order_relaxed);

  // 首字延迟: 同一句只记第一个 partial。exchange 返回旧值, 不相等说明是这个 utt 的首次。
  // 只有实时节奏下这个数才有意义: 全速模式下音频几十毫秒就喂完, ASR 的模拟时延
  // 相对音频时间线被放大, 记进去会污染文档数字(见 run() 里的说明)。
  if (cfg.realtime_pacing &&
      last_partial_utt.exchange(p.utt_id, std::memory_order_relaxed) != p.utt_id) {
    hist_first_partial.record(p.emit_ms - p.speech_end_ms);
  }

  // 交给调度器做稳定前缀对齐 + 推测式翻译(sink 会在本线程内同步回调)
  scheduler->on_asr_partial(p);
}

void Pipeline::Impl::on_hud_update(const HudUpdate& u) {
  // 先上屏: HUD 线程下一帧就能看到, 统计慢一点无所谓, 画面不能慢
  hud_state.apply(u);

  if (u.kind == HudUpdate::Kind::kCommitted || u.kind == HudUpdate::Kind::kReplace) {
    commits.fetch_add(1, std::memory_order_relaxed);
    // 端到端口径(见 pipeline.hpp): "语音结束 → 译文**第一次**出现在眼镜上"。
    // 一句话会分很多个片段提交, 如果每个片段都记样本, 同一句就会贡献几十个越来越晚的
    // 样本, p95 会被拖到"整句翻完"的时间 —— 那不是用户感受到的首屏延迟。
    // 所以按 utt 去重, 每句只记第一次 kCommitted; first_commit 保留同值做对照。
    if (last_commit_utt.exchange(u.utt_id, std::memory_order_relaxed) != u.utt_id &&
        cfg.realtime_pacing) {  // 全速模式的延迟口径失真, 只统计计数
      const int64_t latency = u.at_ms - u.speech_end_ms;
      hist_e2e.record(latency);
      hist_first_commit.record(latency);
    }
  } else {
    drafts.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

PipelineStats Pipeline::Impl::collect_stats() const {
  PipelineStats s;
  s.audio_frames = audio_frames.load(std::memory_order_relaxed);
  s.utterances = utterances.load(std::memory_order_relaxed);
  s.asr_partials = asr_partials.load(std::memory_order_relaxed);
  s.commits = commits.load(std::memory_order_relaxed);
  s.drafts = drafts.load(std::memory_order_relaxed);
  s.hud_frames = hud_frames.load(std::memory_order_relaxed);
  s.hud_revisions = hud_state.revisions();
  s.hud_drafts_dropped = hud_state.drafts_dropped();
  s.dropped_samples = dropped_samples.load(std::memory_order_relaxed);

  s.audio_s = static_cast<double>(total_samples.load(std::memory_order_relaxed)) / kSampleRate;
  s.elapsed_s = wall_elapsed_s;
  s.realtime_factor = s.elapsed_s > 0.0 ? s.audio_s / s.elapsed_s : 0.0;

  s.speech_end_to_first_partial = hist_first_partial.snapshot();
  s.speech_end_to_first_commit = hist_first_commit.snapshot();
  s.speech_end_to_hud = hist_e2e.snapshot();
  s.mt_roundtrip = hist_mt_roundtrip.snapshot();
  s.utterance_len = hist_utterance_len.snapshot();
  s.vad_frame = hist_vad_frame.snapshot();

  if (scheduler) s.stale_drafts = scheduler->stats().stale_drafts;
  return s;
}

std::string PipelineStats::to_table() const {
  return render_two_column_table("SubLens 运行统计", stats_rows(*this));
}

std::string PipelineStats::to_markdown() const {
  std::string md = "| 指标 | 值 |\n| --- | --- |\n";
  for (const auto& [k, v] : stats_rows(*this)) {
    md += "| " + k + " | " + v + " |\n";
  }
  return md;
}

// ---------------------------------------------------------------------------
// Pipeline 对外接口: 全部转发到 Impl
// ---------------------------------------------------------------------------

Pipeline::Pipeline(PipelineConfig cfg, Deps deps)
    : impl_(std::make_unique<Impl>(std::move(cfg), std::move(deps))) {}

Pipeline::~Pipeline() = default;

void Pipeline::run() { impl_->run(); }

void Pipeline::request_stop() { impl_->request_stop(); }

PipelineStats Pipeline::stats() const { return impl_->collect_stats(); }

}  // namespace sublens



