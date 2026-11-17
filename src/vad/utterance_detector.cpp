/// @file utterance_detector.cpp  语音端点检测
///
/// VAD 只回答"这一帧像不像语音", 这里回答"这句话什么时候结束":
///   - pre-roll: 起始确认会晚 onset_frames 帧, 把起点前的音频一起交出去,
///     避免 ASR 吞掉第一个字;
///   - hangover: 能量回落后再等一段, 防止逗号/换气处把一句话切碎;
///   - min/max 时长: 太短的当噪声丢掉, 太长的强制切开保证 ASR 有输出;
///   - flush: 音频流结束时把最后半句话收尾。
///
/// 时间戳口径(下游 ASR/HUD 依赖这个定义):
///   speech_start_ms = pcm[0] 的时间 = pre-roll 起点;
///   speech_end_ms   = 判定句末的那一帧末尾(含 hangover);
///   speech_duration_ms = 有效语音时长, 不含 pre-roll 和尾部 hangover。

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "sublens/audio_source.hpp"
#include "sublens/vad.hpp"

namespace sublens {
namespace {

constexpr int kSamplesPerMs = kSampleRate / 1000;  ///< 16

/// 样本数 → 毫秒(向下取整; 调用方都是整帧, 这里只做兜底)
int64_t span_ms_of(const std::vector<int16_t>& pcm) {
  return static_cast<int64_t>(pcm.size()) / kSamplesPerMs;
}

int frame_ms_of(int n) { return n > 0 ? n * 1000 / kSampleRate : 0; }

/// 维护定长 pre-roll: 超出容量的旧样本从头部丢掉。
void append_pre_roll(std::vector<int16_t>& pre_roll, const int16_t* pcm, int n,
                     int capacity) {
  if (capacity <= 0) return;
  pre_roll.insert(pre_roll.end(), pcm, pcm + n);
  if (static_cast<int>(pre_roll.size()) > capacity) {
    const size_t drop = pre_roll.size() - static_cast<size_t>(capacity);
    pre_roll.erase(pre_roll.begin(), pre_roll.begin() + static_cast<std::ptrdiff_t>(drop));
  }
}

}  // namespace

UtteranceDetector::UtteranceDetector(Callback cb, Config cfg)
    : cb_(std::move(cb)), cfg_(cfg), vad_(cfg.vad) {
  // 预分配: push_frame 在 VAD 线程热路径上, 不希望中途 realloc
  pre_roll_.reserve(static_cast<size_t>(std::max(0, cfg_.pre_roll_ms) * kSamplesPerMs));
  const int capacity_ms =
      std::max(0, cfg_.max_utterance_ms) + std::max(0, cfg_.pre_roll_ms);
  current_.reserve(static_cast<size_t>(capacity_ms) * kSamplesPerMs);
}

UtteranceDetector::~UtteranceDetector() = default;

void UtteranceDetector::push_frame(const int16_t* pcm, int n, int64_t frame_end_ms) {
  if (!pcm || n <= 0) return;
  if (frame_ms_of(n) <= 0) return;

  const bool is_speech = vad_.process(pcm, n);

  if (!in_speech_) {
    append_pre_roll(pre_roll_, pcm, n, cfg_.pre_roll_ms * kSamplesPerMs);
    if (!is_speech) return;

    // VAD 连续 onset_frames 帧确认后才返回 true, 真实起点在这之前;
    // pre-roll 已经完整保留了那几帧, 所以直接把 pre-roll 起点当作 pcm 起点。
    const int64_t pre_roll_start_ms = frame_end_ms - span_ms_of(pre_roll_);
    start_utterance(pre_roll_start_ms);
    last_speech_ms_ = frame_end_ms;
    return;
  }

  current_.insert(current_.end(), pcm, pcm + n);
  if (is_speech) {
    last_speech_ms_ = frame_end_ms;
    // 一句话持续超过上限就强制切分: 宁可多送一次 ASR, 也不能一直不出结果。
    // 用有效语音起点(而不是 pcm 起点)计时, 否则 pre-roll 会让切分提前。
    const int64_t effective_start_ms =
        speech_start_ms_ + span_ms_of(pre_roll_) -
        static_cast<int64_t>(cfg_.vad.onset_frames) * kFrameMs;
    if (frame_end_ms - effective_start_ms > cfg_.max_utterance_ms) {
      finish_utterance(frame_end_ms);
    }
    return;
  }

  // 语音帧之后连续静音超过 hangover 才算说完; 静音帧本身仍进缓冲,
  // 收尾时再统一裁掉(见 finish_utterance)。
  if (frame_end_ms - last_speech_ms_ >= cfg_.hangover_ms) {
    finish_utterance(frame_end_ms);
  }
}

void UtteranceDetector::start_utterance(int64_t start_ms) {
  // pre-roll 先整段拼到语句前面; 这里不清空 pre_roll_, 因为结束时要靠它的
  // 长度从"pcm 起点"反推"有效语音起点"(见 finish_utterance)。
  current_.assign(pre_roll_.begin(), pre_roll_.end());
  speech_start_ms_ = start_ms;
  last_speech_ms_ = start_ms;
  in_speech_ = true;
}

void UtteranceDetector::finish_utterance(int64_t end_ms) {
  if (!in_speech_) return;
  in_speech_ = false;

  // 有效语音时长 = 最后一帧语音 - pcm 起点 - pre-roll + onset 确认延迟补偿。
  // 不含尾部 hangover 静音, 这样 HUD/统计里的"这句话多长"接近脚本真实时长。
  const int64_t pre_roll_ms = span_ms_of(pre_roll_);
  int64_t duration_ms = last_speech_ms_ - speech_start_ms_ - pre_roll_ms +
                        static_cast<int64_t>(cfg_.vad.onset_frames) * kFrameMs;
  duration_ms = std::max<int64_t>(0, duration_ms);

  // 尾部 hangover 静音对 ASR 没有信息量, 只保留到最后一个语音帧。
  const int64_t keep_samples = (last_speech_ms_ - speech_start_ms_) * kSamplesPerMs;
  if (keep_samples >= 0 && keep_samples < static_cast<int64_t>(current_.size())) {
    current_.resize(static_cast<size_t>(keep_samples));
  }

  if (duration_ms < cfg_.min_speech_ms || current_.empty()) {
    ++dropped_short_;
    current_.clear();
  } else {
    Utterance u;
    u.id = next_id_++;
    u.speech_start_ms = speech_start_ms_;
    u.speech_end_ms = end_ms;
    u.speech_duration_ms = duration_ms;
    u.pcm = std::move(current_);
    current_.clear();  // 保留容量, 下一句继续复用
    ++emitted_;
    if (cb_) cb_(std::move(u));
  }

  // 这一句的 pre-roll 已经用不上了, 从下一帧开始重新积累
  pre_roll_.clear();
  speech_start_ms_ = 0;
  last_speech_ms_ = 0;
}

void UtteranceDetector::flush(int64_t now_ms_value) {
  if (!in_speech_) {
    pre_roll_.clear();
    return;
  }
  // 流结束时没有"下一帧"来触发 hangover, 用调用方给的时间(或最后语音帧)
  // 直接收尾; 如果静音其实已经超过 hangover, 结果和正常结束一致。
  const int64_t end_ms = now_ms_value > last_speech_ms_ ? now_ms_value : last_speech_ms_;
  finish_utterance(end_ms);
  pre_roll_.clear();
}

int64_t UtteranceDetector::current_speech_ms(int64_t now) const {
  if (!in_speech_) return 0;
  const int64_t effective_start_ms =
      speech_start_ms_ + span_ms_of(pre_roll_) -
      static_cast<int64_t>(cfg_.vad.onset_frames) * kFrameMs;
  return std::max<int64_t>(0, now - effective_start_ms);
}

}  // namespace sublens
