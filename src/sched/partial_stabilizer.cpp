/// @file partial_stabilizer.cpp
/// @brief LocalAgreement-n 稳定前缀对齐的实现(本项目的核心算法之一)。
///
/// 要解决的问题:
///   流式 ASR 的 partial 是"整句重写"而不是"追加": "我想去火" → "我想去火车站"。
///   如果每来一个 partial 就拿整句去翻译上屏, 字幕会不停改字/回退 —— 对悬在
///   眼前的光波导 HUD 来说, 这比在手机上更糟: 眼球会下意识去追变化的字。
///   所以这里找的不是"最新的句子", 而是"已经连续 confirmations 次都没变过的
///   前缀": 多次独立解码结果一致的片段才算可信(LocalAgreement-n 思想)。
///
/// 四档提交触发条件, 优先级从强到弱(强条件可以绕过弱条件的门限):
///   1) 定稿 is_final: 这句说完了, 剩下的全部提交, 一个字都不能丢;
///   2) 标点: "，。？！、；：" 是语义强边界。人听到句读就期待换行, 而且标点
///      本身就是一个很强的"这里不会变了"的信号, 所以不必再等 LocalAgreement;
///   3) 一致性: 最近 confirmations 个尾部的最长公共前缀;
///   4) max_chunk_chars 兜底: 尾部太长就硬切。否则 ASR 一直不吐标点时, 延迟会
///      无界增长 —— 实时字幕的延迟预算是硬约束, 宁可多翻一次也不能越拖越久。
///
/// 为什么长度/切分一律按 UTF-8 码点算:
///   中文一个字在 UTF-8 里是 3 字节, 按字节切会切出"半个字", 上屏直接乱码。
///   所以这里"字符数"全部是码点数, 所有截断都保证落在码点边界上。

#include "sublens/sched.hpp"

#include <algorithm>
#include <cstddef>

namespace sublens {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 小工具
// ---------------------------------------------------------------------------
// 容错原则: 遇到非法序列(理论上不该出现)按 1 字节前进, 宁可切得保守也不要
// 因为一个坏字节就死循环/崩溃 —— 这是实时链路的兜底, 不能把整条流水线拖死。

/// 该字节是否是一个码点的起始字节(0xxxxxxx / 110xxxxx / 1110xxxx / 11110xxx)
inline bool is_lead_byte(unsigned char c) { return (c & 0xC0) != 0x80; }

/// 从 pos 开始的那个码点占几个字节(非法序列返回 1, 容错前进)
inline size_t char_len_at(const std::string& s, size_t pos) {
  const unsigned char c = static_cast<unsigned char>(s[pos]);
  size_t n = 1;
  if ((c & 0x80) == 0x00) {
    n = 1;
  } else if ((c & 0xE0) == 0xC0) {
    n = 2;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
  }
  if (pos + n > s.size()) return 1;
  for (size_t i = 1; i < n; ++i) {
    if ((static_cast<unsigned char>(s[pos + i]) & 0xC0) != 0x80) return 1;
  }
  return n;
}

/// 前进 chars 个码点后的字节偏移(不够长就返回 s.size())
inline size_t byte_offset_after_chars(const std::string& s, size_t chars) {
  size_t pos = 0;
  size_t count = 0;
  while (pos < s.size() && count < chars) {
    pos += char_len_at(s, pos);
    ++count;
  }
  return pos;
}

/// 码点个数 —— 也就是"用户眼里的字符数", min_chunk_chars / max_chunk_chars 用它
inline size_t utf8_char_count(const std::string& s) {
  size_t pos = 0;
  size_t count = 0;
  while (pos < s.size()) {
    pos += char_len_at(s, pos);
    ++count;
  }
  return count;
}

// ---------------------------------------------------------------------------
// 标点识别
// ---------------------------------------------------------------------------
// 只收两类"句读级"标点: 中文全角句读标点(，。？！、；：) 与 ASCII 的 , . ? !。
// 引号/括号不收 —— 它们往往成对出现, 在中间断句会把一个结构切成两半。
inline bool is_ascii_punct(char c) {
  switch (c) {
    case ',': case '.': case '?': case '!':
      return true;
    default:
      return false;
  }
}

inline bool is_fullwidth_punct(const std::string& s, size_t pos) {
  static const char* const kPuncts[] = {"，", "。", "？", "！", "、", "；", "："};
  for (const char* p : kPuncts) {
    if (s.compare(pos, 3, p) == 0) return true;
  }
  return false;
}

/// 尾部最后一个"句读级"标点之后的字节偏移; 没有标点返回 npos。
/// 注意返回的是"该标点包含在内"的右边界 —— 提交时标点要跟着一起上屏。
inline size_t last_punct_end(const std::string& s) {
  size_t pos = 0;
  size_t last_end = std::string::npos;
  while (pos < s.size()) {
    const size_t len = char_len_at(s, pos);
    if (len == 1) {
      if (is_ascii_punct(s[pos])) last_end = pos + 1;
    } else if (len == 3) {
      if (is_fullwidth_punct(s, pos)) last_end = pos + 3;
    }
    pos += len;
  }
  return last_end;
}

}  // namespace

// ---------------------------------------------------------------------------
// PartialStabilizer
// ---------------------------------------------------------------------------

PartialStabilizer::PartialStabilizer(Config cfg) : cfg_(cfg) {
  // 配置兜底: 这几个参数直接决定提交节奏, 非法组合会让算法卡死 ——
  // 例如 max_chunk_chars < min_chunk_chars 时, 兜底切出来的片段永远够不到
  // 最小长度门限, 尾部会无限增长。这里把不变式(1 ≤ min ≤ max)夹紧。
  if (cfg_.confirmations < 1) cfg_.confirmations = 1;
  if (cfg_.min_chunk_chars < 1) cfg_.min_chunk_chars = 1;
  if (cfg_.max_chunk_chars < 1) cfg_.max_chunk_chars = 1;
  if (cfg_.max_chunk_chars < cfg_.min_chunk_chars) {
    cfg_.max_chunk_chars = cfg_.min_chunk_chars;
  }
}

/// 处理一个 partial, 产出"本次可提交的片段 + 仍未确认的尾部"。
///
/// 换句(utt_id 变化)时只清"本句"状态: committed_chunks_ 是累计指标(长跑统计
/// 用), 换句不清; 只有显式 reset()(换会话/测试)才归零。
PartialStabilizer::Update PartialStabilizer::on_partial(const AsrPartial& p) {
  Update out;

  if (p.utt_id != utt_id_) {
    utt_id_ = p.utt_id;
    committed_.clear();
    tails_.clear();
    warned_revision_ = false;
  }

  // --- 1. 对齐: 从"整句假设"里剥掉已提交部分, 得到未提交尾部 ---
  // 正常情况下 full 以 committed_ 为前缀(ASR 只是继续往后说)。
  // 但 ASR 有时会改写已经提交过的内容(重新解码/纠错), 这时不能回退:
  // 已经上屏的译文进了用户的眼睛, 撤回只会让人以为设备坏了。
  // 处理方式是退到"公共前缀"处对齐 —— 已提交部分原样保留, 公共前缀之后的
  // 剩余部分当作新的不稳定的尾部(可以拿去做草稿), 但这一帧不提交任何新内容。
  const std::string common = longest_common_prefix_utf8(p.text, committed_);
  const bool revision = common.size() < committed_.size();

  std::string tail;
  if (!revision) {
    tail = p.text.substr(committed_.size());
  } else {
    // 例: committed_="我想去", 新文本="我想坐火车" → 公共前缀"我想",
    // 尾部="坐火车"; 已提交的"我想去"保持不变(哪怕它现在"错了")。
    tail = p.text.substr(common.size());
    if (!warned_revision_) {
      // 每句只警告一次: 长句里 ASR 可能反复改写, 逐次打日志会刷爆 stderr,
      // 反而把真正有用的时间线信息淹掉。
      warned_revision_ = true;
      SL_LOG(Warn) << "ASR 改写已提交区(utt=" << p.utt_id << ", 已提交 "
                   << utf8_char_count(committed_) << " 字), 保留已提交内容不回退";
    }
    // 旧尾部是按旧 committed_ 对齐的, 参考价值已经没了(它们和新文本的
    // 公共前缀会算出一个假的"稳定前缀"), 清掉重新积累一致性。
    tails_.clear();
  }

  // --- 2. 只保留最近 confirmations 个尾部 ---
  // 这是 LocalAgreement-n 的 "n": 更老的假设与当前决策无关, 留着只会占用
  // 内存并让公共前缀被历史垃圾拖短。
  if (!revision) {
    tails_.push_back(tail);
    while (tails_.size() > static_cast<size_t>(cfg_.confirmations)) {
      tails_.erase(tails_.begin());
    }
  }

  // --- 3. 选本次要提交的候选片段 ---
  // min_chars 是候选片段的长度门限。默认是 min_chunk_chars(避免逐字翻译:
  // 1 个字翻一次既费钱又让译文读不通), 但强边界/兜底条件会把它放宽到 1:
  //   - 标点: 语义到这里就是断的, 压着不提交只会让字幕"该断不断";
  //   - max_chunk: 这是延迟兜底, 必须无条件生效;
  //   - final: 整句结束, 剩下多少提交多少。
  std::string candidate;
  size_t min_chars = static_cast<size_t>(cfg_.min_chunk_chars);

  if (p.is_final) {
    candidate = tail;
    min_chars = 0;
  } else if (!revision) {
    if (cfg_.split_at_punctuation) {
      const size_t punct_end = last_punct_end(tail);
      if (punct_end != std::string::npos) {
        // 提交到"最后一个标点(含)"为止: 这句里可能已经有多个标点,
        // 一次把整段主谓结构送出去, 比逐句切更省翻译次数。
        candidate = tail.substr(0, punct_end);
        min_chars = 1;
      }
    }
    if (candidate.empty() &&
        utf8_char_count(tail) >= static_cast<size_t>(cfg_.max_chunk_chars)) {
      // 延迟兜底: confirmations 还没凑齐、也没有标点, 但尾巴已经攒够长了。
      // 硬切 max_chunk_chars 个字(按码点切, 不会切出半个汉字), 剩下的继续攒。
      candidate = tail.substr(
          0, byte_offset_after_chars(tail, static_cast<size_t>(cfg_.max_chunk_chars)));
      min_chars = 1;
    }
    if (candidate.empty() &&
        tails_.size() >= static_cast<size_t>(cfg_.confirmations)) {
      // LocalAgreement: 最近 confirmations 个尾部的最长公共前缀。
      // 只要求"两两一致"不够 —— 用前缀折叠能保证结果同时是所有尾部的真前缀。
      std::string stable = tails_.front();
      for (size_t i = 1; i < tails_.size() && !stable.empty(); ++i) {
        stable = longest_common_prefix_utf8(stable, tails_[i]);
      }
      candidate = stable;
    }
  }

  if (utf8_char_count(candidate) < min_chars) candidate.clear();

  // --- 4. 落地提交 ---
  if (!candidate.empty()) {
    committed_ += candidate;
    // 所有尾部都剥掉刚提交的前缀: 它们从此"相对 committed_"表示, 下一次
    // 交上来的 tail 也是相对 committed_ 的, 两边尺度一致才能求公共前缀。
    for (auto& t : tails_) {
      if (t.size() >= candidate.size() &&
          t.compare(0, candidate.size(), candidate) == 0) {
        t.erase(0, candidate.size());
      }
    }
    ++committed_chunks_;
    out.committed = candidate;
  }

  // --- 5. 返回本次视图 ---
  // tail 是"相对 committed_ 仍未确认的部分", 调度器拿它做草稿翻译。
  if (candidate.empty() || candidate.size() > tail.size() ||
      tail.compare(0, candidate.size(), candidate) != 0) {
    out.tail = tail;
  } else {
    out.tail = tail.substr(candidate.size());
  }
  out.final = p.is_final;
  return out;
}

/// 彻底重置(换会话/测试隔离用): 连累计指标一起归零。
void PartialStabilizer::reset() {
  utt_id_ = 0;
  committed_.clear();
  tails_.clear();
  committed_chunks_ = 0;
  warned_revision_ = false;
}

/// 两个合法 UTF-8 串的最长公共前缀(按码点对齐)。
///
/// 先按字节求公共长度, 再回退到最后一个码点边界: 两个串在某个多字节字符
/// 的后半段仍可能字节相同(例如 3 字节汉字的前两个字节相同), 直接按字节截
/// 就会把字符切半。因为公共部分字节完全相同, 判断边界只看 a 就够了。
std::string PartialStabilizer::longest_common_prefix_utf8(const std::string& a,
                                                         const std::string& b) const {
  const size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  while (i > 0 && i < a.size() && !is_lead_byte(static_cast<unsigned char>(a[i]))) {
    --i;
  }
  return a.substr(0, i);
}

}  // namespace sublens
