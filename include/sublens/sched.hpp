#pragma once
/// @file sched.hpp
/// @brief 本项目的核心算法层: 稳定前缀对齐 + 推测式翻译调度。
///
/// 要解决的问题:
///   流式 ASR 的 partial 是"边说边改"的 —— "我想去火" → "我想去火车站"。
///   如果每来一个 partial 就拿整句去翻译并上屏, 字幕会不停闪变、回退,
///   在眼镜上这是灾难性的阅读体验。
///
/// 解法(两步):
///   1) PartialStabilizer: 对最近 N 个 partial 求**最长公共前缀**
///      (LocalAgreement-n 策略), 只有连续 N 次都没变的部分才判定为"稳定",
///      外加标点切分与最大长度兜底, 把不稳定的尾部挡在翻译之前;
///   2) TranslationScheduler: 稳定片段走"正式翻译"——发该句累计原文前缀(带上下文
///      重译), 结果按与上次完整译文的公共前缀做差量追加, 主行只增不改; 不稳定的
///      尾部走"草稿翻译"只更新第二行(可丢弃)。草稿结果到达时如果对应内容
///      已经被正式译文覆盖, 就丢掉 —— 保证已上屏的内容永不回退。
///
/// 这样就得到了"渐进式字幕": 已确认的部分稳定累积, 未确认的部分单独一行
/// 谨慎更新, 视觉上不会打断阅读。

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sublens/asr.hpp"
#include "sublens/common.hpp"
#include "sublens/hud.hpp"
#include "sublens/mt.hpp"  // MtBackend/MtRequest/MtResult: 契约实现必需(原文件漏了这行)

namespace sublens {

/// PartialStabilizer 的配置。
struct StabilizerConfig {
  int confirmations = 2;     ///< 需要多少个 partial 一致才提交(≥2)
  int min_chunk_chars = 2;   ///< 新提交片段的最小字符数(中文按字算)
  int max_chunk_chars = 24;  ///< 未提交尾部的强制切分阈值
  bool split_at_punctuation = true;
};

/// 稳定前缀对齐器(LocalAgreement-n)。
///
/// 算法要点:
///   - 对同一句话保留最近 confirmations 个 partial 的"未提交尾部";
///   - 这些尾部的最长公共前缀 = 可以安全提交的部分;
///   - 提交长度不足 min_chunk_chars 时先攒着, 避免逐字翻译;
///   - 遇到标点(，。？！等)时强制提交到最后一个标点, 让字幕按语义断句;
///   - 尾部超过 max_chunk_chars 时强制提交, 防止长句让延迟无限增长;
///   - is_final=true 时提交全部剩余内容。
class PartialStabilizer {
 public:
  using Config = StabilizerConfig;

  struct Update {
    std::string committed;  ///< 本次新提交的原文(可能为空)
    std::string tail;       ///< 仍未确认的尾部
    bool final = false;     ///< 该句是否已定稿
  };

  explicit PartialStabilizer(Config cfg = {});

  /// 处理一个 ASR partial(同一句的 partial 必须按顺序传入)
  Update on_partial(const AsrPartial& p);

  /// 换句/测试时重置
  void reset();

  uint64_t committed_chunks() const { return committed_chunks_; }

 private:
  std::string longest_common_prefix_utf8(const std::string& a, const std::string& b) const;

  Config cfg_;
  uint64_t utt_id_ = 0;
  std::string committed_;           ///< 该句已提交的全文
  std::vector<std::string> tails_;  ///< 最近 N 个未提交尾部
  uint64_t committed_chunks_ = 0;
  bool warned_revision_ = false;
};

/// TranslationScheduler 的配置。
struct SchedulerConfig {
  bool draft_enabled = true;    ///< 是否开启草稿(推测)翻译
  int draft_throttle_ms = 150;  ///< 同一句两次草稿的最小间隔
  int max_draft_chars = 48;     ///< 太长就不做草稿, 没意义还费钱
  int target_latency_ms = 700;  ///< 目标端到端延迟, 仅用于统计对比
  StabilizerConfig stabilizer;
};

/// 调度器内部统计。
struct SchedulerStats {
  uint64_t partials = 0;
  uint64_t commits = 0;
  uint64_t drafts = 0;
  uint64_t drafts_dropped = 0;
  uint64_t mt_results = 0;
  uint64_t stale_drafts = 0;    ///< 到达时已过时的草稿
  uint64_t final_requests = 0;  ///< 定稿帧补发的整句重译请求数(每句一次)
};

/// 推测式翻译调度器。
///
/// 正式路: 每次提交都发"该句累计的原文前缀"(让 MT 在整句上下文里重译, 而不是
/// 片段级硬拼), 结果回来后与上次完整译文做最长公共前缀的差量追加 —— 既拿到
/// 上下文翻译质量, 又保住 HUD 主行"只增不改"。草稿路: 只发未确认尾部, 整行替换。
///
/// 线程契约: 内部加锁, on_asr_partial / on_mt_result 可以从不同线程调用。
/// sink 回调会在调用方线程内同步触发, 所以 sink 必须足够轻(只更新 HudState)。
class TranslationScheduler {
 public:
  using Config = SchedulerConfig;
  using Stats = SchedulerStats;
  using Sink = std::function<void(const HudUpdate&)>;

  TranslationScheduler(MtBackend& mt, Config cfg = {});

  void set_sink(Sink sink);

  /// 入口: ASR 回调里调用
  void on_asr_partial(const AsrPartial& p);

  /// 入口: MT 回调里调用(由 Pipeline 注册到 MtBackend)
  void on_mt_result(const MtResult& r);

  Stats stats() const;
  void reset();

 private:
  struct UttState {
    std::string committed_src;
    std::string committed_dst;
    int64_t last_draft_ms = 0;
    int64_t speech_end_ms = 0;
    int64_t last_commit_ms = 0;
  };

  /// 每句的调度元数据(跨 ASR 回调 / MT 结果回调必须记住的东西)。
  ///
  /// 为什么不塞进 UttState: UttState 只装"会随正式译文单调累积的数据", 而
  /// 下面这些是配平用的簿记, 生命周期与清理时机也不同(可以独立于 UttState 裁剪)。
  ///   - 正式翻译回调只带回 utt_id, 要把 HudUpdate.src 还原成"本次提交的原文片段",
  ///     就必须把每次提交的片段排队等结果(MT 对同一句按提交顺序返回);
  ///   - 草稿是投机请求, 结果回来时要判断"它基于的原文是否已被正式提交覆盖"。
  ///     节流 150ms 而 MT 往返可能 300ms, 同一句可以同时有多条草稿在途, 所以
  ///     用 FIFO 队列逐条配对: 只用一个槽位的话, 旧草稿结果会配到新草稿的原文;
  ///   - 定稿后该句不允许再冒出草稿(已定稿行只增不改), 需要一个 final 标记。
  struct PendingDraft {
    std::string src;              ///< 发请求时的未确认尾部(回填 HudUpdate.src)
    uint64_t commit_seq = 0;      ///< 发请求时该句的正式提交代数
  };

  struct UttAux {
    /// 等 MT 结果配对的"本次提交原文片段"。注意请求文本是累计前缀(带上下文),
    /// 但 HUD 契约要的 src 只是本次片段, 所以这里存片段、按 FIFO 配对。
    std::deque<std::string> pending_commit_src;
    std::deque<PendingDraft> pending_draft_src;  ///< 在途草稿, 与返回顺序 FIFO 配对
    uint64_t commit_seq = 0;                     ///< 该句累计发出的正式提交数
    bool finalized = false;                      ///< 该句 ASR 已定稿
  };

  void emit(const HudUpdate& u);

  MtBackend& mt_;
  Config cfg_;
  mutable std::mutex mu_;
  PartialStabilizer stabilizer_;
  std::vector<std::pair<uint64_t, UttState>> utts_;      ///< 最近几句的状态(小数组即可)
  std::map<uint64_t, UttAux> utt_aux_;                  ///< 与 utts_ 同步创建/淘汰
  Sink sink_;
  Stats stats_;
};

}  // namespace sublens
