/// @file test_stabilizer.cpp
/// @brief LocalAgreement-n 稳定前缀对齐的单元测试。
///
/// 重点覆盖"用户能看到的行为"而不是内部实现:
///   - 增长序列里只有"连续 N 次一致"的前缀才会被提交;
///   - 标点是强制断句点, 不用等一致性;
///   - final 一定要把剩下的全提交(不能吞字);
///   - 任何一次提交都必须是合法 UTF-8, 且所有提交拼起来 == 原文(不丢不重);
///   - ASR 改写已提交区时, 已提交内容绝不回退。

#include <cstddef>
#include <string>
#include <vector>

#include "sublens/sched.hpp"
#include "tiny_test.hpp"

namespace {

using sublens::AsrPartial;
using sublens::PartialStabilizer;
using sublens::StabilizerConfig;

AsrPartial partial(uint64_t utt, std::string text, bool final = false) {
  AsrPartial p;
  p.utt_id = utt;
  p.text = std::move(text);
  p.is_final = final;
  p.speech_end_ms = 1000;  // 测试里不关心, 给个非零值免得被当成"未知"
  return p;
}

StabilizerConfig cfg(int confirmations, int min_chars, int max_chars, bool punct) {
  StabilizerConfig c;
  c.confirmations = confirmations;
  c.min_chunk_chars = min_chars;
  c.max_chunk_chars = max_chars;
  c.split_at_punctuation = punct;
  return c;
}

/// 校验一段文本是合法 UTF-8: 首字节合法、续字节个数正确、没有被截断。
/// 这是"绝不切半个汉字"最直接的断言方式 —— 切半了一定非法。
bool is_valid_utf8(const std::string& s) {
  size_t pos = 0;
  while (pos < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    size_t n = 0;
    if (c < 0x80) {
      n = 1;
    } else if ((c & 0xE0) == 0xC0) {
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      n = 3;
    } else if ((c & 0xF8) == 0xF0) {
      n = 4;
    } else {
      return false;  // 0x80~0xBF 作为首字节 = 被截断的续字节
    }
    if (pos + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(s[pos + k]) & 0xC0) != 0x80) return false;
    }
    pos += n;
  }
  return true;
}

/// 码点个数(断言"按字提交"而不是按字节)
size_t char_count(const std::string& s) {
  size_t pos = 0;
  size_t count = 0;
  while (pos < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    size_t n = 1;
    if ((c & 0xE0) == 0xC0) {
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      n = 3;
    } else if ((c & 0xF8) == 0xF0) {
      n = 4;
    }
    pos += n;
    ++count;
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 增长序列: 只有连续 confirmations 次一致的前缀才提交
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_growing_partials_commit_stable_prefix) {
  PartialStabilizer stab(cfg(/*confirmations=*/2, /*min=*/2, /*max=*/64, false));

  // 每一步都断言"提交了什么 + 还剩什么", 这样这个用例本身就是算法说明:
  // 第 k 个 partial 交上来时, 只有第 k-1 个 partial 里也有的那部分才稳定。
  const std::vector<std::string> seq = {"我", "我想", "我想去",
                                        "我想去火", "我想去火车", "我想去火车站"};
  const std::vector<std::string> expect_commit = {"", "", "我想", "", "去火", ""};
  const std::vector<std::string> expect_tail = {"我", "我想", "去",
                                                "去火", "车", "车站"};

  std::string committed_all;
  for (size_t i = 0; i < seq.size(); ++i) {
    const auto u = stab.on_partial(partial(7, seq[i]));
    SL_EXPECT_EQ(u.committed, expect_commit[i]);
    SL_EXPECT_EQ(u.tail, expect_tail[i]);
    SL_EXPECT(!u.final);
    SL_EXPECT(is_valid_utf8(u.committed));
    committed_all += u.committed;
  }
  // 一致性门限把"最新的一个字"永远押后一格: 这是延迟与稳定的交换.
  SL_EXPECT_EQ(committed_all, std::string("我想去火"));
  SL_EXPECT_EQ(stab.committed_chunks(), 2u);

  // 定稿: 剩下的"车站"全部提交, 一个字都不留.
  const auto fin = stab.on_partial(partial(7, "我想去火车站", true));
  SL_EXPECT_EQ(fin.committed, std::string("车站"));
  SL_EXPECT(fin.final);
  SL_EXPECT(fin.tail.empty());
  SL_EXPECT_EQ(stab.committed_chunks(), 3u);
}

// ---------------------------------------------------------------------------
// 2. 标点: 语义强边界, 优先于 LocalAgreement, 且只要求 >=1 字
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_punctuation_forces_commit) {
  // confirmations=3 时一致性远未成立, 但标点一到就按语义断句提交。
  // min_chunk_chars=4 用来证明"标点强制提交只要求 >=1 字"。
  PartialStabilizer stab(cfg(/*confirmations=*/3, /*min=*/4, /*max=*/64, true));

  const auto u1 = stab.on_partial(partial(1, "第一句。第二句"));
  SL_EXPECT_EQ(u1.committed, std::string("第一句。"));  // 提交到最后一个标点(含)
  SL_EXPECT_EQ(u1.tail, std::string("第二句"));

  const auto u2 = stab.on_partial(partial(1, "第一句。第二句，"));
  SL_EXPECT_EQ(u2.committed, std::string("第二句，"));  // 2 字 < min=4 也提交
  SL_EXPECT(u2.tail.empty());

  // 半角标点同样生效; 标点后的空格属于未确认尾部, 不能吞掉.
  const auto u3 = stab.on_partial(partial(2, "ok! rest"));
  SL_EXPECT_EQ(u3.committed, std::string("ok!"));
  SL_EXPECT_EQ(u3.tail, std::string(" rest"));

  // 关闭标点切分后, 同样的输入不再强制提交（等一致性）。
  PartialStabilizer no_punct(cfg(/*confirmations=*/3, /*min=*/2, /*max=*/64, false));
  const auto u4 = no_punct.on_partial(partial(1, "第一句。第二句"));
  SL_EXPECT(u4.committed.empty());
}

// ---------------------------------------------------------------------------
// 3. 定稿: 不管一致性/最小长度, 剩下多少提交多少
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_final_flushes_everything) {
  PartialStabilizer stab(cfg(/*confirmations=*/5, /*min=*/3, /*max=*/64, true));
  const auto u1 = stab.on_partial(partial(1, "你好 世界"));
  SL_EXPECT(u1.committed.empty());
  SL_EXPECT_EQ(u1.tail, std::string("你好 世界"));

  // final 时剩下 5 个字, 一次全提交; 否则这几个字永远等不到"下一次一致性".
  const auto u2 = stab.on_partial(partial(1, "你好 世界。", true));
  SL_EXPECT(u2.final);
  SL_EXPECT_EQ(u2.committed, std::string("你好 世界。"));
  SL_EXPECT(u2.tail.empty());
  SL_EXPECT_EQ(stab.committed_chunks(), 1u);

  // 已经提交完再来的 final(空尾巴)不应产生空提交.
  const auto u3 = stab.on_partial(partial(1, "你好 世界。", true));
  SL_EXPECT(u3.committed.empty());
  SL_EXPECT(u3.final);
  SL_EXPECT_EQ(stab.committed_chunks(), 1u);
}

// ---------------------------------------------------------------------------
// 4. UTF-8: max_chunk 强制切分也绝不切半个汉字
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_utf8_never_splits_multibyte) {
  // confirmations 很大 → 一致性永远不成立, 只能靠 max_chunk=2 强制切。
  // 如果实现按"字节"数切, 第一个片段会变成"你" + 半个"好" = 非法 UTF-8。
  PartialStabilizer stab(cfg(/*confirmations=*/99, /*min=*/1, /*max=*/2, false));

  std::vector<std::string> commits;
  const std::vector<std::string> seq = {"你好世", "你好世界", "你好世界啊"};
  for (const auto& s : seq) {
    const auto u = stab.on_partial(partial(1, s));
    SL_EXPECT(is_valid_utf8(u.committed));
    SL_EXPECT(is_valid_utf8(u.tail));
    if (!u.committed.empty()) commits.push_back(u.committed);
  }
  const auto fin = stab.on_partial(partial(1, "你好世界啊", true));
  if (!fin.committed.empty()) commits.push_back(fin.committed);

  SL_EXPECT_EQ(commits.size(), 3u);
  SL_EXPECT_EQ(commits[0], std::string("你好"));  // 恰好 2 个码点 = 6 字节
  SL_EXPECT_EQ(commits[1], std::string("世界"));
  SL_EXPECT_EQ(commits[2], std::string("啊"));
  SL_EXPECT_EQ(char_count(commits[0]), 2u);

  std::string all;
  for (const auto& c : commits) {
    SL_EXPECT(is_valid_utf8(c));
    all += c;
  }
  SL_EXPECT_EQ(all, std::string("你好世界啊"));  // 不丢字、不重复
}

// ---------------------------------------------------------------------------
// 5. ASR 改写已提交区: 已提交内容不回退, 也不重复提交
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_asr_revision_keeps_committed) {
  PartialStabilizer stab(cfg(/*confirmations=*/2, /*min=*/2, /*max=*/64, false));

  const auto u1 = stab.on_partial(partial(3, "我想去"));
  SL_EXPECT(u1.committed.empty());  // 只有一份假设, 一致性不足

  const auto u2 = stab.on_partial(partial(3, "我想去火"));
  SL_EXPECT_EQ(u2.committed, std::string("我想去"));
  SL_EXPECT_EQ(u2.tail, std::string("火"));

  // ASR 把已提交的"去"改写成了"坐": 公共前缀只有"我想"。
  // 已提交内容必须原样保留(哪怕现在看起来"错"了), 这一帧不提交任何新内容,
  // 公共前缀之后的"坐火车"只作为草稿素材返回。
  const auto u3 = stab.on_partial(partial(3, "我想坐火车"));
  SL_EXPECT(u3.committed.empty());
  SL_EXPECT_EQ(u3.tail, std::string("坐火车"));
  SL_EXPECT_EQ(stab.committed_chunks(), 1u);

  // 完全不共享前缀(整句被重新识别)也一样: 不回退、不崩。
  const auto u4 = stab.on_partial(partial(3, "你要去哪儿"));
  SL_EXPECT(u4.committed.empty());
  SL_EXPECT_EQ(u4.tail, std::string("你要去哪儿"));
  SL_EXPECT_EQ(stab.committed_chunks(), 1u);

  // ASR 又"改回"正确文本: 从已提交处继续, 不会把已提交的"我想去"再交一遍。
  const auto u5 = stab.on_partial(partial(3, "我想去火车站"));
  SL_EXPECT(u5.committed.empty());  // 改写时尾部历史被清空, 只剩一份假设
  SL_EXPECT_EQ(u5.tail, std::string("火车站"));

  const auto fin = stab.on_partial(partial(3, "我想去火车站", true));
  SL_EXPECT_EQ(fin.committed, std::string("火车站"));
  SL_EXPECT(fin.final);
  SL_EXPECT_EQ(stab.committed_chunks(), 2u);
}

// ---------------------------------------------------------------------------
// 6. max_chunk 兜底: 没有标点、一致性也不成立时按长度强制切分
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_max_chunk_forces_commit) {
  // confirmations=99 意味着"永远等不到一致性"; 没有标点。若不按 max_chunk 切,
  // 尾部会一直涨, 端到端延迟无界 —— 这是实时字幕不能接受的。
  PartialStabilizer stab(cfg(/*confirmations=*/99, /*min=*/2, /*max=*/4, false));

  const auto u1 = stab.on_partial(partial(1, "一二三"));
  SL_EXPECT(u1.committed.empty());  // 3 字, 还没到 4

  const auto u2 = stab.on_partial(partial(1, "一二三四五"));
  SL_EXPECT_EQ(u2.committed, std::string("一二三四"));
  SL_EXPECT_EQ(u2.tail, std::string("五"));

  const auto u3 = stab.on_partial(partial(1, "一二三四五六七八"));
  SL_EXPECT_EQ(u3.committed, std::string("五六七八"));
  SL_EXPECT(u3.tail.empty());

  // 尾巴已经空了, final 不该再产生一次空提交。
  const auto fin = stab.on_partial(partial(1, "一二三四五六七八", true));
  SL_EXPECT(fin.committed.empty());
  SL_EXPECT(fin.final);
  SL_EXPECT_EQ(stab.committed_chunks(), 2u);
}

// ---------------------------------------------------------------------------
// 7. 改写恰好发生在定稿帧: 已提交不回退, 但句尾的新内容仍必须提交
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_revision_on_final_flushes_suffix) {
  PartialStabilizer stab(cfg(/*confirmations=*/2, /*min=*/2, /*max=*/64, false));
  stab.on_partial(partial(5, "我想去"));
  const auto u2 = stab.on_partial(partial(5, "我想去火"));
  SL_EXPECT_EQ(u2.committed, std::string("我想去"));

  // 定稿这一帧 ASR 把已提交的"去"改成了"坐"。公共前缀只有"我想":
  // 已提交的"我想去"原样保留(不回退), 但这已经是最后机会 —— 公共前缀之后
  // 的"坐火车"必须提交, 否则句尾永远丢失。取舍: 宁可主行里残留一个旧字,
  // 也不能让用户少看到半句。
  const auto fin = stab.on_partial(partial(5, "我想坐火车", /*final=*/true));
  SL_EXPECT(fin.final);
  SL_EXPECT_EQ(fin.committed, std::string("坐火车"));
  SL_EXPECT(fin.tail.empty());
  SL_EXPECT_EQ(stab.committed_chunks(), 2u);
}

// ---------------------------------------------------------------------------
// 8. 换句/重置: 上一句的状态绝不能带进下一句
// ---------------------------------------------------------------------------
SL_TEST(Stabilizer_new_utterance_resets_sentence_state) {
  PartialStabilizer stab(cfg(/*confirmations=*/2, /*min=*/1, /*max=*/64, false));
  stab.on_partial(partial(1, "ab"));
  const auto u2 = stab.on_partial(partial(1, "abc"));
  SL_EXPECT_EQ(u2.committed, std::string("ab"));

  // 换句: committed_ 必须清空, 否则上一句的 "ab" 会被当成本句的已提交前缀,
  // 本句开头的 "ab" 就永远提交不出来了。
  const auto u3 = stab.on_partial(partial(2, "ab"));
  SL_EXPECT(u3.committed.empty());
  SL_EXPECT_EQ(u3.tail, std::string("ab"));

  const auto u4 = stab.on_partial(partial(2, "abc"));
  SL_EXPECT_EQ(u4.committed, std::string("ab"));  // 只按本句自己的公共前缀提交

  // 显式 reset 连累计指标一起归零(换会话/测试隔离用)。
  SL_EXPECT_EQ(stab.committed_chunks(), 2u);
  stab.reset();
  SL_EXPECT_EQ(stab.committed_chunks(), 0u);
  const auto u5 = stab.on_partial(partial(9, "xy"));
  SL_EXPECT(u5.committed.empty());
  SL_EXPECT_EQ(u5.tail, std::string("xy"));
}
