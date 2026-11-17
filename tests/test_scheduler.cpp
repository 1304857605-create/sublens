/// @file test_scheduler.cpp
/// @brief 推测式翻译调度(正式/草稿双路 + 过时草稿丢弃 + 前缀重译差量追加)的单元测试。
///
/// 用假 MT 后端把"云侧往返"变成可控的: submit 只记录请求, 由测试决定何时
/// 用哪个顺序把结果"送回来", 于是过时判定、节流、上屏顺序都能确定性断言。
///
/// 另外专门测一次 sync=true 的后端(submit 内部同步回调): 这验证调度器没有
/// 在持锁时调 mt_.submit —— 否则会自己锁死自己。
///
/// 正式路现在发"该句累计原文前缀"(带上下文重译), 结果只追加与上次完整译文
/// 公共前缀之后的新增部分; 用例 9/10 固化这条"质量换成本 + 只增不改"的路径。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "sublens/sched.hpp"
#include "tiny_test.hpp"

namespace {

using sublens::AsrPartial;
using sublens::HudUpdate;
using sublens::MtRequest;
using sublens::MtResult;
using sublens::SchedulerConfig;
using sublens::TranslationScheduler;

AsrPartial partial(uint64_t utt, std::string text, bool final = false,
                   int64_t speech_end_ms = 1000) {
  AsrPartial p;
  p.utt_id = utt;
  p.text = std::move(text);
  p.is_final = final;
  p.speech_start_ms = speech_end_ms - 800;
  p.speech_end_ms = speech_end_ms;
  return p;
}

/// 假 MT 后端。
///   - 默认(异步): submit 只入队, 测试用 reply(i) 手动"返回"任意一条结果;
///   - sync=true: submit 内部立刻同步回调, 模拟"极快的本地引擎"或同步实现.
class FakeMt : public sublens::MtBackend {
 public:
  explicit FakeMt(bool sync = false) : sync_(sync) {}

  void set_callback(Callback cb) override { cb_ = std::move(cb); }
  bool start() override { return true; }
  void submit(MtRequest req) override {
    requests.push_back(std::move(req));
    if (sync_) reply(requests.size() - 1);
  }
  void flush() override {}
  void stop() override {}
  const char* name() const override { return "fake-mt"; }

  /// 返回第 i 条请求的结果(可以乱序/延迟, 用来模拟云侧往返)。
  void reply(size_t i) {
    if (!cb_) return;
    const MtRequest& req = requests.at(i);
    MtResult r;
    r.utt_id = req.utt_id;
    // 默认译文 = "T:" + 原文: 原文前缀变长时译文也单调变长, 适合直接观察差量。
    // 需要模拟"整句重译改写了尾部"的测试会覆盖 translate。
    r.text = translate ? translate(req.text) : ("T:" + req.text);
    r.draft = req.draft;
    r.emit_ms = sublens::now_ms();
    cb_(r);
  }

  std::vector<MtRequest> requests;
  /// 可选: 自定义"原文 → 译文"映射(整句前缀重译的测试用它模拟真实 MT)。
  std::function<std::string(const std::string&)> translate;

 private:
  bool sync_ = false;
  Callback cb_;
};

SchedulerConfig sched_cfg(int confirmations, int min_chars, int max_chars,
                          bool draft_enabled, int draft_throttle_ms = 150,
                          int max_draft_chars = 48) {
  SchedulerConfig c;
  c.stabilizer.confirmations = confirmations;
  c.stabilizer.min_chunk_chars = min_chars;
  c.stabilizer.max_chunk_chars = max_chars;
  c.stabilizer.split_at_punctuation = false;  // 本文件只测调度, 关掉标点切分
  c.draft_enabled = draft_enabled;
  c.draft_throttle_ms = draft_throttle_ms;
  c.max_draft_chars = max_draft_chars;
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 正式提交 → kCommitted 上屏; 请求发整句前缀、上屏只追加差量
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_commit_emits_committed_update) {
  FakeMt mt;
  TranslationScheduler sched(mt, sched_cfg(/*conf=*/2, /*min=*/2, /*max=*/64,
                                           /*draft=*/false));
  // 回调注册是 Pipeline 的职责(调度器自己不去抢 MtBackend 的回调槽):
  // 这里模拟 Pipeline 的接线, 之后 reply() 才会真正回到 on_mt_result。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  sched.on_asr_partial(partial(1, "你好"));
  SL_EXPECT(out.empty());
  SL_EXPECT_EQ(mt.requests.size(), 0u);  // 一致性不足, 不提交

  sched.on_asr_partial(partial(1, "你好世界"));
  SL_EXPECT_EQ(mt.requests.size(), 1u);
  // 请求文本是"该句累计原文前缀"(这里是第一个提交, 所以等于片段本身)。
  SL_EXPECT_EQ(mt.requests[0].text, std::string("你好"));
  SL_EXPECT(!mt.requests[0].draft);
  SL_EXPECT(out.empty());  // 译文还没回来, 绝不能先上屏

  mt.reply(0);
  SL_EXPECT_EQ(out.size(), 1u);
  SL_EXPECT(out[0].kind == HudUpdate::Kind::kCommitted);
  SL_EXPECT_EQ(out[0].utt_id, 1u);
  SL_EXPECT_EQ(out[0].src, std::string("你好"));  // src 仍是本次片段(契约)
  SL_EXPECT_EQ(out[0].dst, std::string("T:你好"));  // 第一次没有参照译文 → 整段
  SL_EXPECT_EQ(out[0].speech_end_ms, 1000);

  // final 把剩下的"世界"一次性提交。注意请求发的是累计前缀"你好世界",
  // 不是片段"世界" —— MT 要在整句上下文里重译。
  sched.on_asr_partial(partial(1, "你好世界", /*final=*/true));
  SL_EXPECT_EQ(mt.requests.size(), 2u);
  SL_EXPECT_EQ(mt.requests[1].text, std::string("你好世界"));
  mt.reply(1);
  SL_EXPECT_EQ(out.size(), 2u);
  SL_EXPECT(out[1].kind == HudUpdate::Kind::kCommitted);
  SL_EXPECT_EQ(out[1].src, std::string("世界"));
  // 新译文 "T:你好世界" 与上次完整译文 "T:你好" 的公共前缀之后, 只追加"世界"。
  SL_EXPECT_EQ(out[1].dst, std::string("世界"));

  const auto st = sched.stats();
  SL_EXPECT_EQ(st.partials, 3u);
  SL_EXPECT_EQ(st.commits, 2u);
  SL_EXPECT_EQ(st.drafts, 0u);
  SL_EXPECT_EQ(st.mt_results, 2u);
}

// ---------------------------------------------------------------------------
// 2. 草稿节流: 连续 5 个 partial 只产生 1 次草稿请求
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_draft_is_throttled) {
  FakeMt mt;
  // min_chunk 拉得很高 ⇒ 永远不提交正式译文, 这样能干净地只看草稿行为。
  TranslationScheduler sched(
      mt, sched_cfg(/*conf=*/5, /*min=*/99, /*max=*/99, /*draft=*/true,
                    /*draft_throttle_ms=*/10000, /*max_draft_chars=*/48));
  // 回调注册是 Pipeline 的职责(调度器自己不去抢 MtBackend 的回调槽):
  // 这里模拟 Pipeline 的接线, 之后 reply() 才会真正回到 on_mt_result。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  const std::vector<std::string> seq = {"一", "一二", "一二三", "一二三四", "一二三四五"};
  for (const auto& s : seq) sched.on_asr_partial(partial(3, s));

  // 第一个 partial 发草稿(last_draft_ms=0 表示还没发过, 不受节流限制),
  // 之后 4 个都在节流窗口内 → 只有 1 次草稿请求。
  SL_EXPECT_EQ(mt.requests.size(), 1u);
  SL_EXPECT(mt.requests[0].draft);
  SL_EXPECT_EQ(mt.requests[0].text, std::string("一"));
  const auto st = sched.stats();
  SL_EXPECT_EQ(st.partials, 5u);
  SL_EXPECT_EQ(st.commits, 0u);
  SL_EXPECT_EQ(st.drafts, 1u);
  SL_EXPECT(out.empty());
}

// ---------------------------------------------------------------------------
// 3. 草稿太长时不做: 尾部超过 max_draft_chars 没有阅读价值
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_skips_long_draft) {
  FakeMt mt;
  TranslationScheduler sched(
      mt, sched_cfg(/*conf=*/99, /*min=*/99, /*max=*/99, /*draft=*/true,
                    /*draft_throttle_ms=*/0, /*max_draft_chars=*/3));

  sched.on_asr_partial(partial(4, "一二三四"));  // 4 字 > 3, 不做草稿
  SL_EXPECT_EQ(mt.requests.size(), 0u);

  // ASR 把未提交的尾部改短了(还没提交过任何内容, 不算改写已提交区):
  // 3 字 ≤ 上限, 这时才值得做一次草稿。
  sched.on_asr_partial(partial(4, "一二三"));
  SL_EXPECT_EQ(mt.requests.size(), 1u);
  SL_EXPECT(mt.requests[0].draft);
  SL_EXPECT_EQ(mt.requests[0].text, std::string("一二三"));
}

// ---------------------------------------------------------------------------
// 4. 过时草稿必须丢弃: 正式提交之后, 旧草稿绝不能再上屏
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_stale_draft_is_dropped) {
  FakeMt mt;
  TranslationScheduler sched(
      mt, sched_cfg(/*conf=*/2, /*min=*/2, /*max=*/64, /*draft=*/true,
                    /*draft_throttle_ms=*/0, /*max_draft_chars=*/48));
  // 回调注册是 Pipeline 的职责(调度器自己不去抢 MtBackend 的回调槽):
  // 这里模拟 Pipeline 的接线, 之后 reply() 才会真正回到 on_mt_result。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  // p1: 只有一份假设 → 无正式提交; 草稿"你好"发出去 (requests[0])
  sched.on_asr_partial(partial(1, "你好"));
  SL_EXPECT_EQ(mt.requests.size(), 1u);
  SL_EXPECT(mt.requests[0].draft);

  // p2: 出现正式提交"你好" (requests[1]), 未确认尾部"世界"再发草稿 (requests[2])
  sched.on_asr_partial(partial(1, "你好世界"));
  SL_EXPECT_EQ(mt.requests.size(), 3u);
  SL_EXPECT(!mt.requests[1].draft);
  SL_EXPECT_EQ(mt.requests[1].text, std::string("你好"));
  SL_EXPECT(mt.requests[2].draft);
  SL_EXPECT_EQ(mt.requests[2].text, std::string("世界"));

  // 旧草稿结果晚到: 它基于的原文已被正式提交覆盖 → 丢弃, 不上屏。
  mt.reply(0);
  SL_EXPECT(out.empty());
  SL_EXPECT_EQ(sched.stats().stale_drafts, 1u);
  SL_EXPECT_EQ(sched.stats().drafts_dropped, 1u);

  // 正式译文先上屏, 然后新草稿才允许更新草稿行(顺序单调)。
  mt.reply(1);
  SL_EXPECT_EQ(out.size(), 1u);
  SL_EXPECT(out[0].kind == HudUpdate::Kind::kCommitted);
  SL_EXPECT_EQ(out[0].src, std::string("你好"));

  mt.reply(2);
  SL_EXPECT_EQ(out.size(), 2u);
  SL_EXPECT(out[1].kind == HudUpdate::Kind::kDraft);
  SL_EXPECT_EQ(out[1].src, std::string("世界"));  // 配回的是这条草稿的原文
  SL_EXPECT_EQ(out[1].dst, std::string("T:世界"));

  const auto st = sched.stats();
  SL_EXPECT_EQ(st.drafts, 2u);
  SL_EXPECT_EQ(st.commits, 1u);
  SL_EXPECT_EQ(st.mt_results, 1u);
}

// ---------------------------------------------------------------------------
// 5. 定稿之后: 该句任何草稿都不许再上屏
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_draft_after_final_is_stale) {
  FakeMt mt;
  TranslationScheduler sched(
      mt, sched_cfg(/*conf=*/99, /*min=*/99, /*max=*/99, /*draft=*/true,
                    /*draft_throttle_ms=*/0, /*max_draft_chars=*/48));
  // 回调注册是 Pipeline 的职责(调度器自己不去抢 MtBackend 的回调槽):
  // 这里模拟 Pipeline 的接线, 之后 reply() 才会真正回到 on_mt_result。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  sched.on_asr_partial(partial(1, "测试"));  // requests[0] = 草稿
  // final 把所有剩余内容一次提交 (requests[1]), 并给该句打上定稿标记。
  sched.on_asr_partial(partial(1, "测试一", /*final=*/true));
  SL_EXPECT_EQ(mt.requests.size(), 2u);
  SL_EXPECT(mt.requests[0].draft);
  SL_EXPECT(!mt.requests[1].draft);
  SL_EXPECT_EQ(mt.requests[1].text, std::string("测试一"));

  mt.reply(0);  // 草稿晚于定稿到达 → 丢弃
  SL_EXPECT(out.empty());
  SL_EXPECT_EQ(sched.stats().stale_drafts, 1u);

  mt.reply(1);  // 定稿译文正常上屏
  SL_EXPECT_EQ(out.size(), 1u);
  SL_EXPECT(out[0].kind == HudUpdate::Kind::kCommitted);
  SL_EXPECT_EQ(out[0].src, std::string("测试一"));
}

// ---------------------------------------------------------------------------
// 6. 同步回调的 MT 后端: 不能在持锁时 submit(否则死锁)
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_handles_synchronous_mt_callback) {
  FakeMt mt(/*sync=*/true);
  TranslationScheduler sched(mt, sched_cfg(/*conf=*/2, /*min=*/2, /*max=*/64,
                                           /*draft=*/false));
  // 回调注册是 Pipeline 的职责(调度器自己不去抢 MtBackend 的回调槽):
  // 这里模拟 Pipeline 的接线, 之后 reply() 才会真正回到 on_mt_result。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  sched.on_asr_partial(partial(1, "ab"));  // 一致性不足, 没有请求
  SL_EXPECT_EQ(mt.requests.size(), 0u);

  // p2 触发正式提交, 结果在 submit 内部同步返回 → 本次调用内就应完成上屏。
  sched.on_asr_partial(partial(1, "abc"));
  SL_EXPECT_EQ(mt.requests.size(), 1u);
  SL_EXPECT_EQ(out.size(), 1u);
  SL_EXPECT_EQ(out[0].src, std::string("ab"));
  SL_EXPECT_EQ(out[0].dst, std::string("T:ab"));

  // final 再提交剩下的 1 个字, 同样同步返回; 前缀重译后只追加差量"c"。
  sched.on_asr_partial(partial(1, "abc", /*final=*/true));
  SL_EXPECT_EQ(mt.requests.size(), 2u);
  SL_EXPECT_EQ(mt.requests[1].text, std::string("abc"));  // 累计前缀
  SL_EXPECT_EQ(out.size(), 2u);
  SL_EXPECT_EQ(out[1].src, std::string("c"));
  SL_EXPECT_EQ(out[1].dst, std::string("c"));
  SL_EXPECT_EQ(sched.stats().mt_results, 2u);
}

// ---------------------------------------------------------------------------
// 7. reset: 会话切换后旧句的在途结果不能被当成新句的上屏内容
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_reset_clears_state) {
  FakeMt mt;
  TranslationScheduler sched(mt, sched_cfg(/*conf=*/2, /*min=*/2, /*max=*/64,
                                           /*draft=*/false));
  // 回调注册是 Pipeline 的职责(调度器自己不去抢 MtBackend 的回调槽):
  // 这里模拟 Pipeline 的接线, 之后 reply() 才会真正回到 on_mt_result。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  sched.on_asr_partial(partial(1, "你好"));
  sched.on_asr_partial(partial(1, "你好世界"));
  SL_EXPECT_EQ(mt.requests.size(), 1u);
  SL_EXPECT_EQ(sched.stats().commits, 1u);

  sched.reset();
  SL_EXPECT_EQ(sched.stats().commits, 0u);
  SL_EXPECT_EQ(sched.stats().partials, 0u);

  // 旧请求的结果在 reset 之后到达: 状态已清空, 这条结果按"过晚"丢弃 ——
  // 绝不能在切换会话之后把旧语境的内容补到新画面上。
  mt.reply(0);
  SL_EXPECT(out.empty());
  SL_EXPECT_EQ(sched.stats().mt_results, 0u);
}

// ---------------------------------------------------------------------------
// 8. 长跑边界: 超过状态上限才回来的正式译文被丢弃(否则 HUD 主行会回退)
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_drops_orphan_result_after_eviction) {
  FakeMt mt;
  TranslationScheduler sched(mt, sched_cfg(/*conf=*/2, /*min=*/1, /*max=*/64,
                                           /*draft=*/false));
  // 回调要在 sched 构造之后才能接线。
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  // 连续 18 句, 每句都在第二个 partial 提交一个字 —— 共 18 条正式翻译请求。
  // 状态上限 16, 所以第 1、2 句的状态在第 17、18 句到来时被淘汰。
  for (uint64_t id = 1; id <= 18; ++id) {
    sched.on_asr_partial(partial(id, "a"));
    sched.on_asr_partial(partial(id, "ab"));
  }
  SL_EXPECT_EQ(mt.requests.size(), 18u);

  // 第 1 句的结果现在才回来: 它的状态已经没了。补发一条 kCommitted 会让
  // HudState 把主行整体替换回第 1 句(用户已经读到第 18 句了) → 必须丢弃。
  mt.reply(0);
  SL_EXPECT(out.empty());
  SL_EXPECT_EQ(sched.stats().mt_results, 0u);

  // 最新一句的正常上屏。
  mt.reply(17);
  SL_EXPECT_EQ(out.size(), 1u);
  SL_EXPECT_EQ(out[0].utt_id, 18u);
  SL_EXPECT_EQ(out[0].src, std::string("a"));
  SL_EXPECT_EQ(out[0].dst, std::string("T:a"));
}

// ---------------------------------------------------------------------------
// 9. 整句前缀重译: 请求带完整上下文, 结果只追加"新增可读内容"
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_prefix_retranslation_appends_diff) {
  FakeMt mt;
  // 模拟一个"看得到整句上下文"的 MT: 译文按整句前缀给出, 片段级翻译做不到。
  // 这里译文随原文前缀单调变长, 是最理想(也最常见)的流式翻译路径。
  mt.translate = [](const std::string& src) {
    if (src == "你好") return std::string("Hello");
    if (src == "你好世界") return std::string("Hello world");
    if (src == "你好世界啊") return std::string("Hello world!");
    return std::string("<?>");
  };
  TranslationScheduler sched(mt, sched_cfg(/*conf=*/2, /*min=*/1, /*max=*/64,
                                           /*draft=*/false));
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  // conf=2/min=1 下每个 partial 推进一个提交 → 三次正式请求(含 final)。
  sched.on_asr_partial(partial(1, "你好"));
  sched.on_asr_partial(partial(1, "你好世界"));   // 提交"你好"
  sched.on_asr_partial(partial(1, "你好世界啊"));  // 提交"世界"
  sched.on_asr_partial(partial(1, "你好世界啊", /*final=*/true));  // 提交"啊"

  SL_EXPECT_EQ(mt.requests.size(), 3u);
  // 关键: 每次请求发的是"该句累计原文前缀", 不是单个片段。
  SL_EXPECT_EQ(mt.requests[0].text, std::string("你好"));
  SL_EXPECT_EQ(mt.requests[1].text, std::string("你好世界"));
  SL_EXPECT_EQ(mt.requests[2].text, std::string("你好世界啊"));

  mt.reply(0);
  mt.reply(1);
  mt.reply(2);
  SL_EXPECT_EQ(out.size(), 3u);

  // 差量追加: 第一次整段, 之后只追加公共前缀之后的新增部分。
  SL_EXPECT_EQ(out[0].dst, std::string("Hello"));
  SL_EXPECT_EQ(out[1].dst, std::string(" world"));
  SL_EXPECT_EQ(out[2].dst, std::string("!"));
  // src 仍遵守契约: 本次提交的原文片段。
  SL_EXPECT_EQ(out[0].src, std::string("你好"));
  SL_EXPECT_EQ(out[1].src, std::string("世界"));
  SL_EXPECT_EQ(out[2].src, std::string("啊"));

  // 主行 append 出来的总文本 == 最近的完整译文(以最后一次重译为准)。
  std::string joined;
  for (const auto& u : out) joined += u.dst;
  SL_EXPECT_EQ(joined, std::string("Hello world!"));
}

// ---------------------------------------------------------------------------
// 10. 重译改写了已显示内容: 句内不动, 句末整行校正一次(主行 = 最终完整译文)
// ---------------------------------------------------------------------------
SL_TEST(Scheduler_prefix_retranslation_rewrite_does_not_repeat) {
  FakeMt mt;
  // 第 3 次重译把已显示的 "Hello world" 改写成 "Hello there";
  // 第 4 次在**新版本**上继续延长成 "Hello there!"。
  mt.translate = [](const std::string& src) {
    if (src == "你好") return std::string("Hello");
    if (src == "你好世界") return std::string("Hello world");
    if (src == "你好世界啊") return std::string("Hello there");
    if (src == "你好世界啊呀") return std::string("Hello there!");
    return std::string("<?>");
  };
  TranslationScheduler sched(mt, sched_cfg(/*conf=*/2, /*min=*/1, /*max=*/64,
                                           /*draft=*/false));
  mt.set_callback([&sched](const MtResult& r) { sched.on_mt_result(r); });
  std::vector<HudUpdate> out;
  sched.set_sink([&out](const HudUpdate& u) { out.push_back(u); });

  sched.on_asr_partial(partial(1, "你好"));
  sched.on_asr_partial(partial(1, "你好世界"));       // 提交"你好"
  sched.on_asr_partial(partial(1, "你好世界啊"));      // 提交"世界"
  sched.on_asr_partial(partial(1, "你好世界啊呀"));    // 提交"啊"
  sched.on_asr_partial(partial(1, "你好世界啊呀", /*final=*/true));  // 提交"呀"
  SL_EXPECT_EQ(mt.requests.size(), 4u);

  mt.reply(0);
  mt.reply(1);
  SL_EXPECT_EQ(out.size(), 2u);
  SL_EXPECT_EQ(out[0].dst, std::string("Hello"));
  SL_EXPECT_EQ(out[1].dst, std::string(" world"));

  // 第 3 次: 新译文 "Hello there" 与已显示 "Hello world" 的公共前缀只有
  // "Hello ", 说明尾部被重写了。此时该句已经 final, 所以做**整行校正**:
  // 主行直接换成完整译文, 而不是把 " there" 硬接在 "Hello world" 后面(会读成
  // "Hello world there")。这是"句内只增不改 + 句末校正一次"的最后一步。
  mt.reply(2);
  SL_EXPECT_EQ(out.size(), 3u);
  SL_EXPECT(out[2].kind == HudUpdate::Kind::kReplace);
  SL_EXPECT_EQ(out[2].dst, std::string("Hello there"));  // 完整译文
  SL_EXPECT_EQ(out[2].src, std::string("你好世界啊呀"));  // 整句原文(到 final 为止已提交的全部内容)
  SL_EXPECT_EQ(sched.stats().mt_results, 3u);

  // 第 4 次: 在**校正后的版本**上继续, 只追加 "!" —— 证明参照文本已经更新。
  mt.reply(3);
  SL_EXPECT_EQ(out.size(), 4u);
  SL_EXPECT(out[3].kind == HudUpdate::Kind::kCommitted);
  SL_EXPECT_EQ(out[3].src, std::string("呀"));
  SL_EXPECT_EQ(out[3].dst, std::string("!"));

  // 用真实的 HUD 状态机折叠一遍: 最终主行必须是这句的完整译文。
  sublens::HudState hud;
  for (const auto& u : out) hud.apply(u);
  SL_EXPECT_EQ(hud.snapshot().cur_dst, std::string("Hello there!"));
  // 整行校正每句最多一次
  SL_EXPECT_EQ(hud.commit_rewrites(), uint64_t{1});
}

