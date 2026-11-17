/// @file energy_vad.cpp  自适应能量 VAD
///
/// 判决只用两个量: 帧 RMS 能量(dBFS) 和噪声底估计。噪声底是整段 VAD 的
/// 关键 —— 如果它在说话时也跟着更新, 说几句之后底噪就被拉高, 之后静音
/// 会被误判成语音; 所以这里只在**确认的非语音帧**上做指数滑动。
///
/// 端点(一句话什么时候结束)不在这里判: 人说话有逗号停顿, 单帧能量回落
/// 很正常, 交给 UtteranceDetector 的 hangover 计时, 避免句子被切碎。

#include <cmath>
#include <cstdint>

#include "sublens/vad.hpp"

namespace sublens {
namespace {

constexpr double kFullScale = 32768.0;  ///< int16 满幅
constexpr double kMinRms = 1e-9;        ///< 低于它的 RMS 视为数字静音, 防 log(0)
constexpr double kFloorDb = -96.0;      ///< 数字静音对应的 dBFS 下限

/// 帧 RMS → dBFS。n<=0 时按静音处理, 避免空帧让调用方拿到 NaN。
double rms_db(const int16_t* pcm, int n) {
  if (!pcm || n <= 0) return kFloorDb;
  double sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double s = static_cast<double>(pcm[i]);
    sum_sq += s * s;
  }
  const double rms = std::sqrt(sum_sq / n) / kFullScale;
  return rms > kMinRms ? 20.0 * std::log10(rms) : kFloorDb;
}

}  // namespace

EnergyVad::EnergyVad(Config cfg) : cfg_(cfg), noise_db_(cfg.noise_init_db) {}

bool EnergyVad::process(const int16_t* pcm, int n) {
  frame_db_ = rms_db(pcm, n);
  const double snr = frame_db_ - noise_db_;

  if (!in_speech_) {
    if (snr > cfg_.onset_db) {
      // 连续 onset_frames 帧都过起始门限才算语音, 单帧"啪"一声不算
      if (++onset_run_ >= cfg_.onset_frames) {
        onset_run_ = 0;
        in_speech_ = true;
      }
    } else {
      onset_run_ = 0;
      // 只在非语音帧更新噪声底: 说话时更新会把底噪拽向人声, 之后静音判不回来
      noise_db_ += cfg_.noise_adapt * (frame_db_ - noise_db_);
    }
  } else if (snr <= cfg_.offset_db) {
    // 能量回落到噪声底附近 → 退出语音。这里只是"这一帧不像语音",
    // 句子是否真的说完由 detector 的 hangover 决定。
    in_speech_ = false;
    onset_run_ = 0;
  }
  return in_speech_;
}

}  // namespace sublens
