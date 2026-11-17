#pragma once
/// @file mt.hpp
/// @brief 机器翻译后端抽象 + 离线短语翻译器 + 时延注入包装。
///
/// 真实链路是 HTTP/流式 MT 服务。这里抽成 MtBackend 之后:
///   - PhraseTableMt   : 纯 C++ 词典+模板翻译, 用来在没有网络/没有模型的
///                       机器上跑通整条链路(演示与回归测试);
///   - LatencyInjectingMt : 套在任意后端外面, 注入可配置的云侧往返时延,
///                       用来做延迟预算实验和压测;
///   - 接真模型时只需再实现一个 MtBackend(见 docs/CLOUD-INTEGRATION.md)。

#include <functional>
#include <memory>
#include <string>

#include "sublens/common.hpp"

namespace sublens {

struct MtRequest {
  uint64_t utt_id = 0;
  std::string text;
  bool draft = false;  ///< true=推测翻译(可丢弃); false=确定要上屏
};

struct MtResult {
  uint64_t utt_id = 0;
  std::string text;
  bool draft = false;
  int64_t emit_ms = 0;
};

class MtBackend {
 public:
  using Callback = std::function<void(const MtResult&)>;

  virtual ~MtBackend() = default;
  virtual void set_callback(Callback cb) = 0;
  virtual bool start() = 0;
  /// 非阻塞提交
  virtual void submit(MtRequest req) = 0;
  virtual void flush() = 0;
  virtual void stop() = 0;
  virtual const char* name() const = 0;
};

/// 离线短语级翻译器的配置。
struct PhraseMtConfig {
  std::string source_lang = "zh";
  std::string target_lang = "en";
};

/// 离线短语级翻译器(中英双向), 词典 + 模板 + 数字/量词规则。
///
/// 这不是要跟神经网络翻译比质量, 目的是让整条链路在断网、无 GPU 的机器上
/// 也能真实跑起来, 并且延迟是**真实的 CPU 计算延迟**而不是 sleep 出来的。
class PhraseTableMt : public MtBackend {
 public:
  using Config = PhraseMtConfig;

  explicit PhraseTableMt(Config cfg = {});
  ~PhraseTableMt() override;

  void set_callback(Callback cb) override;
  bool start() override;
  void submit(MtRequest req) override;
  void flush() override;
  void stop() override;
  const char* name() const override { return "phrase-table-mt"; }

  /// 同步翻译单句(测试与词表校验用)
  std::string translate_sync(const std::string& text) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 时延注入器配置。
struct LatencyInjectConfig {
  int base_ms = 180;
  int jitter_ms = 80;
  unsigned seed = 11;
};

/// 时延注入器: 给任意 MT 后端叠加固定 + 抖动的"云侧往返时延"。
///
/// 用途: 在单机上复现"云端 MT 往返 180~320ms"的场景, 验证延迟预算与
/// 调度策略的表现。注入的时延是**串行等待**, 不占 CPU, 不会污染 CPU 开销统计。
class LatencyInjectingMt : public MtBackend {
 public:
  using Config = LatencyInjectConfig;

  LatencyInjectingMt(std::unique_ptr<MtBackend> inner, Config cfg);
  ~LatencyInjectingMt() override;

  void set_callback(Callback cb) override;
  bool start() override;
  void submit(MtRequest req) override;
  void flush() override;
  void stop() override;
  const char* name() const override { return "latency-injecting-mt"; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublens
