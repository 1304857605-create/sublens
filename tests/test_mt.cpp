/// @file test_mt.cpp
/// @brief MT 后端层测试: 模板命中率、短语兜底、异步回调、时延注入。
///
/// 验收核心是"demo 12 句模板逐字命中": 这是流水线端到端测试能写确定期望值的前提。
/// 另外要求未知句子不崩且非空 —— 离线翻译器的底线是"翻不好也不能炸/不能空白",
/// 否则断网演示时 HUD 会一片空白。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sublens/mt.hpp"
#include "sublens/script.hpp"
#include "tiny_test.hpp"

namespace {

using namespace sublens;

PhraseTableMt::Config config_zh2en() {
  PhraseTableMt::Config c;
  c.source_lang = "zh";
  c.target_lang = "en";
  return c;
}

PhraseTableMt::Config config_en2zh() {
  PhraseTableMt::Config c;
  c.source_lang = "en";
  c.target_lang = "zh";
  return c;
}

void sleep_briefly(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------------------
// 前缀质量检查用的小工具
//
// 调度器做"整句前缀重译 + 差量追加"时, 拿到的是 ASR 逐词累加出来的前缀
// (同一个拼接规则与 ScriptedAsrBackend 一致: 只有两侧都是中文时才不加空格)。
// 这里复刻该规则, 保证测试测的就是流水线真实会送进来的文本。
// ---------------------------------------------------------------------------

std::string join_words(const std::vector<ScriptWord>& words, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count && i < words.size(); ++i) {
    const std::string& w = words[i].text;
    if (w.empty()) continue;
    if (!out.empty()) {
      const unsigned char prev = static_cast<unsigned char>(out.back());
      const unsigned char cur = static_cast<unsigned char>(w.front());
      if (!(prev >= 0x80 && cur >= 0x80)) out.push_back(' ');
    }
    out += w;
  }
  return out;
}

bool has_ascii_letter(const std::string& s) {
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  }
  return false;
}

bool has_cjk_char(const std::string& s) {
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    unsigned cp = 0;
    int n = 1;
    if ((c >> 5) == 0x6) {
      cp = c & 0x1Fu;
      n = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0x0Fu;
      n = 3;
    } else {
      cp = c & 0x07u;
      n = 4;
    }
    for (int k = 1; k < n && i + static_cast<std::size_t>(k) < s.size(); ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3Fu);
    }
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    i += static_cast<std::size_t>(n);
  }
  return false;
}

std::size_t utf8_char_count(const std::string& s) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      i += 1;
    } else if ((c >> 5) == 0x6) {
      i += 2;
    } else if ((c >> 4) == 0xE) {
      i += 3;
    } else {
      i += 4;
    }
    ++n;
  }
  return n;
}

/// MtResult 收集器(回调跑在 MT 工作线程上)
struct MtCollector {
  mutable std::mutex mu;
  std::condition_variable cv;
  std::vector<MtResult> results;

  MtBackend::Callback callback() {
    return [this](const MtResult& r) {
      std::lock_guard<std::mutex> lk(mu);
      results.push_back(r);
      cv.notify_all();
    };
  }

  std::vector<MtResult> snapshot() const {
    std::lock_guard<std::mutex> lk(mu);
    return results;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mu);
    return results.size();
  }

  bool wait_for_at_least(std::size_t n, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                       [&] { return results.size() >= n; });
  }
};

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

/// 中→英: demo 两个场景共 12 句, 双向键都命中(中→英用 zh2en 的 source
/// 和 en2zh 的中文 target 作为 key)。
SL_TEST(Mt_phrase_table_hits_all_12_templates_zh2en) {
  PhraseTableMt mt(config_zh2en());
  const ConversationScript& zh2en = demo_scenario_zh2en();
  const ConversationScript& en2zh = demo_scenario_en2zh();

  // 场景 2: 中文 source_text → 英文 target_text
  for (const auto& u : zh2en.utterances) {
    SL_EXPECT_EQ(mt.translate_sync(u.source_text), u.target_text);
  }
  // 场景 1 的中文 target_text 反查英文 source_text(同一份模板表双向可用)
  for (const auto& u : en2zh.utterances) {
    SL_EXPECT_EQ(mt.translate_sync(u.target_text), u.source_text);
  }
}

/// 英→中: 同样 12 句模板全命中。
SL_TEST(Mt_phrase_table_hits_all_12_templates_en2zh) {
  PhraseTableMt mt(config_en2zh());
  const ConversationScript& zh2en = demo_scenario_zh2en();
  const ConversationScript& en2zh = demo_scenario_en2zh();

  for (const auto& u : en2zh.utterances) {
    SL_EXPECT_EQ(mt.translate_sync(u.source_text), u.target_text);
  }
  for (const auto& u : zh2en.utterances) {
    SL_EXPECT_EQ(mt.translate_sync(u.target_text), u.source_text);
  }
}

/// 模板未命中时用短语表拼: 必须有意义、非空; 未知句子也不能崩或返回空。
SL_TEST(Mt_phrase_table_phrase_fallback_is_meaningful) {
  PhraseTableMt zh2en(config_zh2en());
  PhraseTableMt en2zh(config_en2zh());

  // 中文 → 英文: 关键词必须真的被翻译出来(不是原文照抄)
  const std::string out1 = zh2en.translate_sync("请问洗手间在哪里?");
  SL_EXPECT(!out1.empty());
  SL_EXPECT(out1.find("bathroom") != std::string::npos);
  SL_EXPECT(out1.find("where") != std::string::npos);

  const std::string out2 = zh2en.translate_sync("我想买两瓶啤酒");
  SL_EXPECT(out2.find("buy") != std::string::npos);
  SL_EXPECT(out2.find("beer") != std::string::npos);

  // 英文 → 中文
  const std::string out3 = en2zh.translate_sync("How much is the ticket?");
  SL_EXPECT(out3.find("多少钱") != std::string::npos);

  const std::string out4 = en2zh.translate_sync("Where is the bathroom?");
  SL_EXPECT(out4.find("洗手间") != std::string::npos);

  // 未知句子: 不崩、非空(允许保留部分原文, 但要有输出)
  const std::string junk_en = "The quick brown fox jumps over the lazy dog.";
  const std::string out5 = en2zh.translate_sync(junk_en);
  SL_EXPECT(!out5.empty());

  const std::string junk_zh = "蓝色的鲸鱼在唱歌。";
  const std::string out6 = zh2en.translate_sync(junk_zh);
  SL_EXPECT(!out6.empty());

  // 空输入不应该崩
  SL_EXPECT(zh2en.translate_sync("").empty());
  SL_EXPECT(en2zh.translate_sync("").empty());
}

/// 异步提交: 回调带正确的 utt_id/text/draft, 与 translate_sync 结果一致;
/// stop() 之后不再有回调。
SL_TEST(Mt_phrase_table_async_callback_passthrough) {
  PhraseTableMt mt(config_zh2en());
  MtCollector col;
  mt.set_callback(col.callback());
  SL_EXPECT(mt.start());

  MtRequest draft;
  draft.utt_id = 7;
  draft.text = "谢谢";
  draft.draft = true;
  MtRequest committed;
  committed.utt_id = 8;
  committed.text = "请问洗手间在哪里?";
  committed.draft = false;

  mt.submit(draft);
  mt.submit(committed);
  SL_EXPECT(col.wait_for_at_least(2, 2000));
  mt.flush();

  const std::vector<MtResult> got = col.snapshot();
  SL_EXPECT_EQ(got.size(), static_cast<std::size_t>(2));

  // 顺序与提交一致(单工作线程 FIFO, 推测翻译依赖这个顺序)
  SL_EXPECT_EQ(got[0].utt_id, static_cast<uint64_t>(7));
  SL_EXPECT_EQ(got[0].text, mt.translate_sync("谢谢"));
  SL_EXPECT_EQ(got[0].draft, true);
  SL_EXPECT(got[0].emit_ms > 0);

  SL_EXPECT_EQ(got[1].utt_id, static_cast<uint64_t>(8));
  SL_EXPECT_EQ(got[1].text, mt.translate_sync("请问洗手间在哪里?"));
  SL_EXPECT_EQ(got[1].draft, false);

  // stop 之后提交必须被静默丢弃, 不能再有任何回调
  mt.stop();
  const std::size_t after_stop = col.size();
  MtRequest late;
  late.utt_id = 9;
  late.text = "谢谢";
  mt.submit(late);
  sleep_briefly(60);
  SL_EXPECT_EQ(col.size(), after_stop);
}

/// 时延注入: 实测往返 >= base_ms, 结果原样透传(utt_id/text/draft)。
SL_TEST(Mt_latency_injecting_adds_delay_and_forwards) {
  PhraseTableMt::Config inner_cfg = config_zh2en();
  const std::string expected = PhraseTableMt(inner_cfg).translate_sync("谢谢");
  const std::string source = "谢谢";

  LatencyInjectConfig inj_cfg;
  inj_cfg.base_ms = 80;
  inj_cfg.jitter_ms = 20;
  inj_cfg.seed = 7;

  LatencyInjectingMt mt(std::make_unique<PhraseTableMt>(inner_cfg), inj_cfg);
  MtCollector col;
  mt.set_callback(col.callback());
  SL_EXPECT(mt.start());

  MtRequest req;
  req.utt_id = 42;
  req.text = source;
  req.draft = true;

  const int64_t submit_ms = now_ms();
  mt.submit(req);
  SL_EXPECT(col.wait_for_at_least(1, 2000));
  const int64_t roundtrip_ms = now_ms() - submit_ms;
  mt.flush();

  // 注入的是串行等待, 实测往返至少 base_ms(允许 1ms 的毫秒截断误差)
  SL_EXPECT(roundtrip_ms >= inj_cfg.base_ms - 1);

  const std::vector<MtResult> got = col.snapshot();
  SL_EXPECT_EQ(got.size(), static_cast<std::size_t>(1));
  SL_EXPECT_EQ(got[0].utt_id, static_cast<uint64_t>(42));
  SL_EXPECT_EQ(got[0].text, expected);  // 结果透传, 没有被包装层改写
  SL_EXPECT_EQ(got[0].draft, true);

  mt.stop();
}

/// 时延注入包装的 stop 也必须彻底: 停止后没有回调, 且不会卡住。
SL_TEST(Mt_latency_injecting_stop_is_quiet) {
  LatencyInjectConfig inj_cfg;
  inj_cfg.base_ms = 200;
  inj_cfg.jitter_ms = 0;
  inj_cfg.seed = 3;

  LatencyInjectingMt mt(std::make_unique<PhraseTableMt>(config_zh2en()), inj_cfg);
  MtCollector col;
  mt.set_callback(col.callback());
  SL_EXPECT(mt.start());

  MtRequest req;
  req.utt_id = 1;
  req.text = "谢谢";
  mt.submit(req);  // 正在 200ms 的注入等待里
  sleep_briefly(30);

  const int64_t t0 = now_ms();
  mt.stop();  // 必须能打断等待, 不能等满 200ms
  const int64_t stop_cost_ms = now_ms() - t0;
  SL_EXPECT(stop_cost_ms < 150);

  const std::size_t after_stop = col.size();
  sleep_briefly(120);
  SL_EXPECT_EQ(col.size(), after_stop);
}

/// 前缀质量护栏(Lead 要求的硬指标):
/// 对两个 demo 场景每一句话的**每一个逐词前缀**:
///   1) 英→中: 译文不含任何 ASCII 字母(英文必须被词表完整覆盖, 不能漏英文);
///   2) 中→英: 译文不含任何汉字;
///   3) 字符数随前缀增长单调不减(调度器做"差量追加"的前提);
///   4) 整句(最后一个前缀)仍命中整句模板, 与 target_text 逐字一致。
SL_TEST(Mt_phrase_table_demo_prefixes_are_clean_and_monotonic) {
  struct ScenarioCase {
    const ConversationScript* script;
    bool en_to_zh;
  };
  const ScenarioCase cases[] = {
      {&demo_scenario_en2zh(), true},
      {&demo_scenario_zh2en(), false},
  };

  for (const auto& c : cases) {
    PhraseTableMt::Config cfg;
    cfg.source_lang = c.en_to_zh ? "en" : "zh";
    cfg.target_lang = c.en_to_zh ? "zh" : "en";
    PhraseTableMt mt(cfg);

    for (const auto& u : c.script->utterances) {
      std::size_t prev_len = 0;
      for (std::size_t k = 1; k <= u.words.size(); ++k) {
        const std::string prefix = join_words(u.words, k);
        const std::string out = mt.translate_sync(prefix);

        // 不能有"没翻译干净的源语言字符"
        if (c.en_to_zh) {
          SL_EXPECT(!has_ascii_letter(out));
        } else {
          SL_EXPECT(!has_cjk_char(out));
        }

        // 越说越长: 只要求非严格单调(冠词 the 之类翻成空也算合法一步)
        const std::size_t len = utf8_char_count(out);
        SL_EXPECT(len >= prev_len);
        prev_len = len;
      }

      // 整句必须回到模板, 与参考译文逐字一致
      const std::string full = join_words(u.words, u.words.size());
      SL_EXPECT_EQ(mt.translate_sync(full), u.target_text);
    }
  }
}

}  // namespace
