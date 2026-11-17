#pragma once
/// @file pipeline.hpp
/// @brief 流水线编排: 音频 → VAD/端点 → ASR → 稳定对齐 → 翻译 → HUD。
///
/// 线程模型(4 条线程, 热路径无锁):
///
///   [采集线程] --20ms PCM--> SpscRing
///        |
///   [VAD 线程] 取帧 → EnergyVad → UtteranceDetector → 语句 → ASR.submit()
///        |
///   [ASR 线程] (后端内部) → AsrPartial 回调 → TranslationScheduler
///        |                                          |
///   [HUD 线程] 固定 fps 读 HudState.snapshot() → 渲染
///                     ↑
///            TranslationScheduler → MtBackend(内部线程) → HudUpdate
///
/// 延迟测量口径(全部用单调时钟, 单位 ms):
///   speech_end_to_hud: 这句话"语音结束"→ 译文第一次出现在眼镜上的时间差。
///   这是唯一有意义的端到端指标, 也是本项目所有优化的目标函数。

#include <cstdint>
#include <memory>
#include <string>

#include "sublens/asr.hpp"
#include "sublens/audio_source.hpp"
#include "sublens/common.hpp"
#include "sublens/hud.hpp"
#include "sublens/mt.hpp"

namespace sublens {

struct PipelineConfig {
  std::string name = "sublens-demo";
  int hud_fps = 20;              ///< HUD 刷新率(眼镜上 15~24 足够)
  int target_latency_ms = 700;   ///< 目标端到端延迟(用于统计达标率)
  bool realtime_pacing = true;   ///< true=按真实时间跑(演示); false=全速(基准)
  int flush_grace_ms = 2000;     ///< 音频结束后等待尾包的宽限时间
  bool verbose = true;
  // 调度策略(透传给 TranslationScheduler, 便于基准测试做 A/B 对比)
  bool draft_enabled = true;      ///< 是否启用"草稿行"推测翻译
  int draft_throttle_ms = 150;    ///< 草稿最小间隔
};

/// 一次运行的统计结果。所有延迟直方图的单位见字段注释。
struct PipelineStats {
  // --- 计数 ---
  uint64_t audio_frames = 0;
  uint64_t utterances = 0;
  uint64_t asr_partials = 0;
  uint64_t commits = 0;
  uint64_t drafts = 0;
  uint64_t hud_frames = 0;
  uint64_t hud_revisions = 0;   ///< 草稿行整行替换次数(稳定性指标)
  uint64_t hud_drafts_dropped = 0;  ///< 草稿行整行替换被 HUD 节流丢弃的次数(同上, 观察抖动)
  uint64_t dropped_samples = 0;
  uint64_t stale_drafts = 0;

  // --- 时间 ---
  double elapsed_s = 0;         ///< 墙钟耗时
  double audio_s = 0;           ///< 音频总时长
  double realtime_factor = 0;   ///< audio_s / elapsed_s

  // --- 延迟(ms) ---
  LatencyHistogram::Snapshot speech_end_to_first_partial;
  LatencyHistogram::Snapshot speech_end_to_first_commit;
  LatencyHistogram::Snapshot speech_end_to_hud;   ///< ★ 端到端
  LatencyHistogram::Snapshot mt_roundtrip;
  LatencyHistogram::Snapshot utterance_len;
  // --- 开销(us) ---
  LatencyHistogram::Snapshot vad_frame;

  /// 对齐的纯文本表, 直接打印
  std::string to_table() const;
  /// Markdown 表格, 直接粘进 docs/BENCHMARK.md
  std::string to_markdown() const;
};

/// 流水线本体。依赖全部由外部注入(方便替换成模拟器/真模型/空渲染器)。
class Pipeline {
 public:
  struct Deps {
    std::unique_ptr<IAudioSource> audio;
    std::unique_ptr<AsrBackend> asr;
    std::unique_ptr<MtBackend> mt;
    std::unique_ptr<HudRenderer> hud;
  };

  Pipeline(PipelineConfig cfg, Deps deps);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  /// 阻塞运行, 直到音频流结束、所有尾包处理完、HUD 收尾。
  void run();

  /// 从其它线程请求停止(演示时按 Ctrl+C 会走到这里)
  void request_stop();

  /// 运行结束后取统计(run() 返回前已全部填好)
  PipelineStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublens
