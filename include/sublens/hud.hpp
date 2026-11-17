#pragma once
/// @file hud.hpp
/// @brief 眼镜 HUD(第一视角字幕)的视图模型与渲染器。
///
/// 为什么 HUD 要单独建模, 而不是"ASR 出字就直接画":
/// 单目波导显示器的字是"悬在眼前"的, 眼球会下意识去追变化的部分。
/// 所以 HUD 有两条硬规则:
///   1) 已定稿的行**只增不改**(append-only), 永不回退重排;
///   2) 未定稿的草稿单独占一行, 只在达到阈值时**整行替换**,
///      用"整行换页"代替"逐字抖动" —— 人眼对整行替换的容忍度远高于逐字跳。
/// 这两条规则由 HudState 强制执行, 渲染器只是被动画快照。

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sublens/common.hpp"

namespace sublens {

/// 调度器产出的上屏事件。
///
/// 三种事件对应 HUD 的两行与两种更新方式:
///   - kDraft   : 未确认尾部的推测译文 → 草稿行, 允许整行替换(带节流);
///   - kCommitted: 稳定片段的译文 → 主行**追加**, 只增不改;
///   - kReplace : 句末整行校正 → 主行用完整译文替换一次。
///     (为什么需要它: 短语级 MT 对"逐段累加的前缀"与"整句"给出的译文不是前缀
///      关系, 句末会重排词序; 若坚持只追加, 主行会停在半句残文上。句末替换一次,
///      换来一句通顺的完整译文, 是更合理的取舍 —— 每句最多一次。)
struct HudUpdate {
  enum class Kind { kDraft, kCommitted, kReplace };
  Kind kind = Kind::kDraft;
  uint64_t utt_id = 0;
  std::string src;            ///< 原文片段(kReplace 时为该句完整原文)
  std::string dst;            ///< 译文(kCommitted 为新增片段; kReplace 为完整译文)
  int64_t speech_end_ms = 0;  ///< 这句话语音结束的时刻(用于统计端到端延迟)
  int64_t at_ms = 0;          ///< 事件产生时刻
};

/// 一帧 HUD 画面(渲染线程只读这个快照, 不碰业务状态)。
struct HudSnapshot {
  std::string cur_src;  ///< 当前句: 已提交原文
  std::string cur_dst;  ///< 当前句: 已提交译文
  std::string draft_src;
  std::string draft_dst;
  bool draft_active = false;
  uint64_t cur_utt_id = 0;
  uint64_t draft_utt_id = 0;
  uint64_t finalized = 0;      ///< 累计已定稿句数
  int64_t last_latency_ms = 0; ///< 最近一句的端到端延迟(角落小字展示)
};

/// HudState 的配置。
struct HudStateConfig {
  int draft_min_interval_ms = 140;  ///< 草稿行两次替换的最小间隔(节流)
  size_t max_line_chars = 120;      ///< 单行最长字符数, 超出截断
};

/// HUD 状态机: 把 HudUpdate 折叠成快照, 并维护"不闪变"约束。
class HudState {
 public:
  using Config = HudStateConfig;

  explicit HudState(Config cfg = {});

  /// 应用一条上屏事件(内部加锁, 可从任意线程调用)
  void apply(const HudUpdate& u);

  /// 取当前快照(内部加锁, 极短临界区)
  HudSnapshot snapshot() const;

  /// 草稿行被整行替换的次数 —— 越小越"稳", 是重要的体验指标
  uint64_t revisions() const;
  /// 主行被"句末整行校正"的次数(每句最多一次)
  uint64_t commit_rewrites() const;
  uint64_t committed_updates() const;
  uint64_t drafts_dropped() const;

  void reset();

 private:
  Config cfg_;
  mutable std::mutex mu_;
  HudSnapshot snap_;
  int64_t last_draft_ms_ = 0;
  uint64_t revisions_ = 0;
  uint64_t commit_rewrites_ = 0;
  uint64_t committed_updates_ = 0;
  uint64_t drafts_dropped_ = 0;
};

/// 终端 ANSI 渲染器配置。
struct AnsiHudConfig {
  int width = 68;            ///< 画布字符宽度
  bool show_metrics = true;  ///< 底部显示延迟/帧率等指标条
  bool color = true;
  std::string header = "SubLens HUD";
};

/// 渲染器接口: 终端 ANSI / Android GLES / 无渲染(基准测试)。
class HudRenderer {
 public:
  virtual ~HudRenderer() = default;

  virtual bool start() = 0;
  virtual void stop() = 0;

  /// 以 HUD 帧率被调用。实现内部应做差量重绘与限流。
  virtual void render(const HudSnapshot& s, int64_t now_ms) = 0;

  virtual const char* name() const = 0;
};

/// 终端 ANSI 渲染器: 用转义序列原地重绘, 模拟"眼镜里看到的两行字幕"。
/// 桌面演示用它, 录屏即可作为作品视频。
class AnsiHudRenderer : public HudRenderer {
 public:
  using Config = AnsiHudConfig;

  explicit AnsiHudRenderer(Config cfg = {});
  ~AnsiHudRenderer() override;

  bool start() override;
  void stop() override;
  void render(const HudSnapshot& s, int64_t now_ms) override;
  const char* name() const override { return "ansi-hud"; }

  /// 把一帧画成纯文本(测试与截图用, 不带控制字符)
  static std::string render_to_string(const HudSnapshot& s, int width, bool color = false);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 空渲染器: 基准测试与单元测试用, 只统计帧数。
class NullHudRenderer : public HudRenderer {
 public:
  bool start() override { return true; }
  void stop() override {}
  void render(const HudSnapshot&, int64_t) override { ++frames_; }
  const char* name() const override { return "null-hud"; }
  uint64_t frames() const { return frames_; }

 private:
  uint64_t frames_ = 0;
};

}  // namespace sublens
