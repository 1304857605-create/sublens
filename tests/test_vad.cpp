/// @file test_vad.cpp  VAD 与端点检测测试
///
/// 分两层:
///   - EnergyVad: 只看单帧判决 —— 静音不触发、单帧突发不触发、连续语音才触发、
///     噪声底不被说话时的能量带跑;
///   - UtteranceDetector: 用 demo 脚本跑完整条合成音频, 断言句数、时长误差、
///     pre-roll、hangover、min/max 时长与 flush 收尾。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "sublens/audio_source.hpp"
#include "sublens/script.hpp"
#include "sublens/vad.hpp"
#include "tiny_test.hpp"

namespace {

using sublens::kFrameMs;
using sublens::kFrameSamples;

constexpr int kSamplesPerMs = sublens::kSampleRate / 1000;  ///< 16

double frame_db(const int16_t* pcm, int n) {
  if (!pcm || n <= 0) return -96.0;
  double sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double s = static_cast<double>(pcm[i]);
    sum_sq += s * s;
  }
  const double rms = std::sqrt(sum_sq / n) / 32768.0;
  return rms > 1e-9 ? 20.0 * std::log10(rms) : -96.0;
}

std::vector<int16_t> make_silence(int n = kFrameSamples) {
  return std::vector<int16_t>(static_cast<size_t>(n), 0);
}

/// 150Hz 正弦: 能量稳定, 用来代表"确定是语音"的帧。
std::vector<int16_t> make_tone(double amplitude, int n = kFrameSamples) {
  std::vector<int16_t> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double v = amplitude * std::sin(2.0 * 3.14159265358979323846 * 150.0 * i /
                                          sublens::kSampleRate);
    out[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(v * 32767.0));
  }
  return out;
}

/// 均匀噪声: 用来测噪声底自适应(不能用 std::rand, 见音源实现里的说明)。
std::vector<int16_t> make_noise(double amplitude, unsigned seed, int n = kFrameSamples) {
  std::vector<int16_t> out(static_cast<size_t>(n));
  uint64_t state = seed ? seed : 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < n; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const double u = static_cast<double>(state >> 40) / 8388608.0 - 1.0;  // [-1,1)
    out[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(u * amplitude * 32767.0));
  }
  return out;
}

int64_t abs_diff(int64_t a, int64_t b) { return a > b ? a - b : b - a; }

// ---------------------------------------------------------------------------
// 端点检测辅助: 用合成音源把整个脚本喂给 detector
// ---------------------------------------------------------------------------
struct DetectorRun {
  std::vector<sublens::Utterance> utterances;
  uint64_t emitted = 0;
  uint64_t dropped = 0;
  bool in_speech_at_end = false;
};

DetectorRun run_detector(const sublens::ConversationScript& script,
                         const sublens::DetectorConfig& dcfg) {
  DetectorRun run;
  sublens::UtteranceDetector det(
      [&run](sublens::Utterance&& u) { run.utterances.push_back(std::move(u)); }, dcfg);

  sublens::ScriptedAudioSource::Config scfg;
  scfg.script = &script;
  scfg.realtime_pacing = false;
  sublens::ScriptedAudioSource src(scfg);
  SL_EXPECT(src.start());

  std::vector<int16_t> buf(kFrameSamples);
  int frame_index = 0;
  for (;;) {
    const int n = src.read(buf.data(), kFrameSamples);
    if (n < 0) break;
    det.push_frame(buf.data(), n, static_cast<int64_t>(frame_index + 1) * kFrameMs);
    ++frame_index;
  }
  det.flush(static_cast<int64_t>(frame_index) * kFrameMs);

  run.emitted = det.emitted();
  run.dropped = det.dropped_short();
  run.in_speech_at_end = det.in_speech();
  return run;
}

}  // namespace

// ===========================================================================
// EnergyVad
// ===========================================================================
SL_TEST(EnergyVad_silence_never_triggers) {
  sublens::EnergyVad vad;
  const auto silent = make_silence();
  for (int i = 0; i < 50; ++i) {
    SL_EXPECT(!vad.process(silent.data(), static_cast<int>(silent.size())));
    SL_EXPECT(!vad.in_speech());
  }
  SL_EXPECT(vad.frame_db() < -90.0);  // 数字静音按 dBFS 下限处理, 不能是 NaN
}

SL_TEST(EnergyVad_needs_consecutive_onset_frames) {
  sublens::VadConfig cfg;
  cfg.onset_frames = 3;
  sublens::EnergyVad vad(cfg);
  const auto tone = make_tone(0.3);

  SL_EXPECT(!vad.process(tone.data(), static_cast<int>(tone.size())));  // 1
  SL_EXPECT(!vad.process(tone.data(), static_cast<int>(tone.size())));  // 2
  SL_EXPECT(!vad.in_speech());
  SL_EXPECT(vad.process(tone.data(), static_cast<int>(tone.size())));  // 第 3 帧确认
  SL_EXPECT(vad.in_speech());
  SL_EXPECT(vad.snr_db() > cfg.onset_db);
}

SL_TEST(EnergyVad_single_frame_burst_rejected) {
  sublens::EnergyVad vad;
  const auto silent = make_silence();
  const auto burst = make_tone(0.4);

  for (int i = 0; i < 50; ++i) vad.process(silent.data(), static_cast<int>(silent.size()));
  SL_EXPECT(!vad.process(burst.data(), static_cast<int>(burst.size())));  // 只有 1 帧, 不够
  for (int i = 0; i < 5; ++i) {
    SL_EXPECT(!vad.process(silent.data(), static_cast<int>(silent.size())));
  }
  SL_EXPECT(!vad.in_speech());
}

SL_TEST(EnergyVad_speech_then_offset) {
  sublens::EnergyVad vad;
  const auto tone = make_tone(0.3);
  const auto silent = make_silence();

  for (int i = 0; i < 5; ++i) vad.process(tone.data(), static_cast<int>(tone.size()));
  SL_EXPECT(vad.in_speech());

  SL_EXPECT(!vad.process(silent.data(), static_cast<int>(silent.size())));
  SL_EXPECT(!vad.in_speech());
}

SL_TEST(EnergyVad_noise_floor_frozen_during_speech) {
  sublens::VadConfig cfg;  // noise_init_db = -58
  sublens::EnergyVad vad(cfg);
  const auto tone = make_tone(0.3);

  for (int i = 0; i < 100; ++i) vad.process(tone.data(), static_cast<int>(tone.size()));
  SL_EXPECT(vad.in_speech());
  // 说话时绝不能用帧能量更新噪声底, 否则底噪被人声带高后再也降不回来
  SL_EXPECT_NEAR(vad.noise_db(), cfg.noise_init_db, 1e-9);
}

SL_TEST(EnergyVad_noise_floor_adapts_on_silence) {
  sublens::VadConfig cfg;
  cfg.noise_init_db = -50.0;  // 比真实底噪略低, 让自适应往上跟
  cfg.onset_db = 12.0;        // 抬高门限, 避免把"底噪比预估高"误判成语音
  sublens::EnergyVad vad(cfg);
  const auto noise = make_noise(0.01, 12345);

  for (int i = 0; i < 300; ++i) {
    SL_EXPECT(!vad.process(noise.data(), static_cast<int>(noise.size())));
  }
  SL_EXPECT(!vad.in_speech());
  // 250ms 后噪声底应该已经收敛到实际帧能量附近
  SL_EXPECT_NEAR(vad.noise_db(), vad.frame_db(), 1.5);
  SL_EXPECT(vad.noise_db() > -46.0);
}

// ===========================================================================
// UtteranceDetector
// ===========================================================================
SL_TEST(UtteranceDetector_demo_script_endpoints) {
  const sublens::ConversationScript* scripts[] = {&sublens::demo_scenario_en2zh(),
                                                  &sublens::demo_scenario_zh2en()};
  for (const sublens::ConversationScript* script : scripts) {
    const sublens::DetectorConfig dcfg;  // 默认 pre_roll 200 / hangover 320 / min 240
    const DetectorRun run = run_detector(*script, dcfg);

    SL_EXPECT_EQ(run.utterances.size(), script->utterances.size());
    SL_EXPECT_EQ(run.emitted, static_cast<uint64_t>(script->utterances.size()));
    SL_EXPECT_EQ(run.dropped, uint64_t{0});
    SL_EXPECT(!run.in_speech_at_end);

    for (size_t i = 0; i < run.utterances.size(); ++i) {
      const sublens::Utterance& u = run.utterances[i];
      const sublens::ScriptUtterance& s = script->utterances[i];

      SL_EXPECT_EQ(u.id, static_cast<uint64_t>(i + 1));
      SL_EXPECT(!u.pcm.empty());
      SL_EXPECT(std::any_of(u.pcm.begin(), u.pcm.end(), [](int16_t v) { return v != 0; }));

      // 硬指标: 端点时长与脚本说话时长误差 < 250ms; 合成音源应做到 < 120ms
      const int64_t duration_err = abs_diff(u.speech_duration_ms, s.speech_ms());
      SL_EXPECT(duration_err < 250);
      SL_EXPECT(duration_err < 120);

      // pcm 起点 = speech_start_ms; pre-roll 起点在真实开口之前(最多 pre_roll_ms)
      SL_EXPECT(u.speech_start_ms <= s.start_ms);
      SL_EXPECT(u.speech_start_ms >= s.start_ms - dcfg.pre_roll_ms - kFrameMs);
      // 除第一句外 pre-roll 应该补满, 即 pcm 开头约 160-200ms 是静音
      if (i > 0) SL_EXPECT(frame_db(u.pcm.data(), kFrameSamples) < -30.0);
      // pcm 末尾必须是语音而不是 hangover 静音(尾部静音对 ASR 无用);
      // 句子末尾可能是正在淡出的半帧(约 -30dBFS), 门限放到 -35。
      SL_EXPECT(u.pcm.size() >= static_cast<size_t>(kFrameSamples));
      SL_EXPECT(frame_db(u.pcm.data() + u.pcm.size() - kFrameSamples, kFrameSamples) > -35.0);
      // pcm 时长 = 有效语音 + pre-roll - onset 确认延迟; 不能短于有效语音
      const int64_t pcm_ms = static_cast<int64_t>(u.pcm.size()) / kSamplesPerMs;
      SL_EXPECT(pcm_ms >= u.speech_duration_ms);
      SL_EXPECT(pcm_ms <= u.speech_duration_ms + dcfg.pre_roll_ms);

      // 句末判定时刻 ≈ 最后一帧语音 + hangover
      const int64_t expected_end = s.start_ms + s.speech_ms() + dcfg.hangover_ms;
      SL_EXPECT(abs_diff(u.speech_end_ms, expected_end) <= 100);
      SL_EXPECT(u.speech_end_ms > u.speech_start_ms);
    }
  }
}

SL_TEST(UtteranceDetector_bench_script_dense_gaps) {
  // 压测脚本句间只有 220ms, 默认 hangover 320 会把相邻两句并成一句;
  // 这里把 hangover 调到 180 验证"高密度对话"下的切分能力。
  sublens::DetectorConfig dcfg;
  dcfg.hangover_ms = 180;
  const sublens::ConversationScript& script = sublens::bench_scenario();
  const DetectorRun run = run_detector(script, dcfg);

  SL_EXPECT_EQ(run.utterances.size(), script.utterances.size());
  SL_EXPECT_EQ(run.dropped, uint64_t{0});
  for (size_t i = 0; i < run.utterances.size(); ++i) {
    SL_EXPECT(abs_diff(run.utterances[i].speech_duration_ms,
                       script.utterances[i].speech_ms()) < 250);
  }
}

SL_TEST(UtteranceDetector_flush_ends_current_speech) {
  sublens::DetectorConfig dcfg;
  dcfg.min_speech_ms = 240;
  dcfg.hangover_ms = 320;
  std::vector<sublens::Utterance> got;
  sublens::UtteranceDetector det(
      [&got](sublens::Utterance&& u) { got.push_back(std::move(u)); }, dcfg);

  const auto silent = make_silence();
  const auto tone = make_tone(0.3);
  int64_t frame_end = 0;
  for (int i = 0; i < 10; ++i) {  // 200ms 静音建立噪声底
    frame_end += kFrameMs;
    det.push_frame(silent.data(), static_cast<int>(silent.size()), frame_end);
  }
  for (int i = 0; i < 15; ++i) {  // 300ms 语音, 说完立刻 flush
    frame_end += kFrameMs;
    det.push_frame(tone.data(), static_cast<int>(tone.size()), frame_end);
  }
  SL_EXPECT(det.in_speech());
  SL_EXPECT(det.current_speech_ms(frame_end + 100) > 0);
  det.flush(0);  // 流结束: 没有下一帧来触发 hangover, 必须靠 flush 收尾

  SL_EXPECT_EQ(got.size(), size_t{1});
  SL_EXPECT(!det.in_speech());
  SL_EXPECT_EQ(det.emitted(), uint64_t{1});
  SL_EXPECT_EQ(det.dropped_short(), uint64_t{0});
  if (!got.empty()) {
    SL_EXPECT(abs_diff(got[0].speech_duration_ms, 300) <= 60);
    SL_EXPECT(!got[0].pcm.empty());
    // pre-roll 已拼到语句前: pcm 至少比有效语音长一个 pre-roll 减去确认延迟
    SL_EXPECT(got[0].pcm.size() >= static_cast<size_t>(300 - 20) * kSamplesPerMs);
    SL_EXPECT_EQ(det.current_speech_ms(frame_end), int64_t{0});
  }
}

SL_TEST(UtteranceDetector_hangover_ends_without_flush) {
  sublens::DetectorConfig dcfg;
  std::vector<sublens::Utterance> got;
  sublens::UtteranceDetector det(
      [&got](sublens::Utterance&& u) { got.push_back(std::move(u)); }, dcfg);

  const auto silent = make_silence();
  const auto tone = make_tone(0.3);
  int64_t frame_end = 0;
  for (int i = 0; i < 10; ++i) {
    frame_end += kFrameMs;
    det.push_frame(silent.data(), static_cast<int>(silent.size()), frame_end);
  }
  for (int i = 0; i < 20; ++i) {  // 400ms 语音
    frame_end += kFrameMs;
    det.push_frame(tone.data(), static_cast<int>(tone.size()), frame_end);
  }
  SL_EXPECT(got.empty());  // 还在说, 没到 hangover
  for (int i = 0; i < 30; ++i) {  // 600ms 静音, 足够触发 hangover
    frame_end += kFrameMs;
    det.push_frame(silent.data(), static_cast<int>(silent.size()), frame_end);
  }
  SL_EXPECT_EQ(got.size(), size_t{1});
  SL_EXPECT(!det.in_speech());
  if (!got.empty()) {
    SL_EXPECT(abs_diff(got[0].speech_duration_ms, 400) <= 60);
    // 句末时刻 = 最后一帧语音 + hangover
    SL_EXPECT(abs_diff(got[0].speech_end_ms, 600 + 320) <= 40);
  }
}

SL_TEST(UtteranceDetector_drops_short_noise) {
  sublens::DetectorConfig dcfg;  // min_speech_ms = 240
  std::vector<sublens::Utterance> got;
  sublens::UtteranceDetector det(
      [&got](sublens::Utterance&& u) { got.push_back(std::move(u)); }, dcfg);

  const auto silent = make_silence();
  const auto tone = make_tone(0.3);
  int64_t frame_end = 0;
  for (int i = 0; i < 10; ++i) {
    frame_end += kFrameMs;
    det.push_frame(silent.data(), static_cast<int>(silent.size()), frame_end);
  }
  for (int i = 0; i < 5; ++i) {  // 100ms 语音 < min_speech_ms
    frame_end += kFrameMs;
    det.push_frame(tone.data(), static_cast<int>(tone.size()), frame_end);
  }
  det.flush(0);

  SL_EXPECT(got.empty());
  SL_EXPECT_EQ(det.dropped_short(), uint64_t{1});
  SL_EXPECT_EQ(det.emitted(), uint64_t{0});
}

SL_TEST(UtteranceDetector_force_split_long_speech) {
  sublens::DetectorConfig dcfg;
  dcfg.max_utterance_ms = 500;
  dcfg.min_speech_ms = 240;
  dcfg.hangover_ms = 320;
  std::vector<sublens::Utterance> got;
  sublens::UtteranceDetector det(
      [&got](sublens::Utterance&& u) { got.push_back(std::move(u)); }, dcfg);

  const auto tone = make_tone(0.3);
  int64_t frame_end = 0;
  for (int i = 0; i < 60; ++i) {  // 1200ms 连续语音, 不停顿
    frame_end += kFrameMs;
    det.push_frame(tone.data(), static_cast<int>(tone.size()), frame_end);
  }
  det.flush(frame_end);

  // 1200ms 被 max_utterance_ms≈500 切成两段完整语句 + 一段不足 min 的尾巴
  SL_EXPECT_EQ(got.size(), size_t{2});
  SL_EXPECT_EQ(det.dropped_short(), uint64_t{1});
  for (const auto& u : got) {
    SL_EXPECT(u.speech_duration_ms >= 480);
    SL_EXPECT(u.speech_duration_ms <= 560);
    SL_EXPECT(!u.pcm.empty());
  }
}
