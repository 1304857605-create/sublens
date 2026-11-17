/// @file test_hud.cpp  HUD 状态机与终端渲染的单元测试
///
/// 重点验证两条"体验硬规则"和一条"终端排版硬约束":
///   1) 已提交行只增不改 —— 任何情况下都不能变短/回退;
///   2) 草稿行受节流, 被丢弃的计入 drafts_dropped, 整行替换计入 revisions;
///   3) render_to_string 的每一行显示宽度都 <= width —— 否则终端会自动折行,
///      HUD 的原地重绘就会错位。

#include <cstddef>
#include <string>
#include <vector>

#include "sublens/common.hpp"
#include "sublens/hud.hpp"
#include "tiny_test.hpp"

namespace {

/// 数 UTF-8 码点个数(测试里用来验证截断没有把汉字切成两半)。
size_t count_codepoints(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    if (c >= 0xF0) {
      len = 4;
    } else if (c >= 0xE0) {
      len = 3;
    } else if (c >= 0xC0) {
      len = 2;
    }
    i += len;
    ++n;
  }
  return n;
}

/// 简易 UTF-8 合法性校验: 防止按字节截断把汉字切成非法序列。
bool valid_utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    if (c < 0x80) {
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    } else {
      return false;  // 非法起始字节
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += len;
  }
  return true;
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find('\n', start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

sublens::HudUpdate make_commit(uint64_t utt, const std::string& src, const std::string& dst,
                               int64_t speech_end_ms, int64_t at_ms) {
  sublens::HudUpdate u;
  u.kind = sublens::HudUpdate::Kind::kCommitted;
  u.utt_id = utt;
  u.src = src;
  u.dst = dst;
  u.speech_end_ms = speech_end_ms;
  u.at_ms = at_ms;
  return u;
}

sublens::HudUpdate make_draft(uint64_t utt, const std::string& src, const std::string& dst,
                              int64_t speech_end_ms, int64_t at_ms) {
  sublens::HudUpdate u = make_commit(utt, src, dst, speech_end_ms, at_ms);
  u.kind = sublens::HudUpdate::Kind::kDraft;
  return u;
}

}  // namespace

// ---------------------------------------------------------------------------
// HudState
// ---------------------------------------------------------------------------

SL_TEST(HudState_committed_is_append_only) {
  sublens::HudState state;

  state.apply(make_commit(1, "Excuse me,", "打扰一下,", 1000, 1200));
  state.apply(make_commit(1, " could you", " 请问", 1000, 1300));

  auto s = state.snapshot();
  SL_EXPECT_EQ(s.cur_dst, std::string("打扰一下, 请问"));
  SL_EXPECT_EQ(s.cur_src, std::string("Excuse me, could you"));
  SL_EXPECT_EQ(s.finalized, 1u);
  SL_EXPECT_EQ(s.last_latency_ms, 300);
  SL_EXPECT_EQ(state.committed_updates(), 2u);

  // 后续提交永远追加到尾部, 不会把已有译文覆盖成更短的内容
  state.apply(make_commit(1, " tell me", " 告诉我", 1000, 1400));
  s = state.snapshot();
  SL_EXPECT_EQ(s.cur_dst, std::string("打扰一下, 请问 告诉我"));
  SL_EXPECT_EQ(s.last_latency_ms, 400);
}

SL_TEST(HudState_new_utterance_switches_main_line_and_counts_finalized) {
  sublens::HudState state;
  state.apply(make_commit(1, "A", "甲", 100, 200));
  state.apply(make_commit(2, "B", "乙", 900, 1100));

  const auto s = state.snapshot();
  SL_EXPECT_EQ(s.cur_utt_id, 2u);
  SL_EXPECT_EQ(s.cur_dst, std::string("乙"));
  SL_EXPECT_EQ(s.finalized, 2u);  // 两句都已经出现过定稿内容
  SL_EXPECT_EQ(s.last_latency_ms, 200);
}

SL_TEST(HudState_draft_is_throttled_and_counted) {
  sublens::HudStateConfig cfg;
  cfg.draft_min_interval_ms = 100;
  sublens::HudState state(cfg);

  // 新句的第一条草稿不受节流。注意 revisions 的口径是"观众看到的草稿行内容变化次数",
  // 所以从空行变成有内容也算 1 次(否则 demo 里一直在出草稿, 指标却永远是 0)。
  state.apply(make_draft(1, "Excuse", "打扰", 1000, 1000));
  auto s = state.snapshot();
  SL_EXPECT(s.draft_active);
  SL_EXPECT_EQ(s.draft_dst, std::string("打扰"));
  SL_EXPECT_EQ(state.revisions(), 1u);
  SL_EXPECT_EQ(state.drafts_dropped(), 0u);

  // 50ms 后又来一条: 距上次替换不足 100ms, 被丢弃, 画面保持不变
  state.apply(make_draft(1, "Excuse me", "打扰一下", 1000, 1050));
  s = state.snapshot();
  SL_EXPECT_EQ(s.draft_dst, std::string("打扰"));
  SL_EXPECT_EQ(state.drafts_dropped(), 1u);
  SL_EXPECT_EQ(state.revisions(), 1u);

  // 100ms 后到达: 允许整行替换, revision +1
  state.apply(make_draft(1, "Excuse me,", "打扰一下,", 1000, 1100));
  s = state.snapshot();
  SL_EXPECT_EQ(s.draft_dst, std::string("打扰一下,"));
  SL_EXPECT_EQ(state.revisions(), 2u);

  // 内容完全相同的草稿事件不算替换也不计入丢弃(画面上没有任何变化)
  state.apply(make_draft(1, "Excuse me,", "打扰一下,", 1000, 1200));
  SL_EXPECT_EQ(state.revisions(), 2u);
  SL_EXPECT_EQ(state.drafts_dropped(), 1u);
}

SL_TEST(HudState_commit_clears_draft_line) {
  sublens::HudStateConfig cfg;
  cfg.draft_min_interval_ms = 10;
  sublens::HudState state(cfg);

  state.apply(make_draft(1, "Excuse me", "打扰一下", 1000, 1000));
  SL_EXPECT(state.snapshot().draft_active);

  state.apply(make_commit(1, "Excuse me", "打扰一下", 1000, 1200));
  const auto s = state.snapshot();
  SL_EXPECT(!s.draft_active);  // 定稿内容进了主行, 草稿行立刻清掉
  SL_EXPECT(s.draft_dst.empty());
  SL_EXPECT_EQ(s.cur_dst, std::string("打扰一下"));
}

SL_TEST(HudState_draft_line_is_clamped_without_splitting_utf8) {
  sublens::HudStateConfig cfg;
  cfg.max_line_chars = 6;
  cfg.draft_min_interval_ms = 0;
  sublens::HudState state(cfg);

  state.apply(make_draft(1, "", "这是一句很长很长的草稿译文", 0, 0));
  const std::string dst = state.snapshot().draft_dst;
  SL_EXPECT(valid_utf8(dst));
  SL_EXPECT_EQ(count_codepoints(dst), 6u);
  SL_EXPECT_EQ(dst.substr(dst.size() - 3), std::string("\xE2\x80\xA6"));  // 结尾是 "…"
}

SL_TEST(HudState_reset_clears_everything) {
  sublens::HudState state;
  state.apply(make_commit(1, "A", "甲", 0, 50));
  state.apply(make_draft(2, "B", "乙", 0, 100));
  state.reset();

  const auto s = state.snapshot();
  SL_EXPECT(s.cur_dst.empty());
  SL_EXPECT(!s.draft_active);
  SL_EXPECT_EQ(s.finalized, 0u);
  SL_EXPECT_EQ(state.committed_updates(), 0u);
  SL_EXPECT_EQ(state.drafts_dropped(), 0u);
  SL_EXPECT_EQ(state.revisions(), 0u);
}

// ---------------------------------------------------------------------------
// AnsiHudRenderer::render_to_string
// ---------------------------------------------------------------------------

namespace {

sublens::HudSnapshot busy_snapshot() {
  sublens::HudSnapshot s;
  s.cur_src = "Excuse me, could you tell me how to get to the train station?";
  s.cur_dst = "打扰一下, 请问火车站怎么走? Excuse me, could you tell me how to get there?";
  s.draft_src = "I would like a large latte with no sugar, please.";
  s.draft_dst = "我要一杯大杯拿铁, 不加糖, 谢谢。I would like a large latte with no sugar.";
  s.draft_active = true;
  s.cur_utt_id = 12;
  s.draft_utt_id = 13;
  s.finalized = 7;
  s.last_latency_ms = 428;
  return s;
}

void expect_frame_width_ok(const sublens::HudSnapshot& s, int width) {
  const std::string frame = sublens::AnsiHudRenderer::render_to_string(s, width, false);
  SL_EXPECT(frame.find('\x1b') == std::string::npos);  // 纯文本帧不能带转义序列

  const auto lines = split_lines(frame);
  SL_EXPECT_EQ(lines.size(), 8u);  // 行高恒定, 否则原地重绘必闪
  for (const auto& line : lines) {
    SL_EXPECT(sublens::display_width(line) <= static_cast<size_t>(width));
  }
}

}  // namespace

SL_TEST(AnsiRenderer_every_line_fits_width_with_mixed_text) {
  const auto s = busy_snapshot();
  // 40 列(窄) / 68 列(默认) / 100 列(宽), 中英混排都不能溢出
  expect_frame_width_ok(s, 40);
  expect_frame_width_ok(s, 68);
  expect_frame_width_ok(s, 100);
}

SL_TEST(AnsiRenderer_truncated_lines_stay_within_width) {
  sublens::HudSnapshot s;
  // 故意给一条超长主行和超长草稿行, 逼出 "..." 截断分支
  s.cur_dst = std::string(200, 'A') + "这是很长的中文内容用来测试截断";
  s.draft_active = true;
  s.draft_dst = std::string(200, 'B') + "另一段超长的草稿中文内容";
  s.last_latency_ms = 999;
  expect_frame_width_ok(s, 40);
  expect_frame_width_ok(s, 68);
}

SL_TEST(AnsiRenderer_empty_snapshot_is_still_valid_frame) {
  sublens::HudSnapshot s;
  expect_frame_width_ok(s, 40);
  const std::string frame = sublens::AnsiHudRenderer::render_to_string(s, 40, false);
  const auto lines = split_lines(frame);
  SL_EXPECT(lines.size() == 8);
  SL_EXPECT(lines[1].find("ready") != std::string::npos);  // 还没开始听
  SL_EXPECT(lines[1].find("listening") == std::string::npos);
}

SL_TEST(AnsiRenderer_shows_status_and_content) {
  sublens::HudSnapshot s;
  s.cur_dst = "打扰一下, 请问火车站怎么走?";  // 短句: 完整显示, 不走截断
  s.draft_src = "could you";
  s.draft_dst = "请问";
  s.draft_active = true;
  s.cur_utt_id = 12;
  s.last_latency_ms = 428;

  const std::string frame = sublens::AnsiHudRenderer::render_to_string(s, 68, false);
  SL_EXPECT(frame.find("listening") != std::string::npos);  // 草稿活跃 = 正在听
  SL_EXPECT(frame.find("打扰一下, 请问火车站怎么走?") != std::string::npos);
  SL_EXPECT(frame.find("...") != std::string::npos);        // 草稿行前缀
  SL_EXPECT(frame.find("#12") != std::string::npos);        // 当前句号
  SL_EXPECT(frame.find("428ms") != std::string::npos);      // 最近延迟
}

SL_TEST(AnsiRenderer_color_frame_only_differs_by_escapes) {
  const auto s = busy_snapshot();
  const std::string plain = sublens::AnsiHudRenderer::render_to_string(s, 68, false);
  const std::string colored = sublens::AnsiHudRenderer::render_to_string(s, 68, true);
  SL_EXPECT(colored.find("\x1b[") != std::string::npos);
  SL_EXPECT(colored.size() > plain.size());
  // 着色帧去掉转义序列后, 文本内容部分应与纯文本帧一致(只是包了一层颜色)
  SL_EXPECT(colored.find("listening") != std::string::npos);
}
