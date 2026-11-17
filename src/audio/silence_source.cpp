/// @file silence_source.cpp  恒静音音源
///
/// 用途: 验证"没有语音时流水线不产生任何输出", 也是延迟基准里的空载基线。
/// 这里刻意输出严格的全零而不是 1 LSB 抖动: 单测需要逐样本比较;
/// 真实麦克风不可能全零, 抖动只对真实 DAC 有意义。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#include "sublens/audio_source.hpp"

namespace sublens {
namespace {

/// 睡到第 frame_index 帧的绝对开始时刻, 理由同 ScriptedAudioSource。
void pace_until(int64_t start_ms, int64_t frame_index) {
  const TimePoint deadline = TimePoint(std::chrono::milliseconds(start_ms)) +
                             std::chrono::milliseconds(frame_index * kFrameMs);
  if (deadline > SteadyClock::now()) std::this_thread::sleep_until(deadline);
}

}  // namespace

SilenceSource::SilenceSource(int total_ms, bool realtime_pacing)
    : total_samples_(std::max(0, total_ms) * kSampleRate / 1000),
      realtime_pacing_(realtime_pacing) {}

bool SilenceSource::start() {
  produced_ = 0;
  start_ms_ = realtime_pacing_ ? now_ms() : 0;
  return true;
}

void SilenceSource::stop() {}

int SilenceSource::read(int16_t* out, int max_samples) {
  if (!out || max_samples <= 0) return 0;
  if (produced_ >= total_samples_) return -1;

  const int offset = produced_ % kFrameSamples;
  const int n = std::min({max_samples, kFrameSamples - offset, total_samples_ - produced_});
  if (n <= 0) return 0;

  if (realtime_pacing_) pace_until(start_ms_, produced_ / kFrameSamples);
  std::fill(out, out + n, int16_t{0});
  produced_ += n;
  return n;
}

}  // namespace sublens
