#pragma once
/// @file audio_source.hpp
/// @brief 音频输入抽象 + 桌面实现(合成音源 / 静音源 / WAV 文件)。
///
/// 真机(Android 眼镜)上的 AAudio/Oboe 实现只需要遵守 IAudioSource 这一个接口,
/// 上层 VAD 与流水线完全不需要改 —— 这是"同一份 C++ 跑在眼镜上"的关键切口。

#include <cstdint>
#include <memory>
#include <string>

#include "sublens/common.hpp"
#include "sublens/script.hpp"

namespace sublens {

constexpr int kSampleRate = 16000;  ///< 16 kHz, 语音识别标准采样率
constexpr int kFrameMs = 20;        ///< 20 ms 一帧, 兼顾延迟与 VAD 精度
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;  ///< 320 样本

/// 音频源: 每次 read() 交出 20ms 的 PCM16 单声道样本。
class IAudioSource {
 public:
  virtual ~IAudioSource() = default;

  virtual bool start() = 0;
  virtual void stop() = 0;

  /// @param out        接收缓冲区, 容量至少 max_samples
  /// @param max_samples 本次最多读多少个样本
  /// @return >0 实际样本数; 0 表示暂时没有数据(调用方应短暂让出 CPU);
  ///         -1 表示音频流结束
  virtual int read(int16_t* out, int max_samples) = 0;

  virtual const char* name() const = 0;
};

/// ScriptedAudioSource 的配置。
struct ScriptedAudioConfig {
  const ConversationScript* script = nullptr;
  bool realtime_pacing = true;
  int frame_ms = kFrameMs;
  double noise_amplitude = 0.015;  ///< 背景噪声幅度(0~1)
  double speech_amplitude = 0.35;  ///< 语音幅度
  unsigned seed = 7;
};

/// 按 ConversationScript 合成音频: 句内是带包络的谐波+噪声(像说话),
/// 句间是静音。realtime_pacing=true 时按真实时间节奏产出, 用于演示;
/// false 时全速产出, 用于基准测试。
class ScriptedAudioSource : public IAudioSource {
 public:
  using Config = ScriptedAudioConfig;

  explicit ScriptedAudioSource(Config cfg);
  ~ScriptedAudioSource() override;

  bool start() override;
  void stop() override;
  int read(int16_t* out, int max_samples) override;
  const char* name() const override { return "scripted-audio"; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 恒静音源: 用来验证"没有语音时流水线不产生任何输出"。
class SilenceSource : public IAudioSource {
 public:
  explicit SilenceSource(int total_ms = 3000, bool realtime_pacing = false);
  bool start() override;
  void stop() override;
  int read(int16_t* out, int max_samples) override;
  const char* name() const override { return "silence"; }

 private:
  int total_samples_;
  int produced_ = 0;
  bool realtime_pacing_;
  int64_t start_ms_ = 0;
};

/// 16-bit PCM WAV 文件源: 接真实录音做回归测试。
/// 只支持单声道/立体声 16-bit, 采样率不是 16k 时直接报错(避免隐式重采样)。
class WavFileSource : public IAudioSource {
 public:
  static std::unique_ptr<WavFileSource> open(const std::string& path);
  ~WavFileSource() override;

  bool start() override;
  void stop() override;
  int read(int16_t* out, int max_samples) override;
  const char* name() const override { return "wav-file"; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublens
