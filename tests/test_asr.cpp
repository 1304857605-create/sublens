/// @file test_asr.cpp
/// @brief ASR 后端层测试: 脚本模拟器的时序/顺序/静默契约 + 云端骨架的失败语义。
///
/// 这些用例同时是契约文档 —— 上层调度器(稳定前缀 + 推测翻译)直接依赖这些行为:
///   - partial 是"到目前为止的完整假设", 必须前缀递增;
///   - 同一句的 partial 严格有序, 不同句不能串号;
///   - stop() 返回之后绝不再有回调(否则上层销毁统计/调度对象会踩悬空指针);
///   - 首字延迟必须落在配置值附近, 它是端到端延迟预算里最大的一块。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sublens/asr.hpp"
#include "tiny_test.hpp"

namespace {

using namespace sublens;

// ---------------------------------------------------------------------------
// 测试脚本构造: 时间轴用短词长, 保证整个测试套件 5 秒内跑完
// ---------------------------------------------------------------------------

void add_utterance(ConversationScript& script, uint64_t id, std::string text,
                   std::vector<std::string> words, int word_ms = 100) {
  ScriptUtterance u;
  u.id = id;
  u.source_text = text;
  u.target_text = text;
  for (auto& w : words) u.words.push_back(ScriptWord{std::move(w), word_ms});
  script.utterances.push_back(std::move(u));
}

ConversationScript make_script(const char* name = "asr-test") {
  ConversationScript s;
  s.name = name;
  s.source_lang = "en";
  s.target_lang = "zh";
  return s;
}

Utterance make_utterance(uint64_t id, int64_t speech_start_ms, int64_t speech_end_ms) {
  Utterance u;
  u.id = id;
  u.speech_start_ms = speech_start_ms;
  u.speech_end_ms = speech_end_ms;
  u.speech_duration_ms = speech_end_ms - speech_start_ms;
  return u;
}

void sleep_briefly(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/// partial 收集器: 回调跑在 ASR 自己的工作线程上, 用 mutex+cv 等待,
/// 不用 sleep 轮询(否则测试既慢又不稳定)。
struct Collector {
  mutable std::mutex mu;
  std::condition_variable cv;
  std::vector<AsrPartial> events;

  AsrBackend::Callback callback() {
    return [this](const AsrPartial& p) {
      std::lock_guard<std::mutex> lk(mu);
      events.push_back(p);
      cv.notify_all();
    };
  }

  std::vector<AsrPartial> snapshot() const {
    std::lock_guard<std::mutex> lk(mu);
    return events;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mu);
    return events.size();
  }

  bool wait_for_at_least(std::size_t n, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                       [&] { return events.size() >= n; });
  }
};

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

/// 单句: 首字延迟、>=3 个 partial、前缀递增、final 标记、时间戳透传。
SL_TEST(Asr_scripted_single_utterance_progresses) {
  ConversationScript script = make_script();
  add_utterance(script, 1, "Hello world again", {"Hello", "world", "again"});

  ScriptedAsrConfig cfg;
  cfg.script = &script;
  cfg.first_partial_delay_ms = 120;
  cfg.word_step_ms = 25;
  cfg.final_extra_ms = 30;
  cfg.jitter_ms = 15;

  ScriptedAsrBackend asr(cfg);
  Collector col;
  asr.set_callback(col.callback());
  SL_EXPECT(asr.start());

  const int64_t submit_ms = now_ms();
  asr.submit(make_utterance(1, 1000, 2500));
  asr.flush();  // flush 返回时末回调已经跑完

  const std::vector<AsrPartial> events = col.snapshot();
  SL_EXPECT(events.size() >= 3);

  // 首字延迟: 落在 [delay - jitter - 容差, delay + jitter + 容差]
  const int64_t first_latency = events.front().emit_ms - submit_ms;
  SL_EXPECT(first_latency >= cfg.first_partial_delay_ms - cfg.jitter_ms - 50);
  SL_EXPECT(first_latency <= cfg.first_partial_delay_ms + cfg.jitter_ms + 150);

  // 前缀递增 + 语句边界/句号透传 + final 只能是最后一个
  for (std::size_t i = 0; i < events.size(); ++i) {
    SL_EXPECT_EQ(events[i].utt_id, static_cast<uint64_t>(1));
    SL_EXPECT_EQ(events[i].speech_start_ms, static_cast<int64_t>(1000));
    SL_EXPECT_EQ(events[i].speech_end_ms, static_cast<int64_t>(2500));
    SL_EXPECT(events[i].emit_ms >= submit_ms);
    if (i > 0) {
      // 后一个 partial 必须以前一个为前缀, 且不会变短
      SL_EXPECT_EQ(events[i].text.rfind(events[i - 1].text, 0), static_cast<std::size_t>(0));
      SL_EXPECT(events[i].text.size() >= events[i - 1].text.size());
    }
    if (i + 1 < events.size()) SL_EXPECT(!events[i].is_final);
  }
  SL_EXPECT(events.back().is_final);
  SL_EXPECT_EQ(events.back().text, std::string("Hello world again"));

  asr.stop();
}

/// 多句连续提交: 顺序消费, utt_id 不串号, 每句各自的 final 文本正确。
SL_TEST(Asr_scripted_multi_utterance_stays_ordered) {
  ConversationScript script = make_script();
  add_utterance(script, 1, "one two", {"one", "two"});
  add_utterance(script, 2, "three four", {"three", "four"});
  add_utterance(script, 3, "five six", {"five", "six"});

  ScriptedAsrConfig cfg;
  cfg.script = &script;
  cfg.first_partial_delay_ms = 15;
  cfg.word_step_ms = 10;
  cfg.final_extra_ms = 5;
  cfg.jitter_ms = 0;

  ScriptedAsrBackend asr(cfg);
  Collector col;
  asr.set_callback(col.callback());
  SL_EXPECT(asr.start());

  asr.submit(make_utterance(1, 100, 300));
  asr.submit(make_utterance(2, 400, 600));
  asr.submit(make_utterance(3, 700, 900));
  asr.flush();

  const std::vector<AsrPartial> events = col.snapshot();
  SL_EXPECT(events.size() >= 9);  // 每句至少 3 个事件

  // 每个事件必须属于 1/2/3 之一; 同一句内文本前缀递增
  std::map<uint64_t, std::string> last_text;
  std::map<uint64_t, std::string> final_text;
  std::map<uint64_t, std::size_t> final_count;
  std::vector<uint64_t> final_order;
  for (const auto& e : events) {
    SL_EXPECT(e.utt_id >= 1 && e.utt_id <= 3);
    auto it = last_text.find(e.utt_id);
    if (it != last_text.end()) {
      SL_EXPECT_EQ(e.text.rfind(it->second, 0), static_cast<std::size_t>(0));
    }
    last_text[e.utt_id] = e.text;
    if (e.is_final) {
      final_text[e.utt_id] = e.text;
      ++final_count[e.utt_id];
      final_order.push_back(e.utt_id);
    }
  }

  // 每句恰好一个 final, 且 final 顺序 = 提交顺序(FIFO 单工作线程)
  SL_EXPECT_EQ(final_order.size(), static_cast<std::size_t>(3));
  SL_EXPECT_EQ(final_order[0], static_cast<uint64_t>(1));
  SL_EXPECT_EQ(final_order[1], static_cast<uint64_t>(2));
  SL_EXPECT_EQ(final_order[2], static_cast<uint64_t>(3));
  for (uint64_t id = 1; id <= 3; ++id) {
    SL_EXPECT_EQ(final_count[id], static_cast<std::size_t>(1));
    const auto& expect = script.utterances[static_cast<std::size_t>(id - 1)].source_text;
    SL_EXPECT_EQ(final_text[id], expect);
  }

  asr.stop();
}

/// stop() 必须能打断"正在等待首字延迟/词间隔"的工作线程, 且返回后彻底安静。
SL_TEST(Asr_scripted_stop_silences_callbacks) {
  ConversationScript script = make_script();
  add_utterance(script, 1, "a b c d e f g h", {"a", "b", "c", "d", "e", "f", "g", "h"});

  ScriptedAsrConfig cfg;
  cfg.script = &script;
  cfg.first_partial_delay_ms = 10;
  cfg.word_step_ms = 40;
  cfg.final_extra_ms = 10;
  cfg.jitter_ms = 0;

  ScriptedAsrBackend asr(cfg);
  Collector col;
  asr.set_callback(col.callback());
  SL_EXPECT(asr.start());

  asr.submit(make_utterance(1, 0, 800));
  SL_EXPECT(col.wait_for_at_least(2, 2000));

  asr.stop();
  const std::size_t after_stop = col.size();  // stop 返回时回调已经全部收尾
  sleep_briefly(120);
  SL_EXPECT_EQ(col.size(), after_stop);  // 之后不可能再增加

  // 停止后继续提交应被静默丢弃: 不崩、也不产生新回调
  asr.submit(make_utterance(2, 900, 1000));
  sleep_briefly(80);
  SL_EXPECT_EQ(col.size(), after_stop);
}

/// 错误注入: 错字只出现在首词之后, 稳定前缀不被带歪; 前缀递增关系不破坏。
SL_TEST(Asr_scripted_error_injection_keeps_stable_prefix) {
  ConversationScript script = make_script();
  add_utterance(script, 1, "Hello dear friend good day",
                {"Hello", "dear", "friend", "good", "day"});

  ScriptedAsrConfig cfg;
  cfg.script = &script;
  cfg.first_partial_delay_ms = 10;  // 只够露出第一个词
  cfg.word_step_ms = 5;
  cfg.final_extra_ms = 5;
  cfg.jitter_ms = 0;
  cfg.inject_errors = true;
  cfg.seed = 2026;

  ScriptedAsrBackend asr(cfg);
  Collector col;
  asr.set_callback(col.callback());
  SL_EXPECT(asr.start());

  asr.submit(make_utterance(1, 0, 500));
  asr.flush();

  const std::vector<AsrPartial> events = col.snapshot();
  SL_EXPECT(events.size() >= 5);
  // 首字延迟窗口内只有第一个词, 且注入点永远在首词之后 → 稳定前缀不受影响
  SL_EXPECT_EQ(events.front().text, std::string("Hello"));
  // 定稿文本里确实带了错误内容(否则这个测试没有意义)
  SL_EXPECT(events.back().text != std::string("Hello dear friend good day"));
  SL_EXPECT_EQ(events.back().text.rfind("Hello", 0), static_cast<std::size_t>(0));
  // 错误只影响内容, 不影响"前缀递增"这一结构性质
  for (std::size_t i = 1; i < events.size(); ++i) {
    SL_EXPECT_EQ(events[i].text.rfind(events[i - 1].text, 0), static_cast<std::size_t>(0));
  }
  SL_EXPECT(events.back().is_final);

  asr.stop();
}

/// 云端骨架: 没配 endpoint 必须明确失败(而不是假成功), 其它方法安全空转。
SL_TEST(Asr_cloud_backend_requires_endpoint) {
  CloudAsrBackend empty(CloudAsrConfig{});
  SL_EXPECT(!empty.start());  // endpoint 为空 → 拒绝启动 + WARN
  SL_EXPECT_EQ(std::string(empty.name()), std::string("cloud-asr"));

  // 骨架方法必须安全: 不崩、不阻塞
  empty.set_callback([](const AsrPartial&) {});
  empty.submit(make_utterance(1, 0, 100));
  empty.flush();
  empty.stop();

  // 配了 endpoint 但真实网络层未实现 → 也明确失败, 不能"看起来能跑"
  CloudAsrConfig configured;
  configured.endpoint = "wss://asr.example.com/v1/stream";
  configured.api_key = "demo-key";
  CloudAsrBackend cloud(std::move(configured));
  SL_EXPECT(!cloud.start());
  cloud.stop();
}

/// 没给脚本时: 不崩、不回调、flush 不卡死(模拟器只负责"跳过并告警")。
SL_TEST(Asr_scripted_without_script_is_safe) {
  ScriptedAsrBackend asr(ScriptedAsrConfig{});  // script == nullptr
  std::atomic<int> callbacks{0};
  asr.set_callback([&callbacks](const AsrPartial&) { callbacks.fetch_add(1); });
  SL_EXPECT(asr.start());

  asr.submit(make_utterance(42, 0, 100));
  asr.flush();  // 没有可播报内容 → 立即返回, 不能死等
  SL_EXPECT_EQ(callbacks.load(), 0);
  asr.stop();
}

}  // namespace
