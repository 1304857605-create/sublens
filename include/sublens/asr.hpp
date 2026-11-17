#pragma once
/// @file asr.hpp
/// @brief 流式语音识别后端抽象 + 脚本化模拟器。
///
/// 真实链路里这里是 WebSocket 流式 ASR(阿里云/讯飞/OpenAI Realtime/本地
/// whisper.cpp)。所有差异都被关在 AsrBackend 后面, 上层调度器只认
/// "partial 事件 + 时间戳", 因此换供应商不需要改流水线。
///
/// 为什么需要"模拟器": 端到端延迟是被 ASR 的**首字延迟**和**尾部收敛速度**
/// 主导的, 而这两个指标只能在真实网络下测。模拟器把这两个量参数化,
/// 于是可以在单机上做可复现的延迟预算实验(见 docs/LATENCY-BUDGET.md)。

#include <functional>
#include <memory>
#include <string>

#include "sublens/common.hpp"
#include "sublens/script.hpp"
#include "sublens/vad.hpp"

namespace sublens {

/// 一次流式识别事件。
struct AsrPartial {
  uint64_t utt_id = 0;
  std::string text;       ///< 该句**到目前为止**的完整假设(不是增量!)
  bool is_final = false;  ///< 该句是否已定稿(云端返回 final)
  int64_t speech_start_ms = 0;
  int64_t speech_end_ms = 0;  ///< 这句话语音结束的时刻(延迟从这里开始算)
  int64_t emit_ms = 0;        ///< 产生该事件时的单调时钟
  double confidence = 1.0;
};

/// ASR 后端接口。
///
/// 线程契约: submit() 非阻塞, 结果通过 set_callback() 注册的回调返回,
/// 回调可能在任何线程上发生(实现方自备工作线程), 所以回调必须足够快、
/// 不阻塞 —— 上层只会把它转成一个无锁队列的 push。
class AsrBackend {
 public:
  using Callback = std::function<void(const AsrPartial&)>;

  virtual ~AsrBackend() = default;

  virtual void set_callback(Callback cb) = 0;
  virtual bool start() = 0;
  virtual void submit(Utterance utt) = 0;
  /// 等待内部所有已提交的语句处理完毕
  virtual void flush() = 0;
  virtual void stop() = 0;
  virtual const char* name() const = 0;
};

/// 脚本化流式 ASR。
///
/// 行为建模(尽量贴近真实云端流式 ASR):
///   - 提交后先等 first_partial_delay_ms —— 这是"首字延迟", 也是延迟预算里
///     最大的一块;
///   - 之后每 word_step_ms 吐一个新的 partial, 文本逐步变长;
///   - 句尾再补一个 is_final=true 的 partial;
///   - jitter_ms 给每个事件加上随机抖动(模拟网络与解码抖动);
///   - inject_errors=true 时故意写错某几个字, 用来验证"稳定前缀"机制
///     不会被识别错误带歪(这是推测式翻译最容易踩的坑)。
struct ScriptedAsrConfig {
  const ConversationScript* script = nullptr;
  int first_partial_delay_ms = 260;  ///< 首字延迟
  int word_step_ms = 95;             ///< 每词推进间隔
  int final_extra_ms = 120;          ///< 句尾定稿额外延迟
  int jitter_ms = 25;                ///< 抖动幅度(0 ~ jitter)
  bool inject_errors = false;
  unsigned seed = 2026;
};

class ScriptedAsrBackend : public AsrBackend {
 public:
  using Config = ScriptedAsrConfig;

  explicit ScriptedAsrBackend(Config cfg);
  ~ScriptedAsrBackend() override;

  void set_callback(Callback cb) override;
  bool start() override;
  void submit(Utterance utt) override;
  void flush() override;
  void stop() override;
  const char* name() const override { return "scripted-asr"; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 云端流式 ASR 的骨架实现: 留出 where-to-integrate。
///
/// 没有配置 endpoint 时 start() 返回 false, 流水线会给出明确报错,
/// 而不是静默降级 —— 演示工程最怕"看起来能跑其实没接上"。
struct CloudAsrConfig {
  std::string endpoint;  ///< wss://... 或 http://...
  std::string api_key;
  std::string model = "paraformer-realtime-v2";
  int connect_timeout_ms = 3000;
};

class CloudAsrBackend : public AsrBackend {
 public:
  using Config = CloudAsrConfig;

  explicit CloudAsrBackend(Config cfg);
  void set_callback(Callback cb) override;
  bool start() override;
  void submit(Utterance utt) override;
  void flush() override;
  void stop() override;
  const char* name() const override { return "cloud-asr"; }

 private:
  Config cfg_;
  Callback cb_;
};

}  // namespace sublens
