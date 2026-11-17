/// @file bench_vad.cpp  VAD / 端点检测基准
///
/// 两个数字最关键:
///   1) 单帧(20ms 音频)处理耗时 —— 端侧 CPU 预算就是靠它守住的;
///   2) 端点判定误差 —— 决定"说完到送云端"这一步的固有延迟,
///      误差大意味着不该切的地方切了, 或者该切的地方拖了。

#include <thread>
#include <vector>

#include "bench.hpp"
#include "sublens/audio_source.hpp"
#include "sublens/script.hpp"
#include "sublens/vad.hpp"

namespace sublens_bench {
namespace {

/// 用合成音频逐帧喂 VAD, 统计单帧耗时(纳秒)。
///
/// 注意: 帧时间戳必须用**音频时间轴**(第 i 帧 = i*20ms), 不能用墙钟 ——
/// 基准是全速跑的, 墙钟几乎不前进, 端点检测的 hangover 永远不会到期。
Snapshot measure_frame_cost(const sublens::ConversationScript& script, bool use_detector) {
  using namespace sublens;
  ScriptedAudioConfig cfg;
  cfg.script = &script;
  cfg.realtime_pacing = false;  // 基准只看 CPU 成本, 不需要实时节奏
  ScriptedAudioSource src(cfg);
  src.start();

  LatencyHistogram hist({10, 500000, "ns"});
  EnergyVad vad;
  std::vector<Utterance> utts;
  UtteranceDetector detector([&](Utterance&& u) { utts.push_back(std::move(u)); });

  int16_t frame[kFrameSamples];
  int64_t frame_index = 0;
  while (true) {
    const int n = src.read(frame, kFrameSamples);
    if (n < 0) break;
    if (n == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      continue;
    }
    const int64_t audio_ts = (frame_index + 1) * kFrameMs;
    ++frame_index;
    const auto t0 = std::chrono::steady_clock::now();
    if (use_detector) {
      detector.push_frame(frame, n, audio_ts);
    } else {
      vad.process(frame, n);
    }
    const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    hist.record(dt);
  }
  if (use_detector) detector.flush();
  src.stop();
  return hist.snapshot();
}

}  // namespace

BenchReport bench_vad() {
  using namespace sublens;
  BenchReport r;
  r.id = "vad";
  r.title = "VAD / 端点检测";

  const ConversationScript& script = demo_scenario_en2zh();  // 6 句, 句间停顿 550~900ms

  const Snapshot vad_cost = measure_frame_cost(script, false);
  const Snapshot det_cost = measure_frame_cost(script, true);

  std::string md;
  md += "### 1. 单帧处理耗时\n\n";
  md += latency_table("ns/帧(20ms 音频)", {{"EnergyVad::process", vad_cost},
                                           {"UtteranceDetector::push_frame(含 VAD)", det_cost}});
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "\n| 指标 | 数值 |\n|---|---|\n"
                "| 单帧耗时占 20ms 预算(p95) | %.4f%% |\n"
                "| 1 分钟音频总 CPU 时间 | %.2f ms |\n\n",
                static_cast<double>(det_cost.p95) / 20'000'000.0 * 100.0,
                static_cast<double>(det_cost.mean) * 3000.0 / 1e6);
  md += buf;

  // 端点精度: 用端点检测器跑一遍合成音频, 和脚本真值对比
  md += "### 2. 端点检测精度(合成音频 vs 脚本真值)\n\n";
  {
    ScriptedAudioConfig acfg;
    acfg.script = &script;
    acfg.realtime_pacing = false;
    ScriptedAudioSource src(acfg);
    src.start();

    std::vector<Utterance> utts;
    UtteranceDetector detector([&](Utterance&& u) { utts.push_back(std::move(u)); });
    int16_t frame[kFrameSamples];
    int64_t frame_index = 0;
    while (true) {
      const int n = src.read(frame, kFrameSamples);
      if (n < 0) break;
      if (n == 0) continue;
      // 音频域时间戳: 全速跑时墙钟几乎不动, 必须用音频时间轴, 否则
      // hangover(320ms) 永远到不了, 端点会被 max_utterance 强行切断。
      detector.push_frame(frame, n, (frame_index + 1) * kFrameMs);
      ++frame_index;
    }
    detector.flush();
    src.stop();

    std::vector<int64_t> errors_ms;
    for (size_t i = 0; i < utts.size() && i < script.utterances.size(); ++i) {
      const int64_t truth = script.utterances[i].speech_ms();
      errors_ms.push_back(std::llabs(static_cast<long long>(utts[i].speech_duration_ms) - truth));
    }
    std::snprintf(buf, sizeof(buf),
                  "| 指标 | 数值 |\n|---|---|\n"
                  "| 脚本句数 | %zu |\n"
                  "| 检出句数 | %zu |\n"
                  "| 语音时长误差 p50 | %lld ms |\n"
                  "| 语音时长误差 max | %lld ms |\n"
                  "| 被丢弃的过短片段 | %llu |\n\n",
                  script.utterances.size(), utts.size(),
                  static_cast<long long>(percentile(errors_ms, 0.5)),
                  static_cast<long long>(percentile(errors_ms, 1.0)),
                  static_cast<unsigned long long>(detector.dropped_short()));
    md += buf;
    md += "> 说明: 句间静音 ≥ 550ms 而 hangover=320ms, 因此每句都能独立切出; "
          "误差主要来自句首 attack 段被 VAD 判为非语音的几个帧。\n";
  }

  r.markdown = md;
  return r;
}

}  // namespace sublens_bench
