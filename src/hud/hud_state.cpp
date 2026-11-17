/// @file hud_state.cpp  HUD 视图模型: HudUpdate → HudSnapshot 的折叠逻辑
///
/// 这里是"不闪变"约束的唯一执行点。为什么约束不放渲染器:
/// 渲染器只看到快照, 无法判断"这次变化是追加还是回退"; 而快照一旦被
/// 改写, 任何渲染器(终端/GLES/Android)都会忠实显示错误内容。
/// 所以把规则钉死在状态机里, 渲染器就只是被动的画布。

#include "sublens/hud.hpp"

#include <cstddef>
#include <mutex>
#include <string>

namespace sublens {
namespace {

/// 按 UTF-8 码点(而不是字节)数出最多 max_chars 个字符的字节长度。
/// 为什么不用字节截断: 直接 substr 会把一个汉字切成半个, 终端就会出乱码。
size_t utf8_prefix_bytes(const std::string& s, size_t max_chars) {
  size_t i = 0;
  size_t chars = 0;
  while (i < s.size() && chars < max_chars) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    if (c >= 0xF0) {
      len = 4;  // 4 字节: emoji 等
    } else if (c >= 0xE0) {
      len = 3;  // 3 字节: CJK
    } else if (c >= 0xC0) {
      len = 2;  // 2 字节: 拉丁扩展
    }
    if (i + len > s.size()) len = s.size() - i;  // 非法/截断的尾字节, 兜底按单字节走
    i += len;
    ++chars;
  }
  return i;
}

/// 单行草稿的长度上限。草稿是"可以整行替换"的预览行, 超过一行就没法看了,
/// 所以在状态机层就砍掉; 已提交行不能这么处理(只增不改, 砍了就是回退)。
std::string clamp_line(const std::string& s, size_t max_chars) {
  if (max_chars == 0) return std::string();
  if (utf8_prefix_bytes(s, max_chars) >= s.size()) return s;
  if (max_chars == 1) return "\xE2\x80\xA6";  // "…"
  const size_t cut = utf8_prefix_bytes(s, max_chars - 1);
  return s.substr(0, cut) + "\xE2\x80\xA6";
}

}  // namespace

HudState::HudState(Config cfg) : cfg_(cfg) {}

void HudState::apply(const HudUpdate& u) {
  std::lock_guard<std::mutex> lock(mu_);

  switch (u.kind) {
    case HudUpdate::Kind::kDraft: {
      // 同一句、内容完全没变的草稿事件: 画面上什么都不会变, 直接吞掉。
      // 这不计入"被节流丢弃"(那个指标要反映节流策略的代价, 不能被重复事件污染)。
      if (snap_.draft_active && u.utt_id == snap_.draft_utt_id && u.src == snap_.draft_src &&
          u.dst == snap_.draft_dst) {
        return;
      }
      // 换句时草稿行本来就该整体换掉, 不受上一句的节流计时约束,
      // 否则新一句的第一条草稿会被旧句的时间戳挡在门外, 延迟白白变差。
      const bool new_sentence = snap_.draft_utt_id != u.utt_id;
      // 节流: 人眼对"同一行文字被反复改写"极其敏感, 眼镜上尤其晃眼。
      // 所以草稿行只在距上次替换 >= draft_min_interval_ms 时才整行换掉,
      // 期间到达的草稿直接丢弃(这些是推测翻译, 丢掉不影响正确性)。
      if (!new_sentence && last_draft_ms_ != 0 &&
          u.at_ms - last_draft_ms_ < cfg_.draft_min_interval_ms) {
        ++drafts_dropped_;
        return;
      }

      // 只有"同一句的草稿行被新内容覆盖"才算一次 revision;
      // 新句首条草稿是建立行, 不是改写已有内容, 不计。
      // 注意这里的计数口径是"观众真实看到的草稿行内容变化次数":
      // 从空行变成有内容也算一次(否则 demo 里明明一直在出草稿, 指标却是 0),
      // 被节流丢弃的候选没有上屏, 不计入 revisions, 单独计 drafts_dropped。
      snap_.draft_src = clamp_line(u.src, cfg_.max_line_chars);
      snap_.draft_dst = clamp_line(u.dst, cfg_.max_line_chars);
      snap_.draft_utt_id = u.utt_id;
      snap_.draft_active = true;
      last_draft_ms_ = u.at_ms;
      ++revisions_;
      return;
    }

    case HudUpdate::Kind::kCommitted: {
      const bool new_sentence = snap_.cur_utt_id != u.utt_id;
      if (new_sentence) {
        // 出现第一条属于新句的定稿内容 → 上一句到这里才算真正"定稿"
        // (调度器保证同一句的 kCommitted 只增不改, 换句意味着旧句不会再变)。
        // finalized 按"已经出现过定稿内容的句子数"累计, 这样 HUD 底栏
        // 的句数从第一句起就单调增长, 不会出现 0 挂很久的尴尬。
        ++snap_.finalized;
        snap_.cur_utt_id = u.utt_id;
        snap_.cur_src = u.src;
        snap_.cur_dst = u.dst;
      } else {
        // 同一句的后续提交: 只在尾部**追加**。绝不重排、绝不回写已有文字 ——
        // 人眼正在读的那半句如果被改写, 阅读节奏就断了(这是本项目的核心体验指标)。
        snap_.cur_src += u.src;
        snap_.cur_dst += u.dst;
      }
      ++committed_updates_;

      // 草稿行的内容已经进了主行, 再留着就是同一句话显示两遍;
      // 只有"草稿属于这一句或更早的句子"时才清(更新的句子里的草稿不能被误清)。
      if (snap_.draft_utt_id != 0 && u.utt_id >= snap_.draft_utt_id) {
        snap_.draft_src.clear();
        snap_.draft_dst.clear();
        snap_.draft_utt_id = 0;
        snap_.draft_active = false;
      }

      // 端到端延迟: 语音结束 → 这一块译文上屏。负值(时钟异常/跨线程乱序)归零。
      int64_t latency = u.at_ms - u.speech_end_ms;
      if (latency < 0) latency = 0;
      snap_.last_latency_ms = latency;
      return;
    }

    case HudUpdate::Kind::kReplace: {
      // 句末整行校正。为什么需要它: 短语级 MT 对"逐段累加的前缀"和"整句"给出的
      // 译文不是前缀关系(句末会重排词序), 若坚持只追加, 主行会停在半句残文上。
      // 这里用完整译文替换主行一次 —— 它不是"回退", 而是这句的最终稿。
      // 调度器保证每句最多发一次, 所以阅读节奏只被打断这一次。
      snap_.cur_utt_id = u.utt_id;
      snap_.cur_src = u.src;
      snap_.cur_dst = clamp_line(u.dst, cfg_.max_line_chars);
      ++commit_rewrites_;
      ++committed_updates_;

      if (snap_.draft_utt_id != 0 && u.utt_id >= snap_.draft_utt_id) {
        snap_.draft_src.clear();
        snap_.draft_dst.clear();
        snap_.draft_utt_id = 0;
        snap_.draft_active = false;
      }

      // 注意: 整行校正发生在句末, 用它的到达时刻算延迟会把"整句说完"的时间
      // 也算进去(看起来 1 秒以上)。底栏显示的是"首屏延迟", 所以这里不回写。
      return;
    }
  }
}

HudSnapshot HudState::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return snap_;
}

uint64_t HudState::revisions() const {
  std::lock_guard<std::mutex> lock(mu_);
  return revisions_;
}

uint64_t HudState::commit_rewrites() const {
  std::lock_guard<std::mutex> lock(mu_);
  return commit_rewrites_;
}

uint64_t HudState::committed_updates() const {
  std::lock_guard<std::mutex> lock(mu_);
  return committed_updates_;
}

uint64_t HudState::drafts_dropped() const {
  std::lock_guard<std::mutex> lock(mu_);
  return drafts_dropped_;
}

void HudState::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  snap_ = HudSnapshot{};
  last_draft_ms_ = 0;
  revisions_ = 0;
  commit_rewrites_ = 0;
  committed_updates_ = 0;
  drafts_dropped_ = 0;
}

}  // namespace sublens
