#pragma once
/// @file vad.hpp
/// @brief 语音活动检测(VAD)与端点检测(Endpointing)。
///
/// 端侧最重要的省延迟手段: 不要把整段音频攒起来再发云端, 而是本地判断
/// "这句话说完了", 立刻把这一句送出去。这样端到端延迟就从
/// "等整段音频 + 云端识别" 降到了 "hangover(≈320ms) + 单句识别"。
///
/// 选型说明: 这里是能量+过零率的自适应 VAD, 纯 C++、零依赖、单帧 320 样本
/// 耗时在微秒级; 真机上可以换成 WebRTC VAD / Silero-VAD(onnx), 接口不变。

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "sublens/common.hpp"

namespace sublens {

/// 一段被切出来的话。
struct Utterance {
  uint64_t id = 0;
  int64_t speech_start_ms = 0;  ///< 语音起点(单调时钟 ms)
  int64_t speech_end_ms = 0;    ///< 判定说完的时刻(含 hangover)
  int64_t speech_duration_ms = 0;
  std::vector<int16_t> pcm;     ///< 语句音频(含 pre-roll), 供 ASR 上传
};

/// EnergyVad 的配置。
struct VadConfig {
  double noise_init_db = -58.0;  ///< 初始噪声底
  double noise_adapt = 0.06;     ///< 噪声底自适应速度
  double onset_db = 9.0;         ///< 起始门限(高于噪声底多少 dB)
  double offset_db = 5.0;        ///< 结束门限
  int onset_frames = 2;          ///< 确认起始所需的连续帧数
};

/// 自适应能量 VAD。
///
/// 判决逻辑:
///   1) 每帧算 RMS 能量 → dB; 用"只更新非语音帧"的滑动平均估计噪声底,
///      这样说话时噪声底不会被自己的人声带跑;
///   2) 语音起始: 能量比噪声底高 onset_db 且连续 onset_frames 帧成立(抗突发噪声);
///   3) 语音结束: 能量回落到噪声底附近 —— 但不在这里判定端点,
///      端点交给 UtteranceDetector 的 hangover 计时(避免逗号处被切开)。
class EnergyVad {
 public:
  using Config = VadConfig;

  explicit EnergyVad(Config cfg = {});

  /// 处理一帧, 返回该帧是否判为语音。
  bool process(const int16_t* pcm, int n);

  /// 当前帧的 RMS 能量(dBFS)
  double frame_db() const { return frame_db_; }
  /// 当前估计的噪声底(dBFS)
  double noise_db() const { return noise_db_; }
  /// 当前信噪比(dB)
  double snr_db() const { return frame_db_ - noise_db_; }
  bool in_speech() const { return in_speech_; }

 private:
  Config cfg_;
  double noise_db_;
  double frame_db_ = -96.0;
  bool in_speech_ = false;
  int onset_run_ = 0;
};

/// UtteranceDetector 的配置。
struct DetectorConfig {
  VadConfig vad;
  int hangover_ms = 320;         ///< 连续静音超过它 → 判定句末
  int min_speech_ms = 240;       ///< 短于它的片段当噪声丢弃
  int max_utterance_ms = 12000;  ///< 超长强制切分(防止一个人一直说)
  int pre_roll_ms = 200;         ///< 起点前保留的音频, 避免吞首字
};

/// 端点检测器: 把连续的语音帧攒成 Utterance, 说完就回调交出去。
///
/// 延迟来源(必须写进延迟预算文档):
///   hangover_ms(320) + 最小语音长度检查 ≈ 端侧引入的固定延迟。
///   想要更激进可以降到 240ms, 但会被句中停顿切断, 需要和 ASR 的标点联调。
class UtteranceDetector {
 public:
  using Config = DetectorConfig;
  using Callback = std::function<void(Utterance&&)>;

  UtteranceDetector(Callback cb, Config cfg = {});
  ~UtteranceDetector();

  /// 喂一帧音频。
  /// @param frame_end_ms 这一帧最后一个样本的单调时钟时间(ms)
  void push_frame(const int16_t* pcm, int n, int64_t frame_end_ms);

  /// 音频流结束, 强制把还在缓冲的语句收尾。
  void flush(int64_t now_ms_value = 0);

  /// 当前是否正在说
  bool in_speech() const { return in_speech_; }
  /// 当前说的这句话已经持续多久(ms), 用于 HUD 的"正在听"指示
  int64_t current_speech_ms(int64_t now) const;

  uint64_t emitted() const { return emitted_; }
  uint64_t dropped_short() const { return dropped_short_; }

 private:
  void finish_utterance(int64_t end_ms);
  void start_utterance(int64_t start_ms);

  Callback cb_;
  Config cfg_;
  EnergyVad vad_;

  std::vector<int16_t> pre_roll_;  ///< 起点前的环绕缓冲
  std::vector<int16_t> current_;   ///< 当前语句音频
  int64_t speech_start_ms_ = 0;
  int64_t last_speech_ms_ = 0;  ///< 最后一次检出语音的时间
  bool in_speech_ = false;
  uint64_t next_id_ = 1;
  uint64_t emitted_ = 0;
  uint64_t dropped_short_ = 0;
};

}  // namespace sublens
