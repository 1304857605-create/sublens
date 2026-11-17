/// @file scripted_audio_source.cpp  按对话脚本合成"像人说话"的音频
///
/// 合成音源是整条离线链路的时间基准: 句内是谐波+噪声, 句间是底噪,
/// 帧边界严格对齐脚本的 start_ms / speech_ms。这样 VAD 切出来的端点和
/// 脚本时间线一一对应, 演示与回归测试才有确定的预期, 而不是"听感差不多"。
///
/// 选型说明: 不用 std::rand 生成噪声 —— 它内部有全局锁, 会污染多线程基准,
/// 且不同 libc 的实现不同、序列不可复现。这里用自带的 xorshift64*,
/// 同一个 seed 在任何平台上都产出同一段音频。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include "sublens/audio_source.hpp"

namespace sublens {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSamplesPerMs = kSampleRate / 1000;  ///< 16 kHz 下 1ms 恰好 16 样本

// ---- 语音合成参数: 目标不是"好听", 而是让每帧能量和真人说话一样稳定在
//      噪声底之上, 这样 VAD 的门限在不同句子上表现一致。 ----
constexpr double kFundamentalMin = 120.0;  ///< 基频下限(女声/童声范围)
constexpr double kFundamentalMax = 220.0;  ///< 基频上限(男声范围)
constexpr int kHarmonicCount = 4;
/// 谐波增益: 1, 0.45, 0.22, 0.10 —— 浊音能量随谐波次数衰减的大致形状
constexpr double kHarmonicGain[kHarmonicCount] = {1.0, 0.45, 0.22, 0.10};
constexpr double kHarmonicNorm = 1.77;  ///< 上面 4 个增益之和, 用于归一化防削波
constexpr double kEnvFloor = 0.45;      ///< 包络下限: 保证音节间隙也不会掉到 VAD 门限下
constexpr double kEnvDepth = 1.0 - kEnvFloor;
constexpr double kSyllableHz = 4.2;    ///< 音节率(中文/英文平均 4~5 音节/秒)
constexpr double kVibratoHz = 5.5;     ///< 基频微颤
constexpr double kVibratoDepth = 0.035;
constexpr double kEdgeFadeSec = 0.015;  ///< 15ms 淡入淡出, 去掉起停瞬间的爆音

/// 背景噪声的绝对电平必须和 VadConfig 的默认噪声底对齐, 否则 VAD 开机就误判:
/// 均匀噪声 RMS = 峰值/√3, 若把 noise_amplitude(0.015) 直接当峰值,
/// 底噪约 -41dBFS, 比 VadConfig::noise_init_db(-58dBFS) 高 17dB > onset_db(9dB),
/// 于是第一段句间静音会被当成语音, 整条对话都切不开。
/// 这里把 noise_amplitude 当"相对强度"(默认 0.015 对应 -58dBFS), 做一次绝对电平标定。
constexpr double kNoiseFloorDb = -58.0;
constexpr double kNoiseAmplitudeDefault = 0.015;

/// xorshift64*: 每样本一次, 无锁、可复现。
struct XorShift64 {
  uint64_t state;
  explicit XorShift64(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint64_t next() {
    uint64_t x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1Dull;
  }
  /// [-1, 1) 均匀分布; 取高位 24 bit, 低位随机性差。
  double uniform() {
    const uint32_t u = static_cast<uint32_t>(next() >> 40);
    return static_cast<double>(u) * (2.0 / 16777216.0) - 1.0;
  }
};

/// 睡到第 frame_index 帧的绝对开始时刻。
/// 为什么用绝对时刻而不是"每帧睡 20ms": 后者会把调度误差逐帧累加,
/// 长对话跑几分钟后音频时间线就和单调时钟偏出去几十毫秒。
void pace_until(TimePoint start, int64_t frame_index, int frame_ms) {
  const TimePoint deadline = start + std::chrono::milliseconds(frame_index * frame_ms);
  if (deadline > SteadyClock::now()) std::this_thread::sleep_until(deadline);
}

}  // namespace

struct ScriptedAudioSource::Impl {
  Config cfg;
  const ConversationScript* script = nullptr;
  XorShift64 rng{1};

  int total_samples = 0;  ///< 按脚本总时长算出的样本总数
  int pos = 0;            ///< 已产出的绝对样本位置
  bool running = false;
  int next_utt = 0;  ///< 样本时间单调前进, 语句游标只增不减, 摊还 O(1)
  double phase = 0.0;  ///< 谐波相位, 连续累加以免句内相位跳变
  double noise_peak = 0.0;  ///< 标定后的背景噪声峰值
  TimePoint start_time{};

  int frame_samples() const {
    return kSampleRate * (cfg.frame_ms > 0 ? cfg.frame_ms : kFrameMs) / 1000;
  }

  /// 当前样本时刻落在哪句话里, 返回语句下标; 句间静音返回 -1。
  int speech_utterance_at(int64_t sample_ms) {
    while (next_utt < static_cast<int>(script->utterances.size())) {
      const ScriptUtterance& u = script->utterances[next_utt];
      const int64_t end_ms = static_cast<int64_t>(u.start_ms) + u.speech_ms();
      if (sample_ms < u.start_ms) return -1;
      if (sample_ms < end_ms) return next_utt;
      ++next_utt;  // 这句话已经说完, 移到下一句
    }
    return -1;
  }

  /// 基频按语句下标错开, 让 6 句听起来不是同一个人反复念同一句。
  double fundamental_of(int utt_index) const {
    return kFundamentalMin +
           std::fmod(static_cast<double>(utt_index) * 37.0,
                     kFundamentalMax - kFundamentalMin + 1.0);
  }

  int16_t render_sample() {
    double speech = 0.0;
    // 逐样本取当前位置并立刻递增: 包络/淡入淡出/相位都是"样本级"量,
    // 用整帧的起始位置生成一帧会让人声变成一段常数值, 等于没有语音起伏。
    const int64_t sample_pos = pos++;
    const int64_t ms = sample_pos / kSamplesPerMs;
    const int utt = speech_utterance_at(ms);
    if (utt >= 0) {
      const ScriptUtterance& u = script->utterances[utt];
      const int64_t utt_start = static_cast<int64_t>(u.start_ms) * kSamplesPerMs;
      const double t = static_cast<double>(sample_pos - utt_start) / kSampleRate;
      const double dur = static_cast<double>(u.speech_ms()) / 1000.0;

      // 音节级包络: 一个低频正弦近似"字与字之间的强弱起伏"
      const double env_phase = 0.9 * static_cast<double>(utt);
      double env = kEnvFloor +
                   kEnvDepth * 0.5 *
                       (1.0 + std::sin(2.0 * kPi * kSyllableHz * t + env_phase));
      // 句首/句尾 15ms 淡入淡出: 不做的话起停处会产生宽带"咔哒"声,
      // 那一帧的瞬时能量会干扰 VAD 的起始判决。
      env *= std::min({1.0, t / kEdgeFadeSec,
                       std::max(0.0, (dur - t) / kEdgeFadeSec)});

      const double f0 = fundamental_of(utt);
      const double freq =
          f0 * (1.0 + kVibratoDepth * std::sin(2.0 * kPi * kVibratoHz * t));
      double wave = 0.0;
      for (int h = 1; h <= kHarmonicCount; ++h) {
        wave += kHarmonicGain[h - 1] * std::sin(phase * static_cast<double>(h));
      }
      phase += 2.0 * kPi * freq / kSampleRate;
      if (phase > 2.0 * kPi) phase -= 2.0 * kPi;  // 长期运行避免相位数值膨胀

      speech = cfg.speech_amplitude * env * (wave / kHarmonicNorm);
    }

    // 背景噪声一直存在(通话/室内底噪), 只是句间没有语音
    const double v =
        std::clamp(speech + rng.uniform() * noise_peak, -1.0, 1.0);
    return static_cast<int16_t>(std::lround(v * 32767.0));
  }
};

ScriptedAudioSource::ScriptedAudioSource(Config cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = cfg;
  impl_->script = cfg.script;
  impl_->rng = XorShift64(cfg.seed);
  // 绝对电平标定: 把相对强度换算成"默认 -58dBFS 的均匀噪声峰值"
  impl_->noise_peak = std::pow(10.0, kNoiseFloorDb / 20.0) * std::sqrt(3.0) *
                      (cfg.noise_amplitude / kNoiseAmplitudeDefault);
  if (cfg.script) {
    impl_->total_samples =
        static_cast<int>(static_cast<int64_t>(cfg.script->total_ms()) * kSamplesPerMs);
  }
}

ScriptedAudioSource::~ScriptedAudioSource() = default;

bool ScriptedAudioSource::start() {
  if (!impl_ || !impl_->script || impl_->total_samples <= 0) return false;
  impl_->pos = 0;
  impl_->next_utt = 0;
  impl_->phase = 0.0;
  impl_->rng = XorShift64(impl_->cfg.seed);
  impl_->start_time = SteadyClock::now();
  impl_->running = true;
  return true;
}

void ScriptedAudioSource::stop() {
  if (impl_) impl_->running = false;
}

int ScriptedAudioSource::read(int16_t* out, int max_samples) {
  if (!impl_ || !out || max_samples <= 0) return 0;
  if (!impl_->running) return -1;
  if (impl_->pos >= impl_->total_samples) return -1;

  // 一次只交出一帧的剩余部分: 消费者按 20ms 一帧处理, 帧边界必须干净,
  // 否则 VAD 的 frame_end_ms 和脚本时间线对不上。
  const int frame_samples = impl_->frame_samples();
  const int offset = impl_->pos % frame_samples;
  const int n = std::min({max_samples, frame_samples - offset,
                          impl_->total_samples - impl_->pos});
  if (n <= 0) return 0;

  if (impl_->cfg.realtime_pacing) {
    pace_until(impl_->start_time, impl_->pos / frame_samples, impl_->cfg.frame_ms);
  }
  for (int i = 0; i < n; ++i) out[i] = impl_->render_sample();
  // pos 已经由 render_sample() 逐样本推进, 这里不能再加一次
  return n;
}

}  // namespace sublens
