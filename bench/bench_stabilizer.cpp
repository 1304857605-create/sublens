/// @file bench_stabilizer.cpp  稳定前缀对齐(LocalAgreement-n)基准
///
/// 这个模块是"字幕不闪变"的关键, 但它也直接决定延迟:
/// 要求越多 partial 一致(confirmations 越大), 越稳但也越慢。
/// 这里把 trade-off 量化出来, 是延迟预算里唯一可以"调"的旋钮。

#include <cctype>
#include <string>
#include <vector>

#include "bench.hpp"
#include "sublens/asr.hpp"
#include "sublens/common.hpp"
#include "sublens/sched.hpp"
#include "sublens/script.hpp"

namespace sublens_bench {
namespace {

/// 把脚本的每个词按顺序拼成 partial 序列(真实流式 ASR 的行为)。
///
/// 关键: 拼接规则必须和 script.source_text 的生成规则一致(英文词之间加空格),
/// 否则 "已提交内容" 和最终定稿文本会差几个空格, 完整性检查会误报。
std::vector<sublens::AsrPartial> make_partials(const sublens::ScriptUtterance& u) {
  bool latin = false;
  for (const auto& w : u.words) {
    for (char c : w.text) {
      if (std::isalpha(static_cast<unsigned char>(c)) != 0) latin = true;
    }
  }

  std::vector<sublens::AsrPartial> out;
  std::string acc;
  int64_t offset = 0;
  for (size_t i = 0; i < u.words.size(); ++i) {
    offset += u.words[i].dur_ms;
    acc += u.words[i].text;
    if (latin && i + 1 < u.words.size()) acc += ' ';
    sublens::AsrPartial p;
    p.utt_id = u.id;
    p.text = acc;
    p.is_final = false;
    p.speech_start_ms = u.start_ms;
    p.speech_end_ms = u.start_ms + u.speech_ms();
    p.emit_ms = p.speech_end_ms + u.speech_ms() - offset;  // 时间轴仅作占位
    out.push_back(std::move(p));
  }
  sublens::AsrPartial fin;
  fin.utt_id = u.id;
  fin.text = u.source_text;  // 与逐词拼接结果逐字一致(见 scenarios.cpp 的 join_words)
  fin.is_final = true;
  fin.speech_start_ms = u.start_ms;
  fin.speech_end_ms = u.start_ms + u.speech_ms();
  fin.emit_ms = fin.speech_end_ms + 120;
  out.push_back(std::move(fin));
  return out;
}

}  // namespace

BenchReport bench_stabilizer() {
  using namespace sublens;
  BenchReport r;
  r.id = "stabilizer";
  r.title = "稳定前缀对齐(LocalAgreement-n)";

  const ConversationScript& script = demo_scenario_en2zh();

  std::string md;
  md += "### 1. confirmations 的取舍(n 越大越稳, 也越慢)\n\n";
  md += "| confirmations | 提交片段数 | 平均每句片段 | 平均尾部字数 | 提交内容完整性 |\n";
  md += "|---|---|---|---|---|\n";

  for (int confirmations : {1, 2, 3}) {
    PartialStabilizer stab({confirmations, 2, 24, true});
    uint64_t chunks = 0;
    int64_t tail_chars = 0;
    int64_t tail_samples = 0;
    bool integrity_ok = true;

    for (const auto& u : script.utterances) {
      std::string rebuilt;
      for (const auto& p : make_partials(u)) {
        const auto up = stab.on_partial(p);
        if (!up.committed.empty()) {
          ++chunks;
          rebuilt += up.committed;
        }
        tail_chars += static_cast<int64_t>(up.tail.size());
        ++tail_samples;
      }
      if (rebuilt != u.source_text) integrity_ok = false;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "| %d | %llu | %.2f | %.1f | %s |\n", confirmations,
                  static_cast<unsigned long long>(chunks),
                  static_cast<double>(chunks) / static_cast<double>(script.utterances.size()),
                  static_cast<double>(tail_chars) / static_cast<double>(tail_samples),
                  integrity_ok ? "✓ 与原文逐字一致" : "✗ 有丢失");
    md += buf;
  }

  md += "\n### 2. 算法本身的开销\n\n";
  {
    PartialStabilizer stab;
    // 每句 8 个逐步变长的 partial, 8 个之后换句(utt_id 递增会重置状态),
    // 保证文本始终是"单调增长"的合法 partial 序列。
    const std::string base = "前方路口右转大约三百米就到了";
    const int kIters = 2'000'000;
    const int kPerUtt = 8;
    Stopwatch sw;
    for (int i = 0; i < kIters; ++i) {
      AsrPartial p;
      p.utt_id = static_cast<uint64_t>(i / kPerUtt) + 1;
      const size_t chars = static_cast<size_t>(i % kPerUtt) + 1;  // 1..8 个汉字
      p.text = base.substr(0, chars * 3);  // 每个汉字 3 字节 UTF-8
      stab.on_partial(p);
    }
    const double ns_per = static_cast<double>(sw.ns()) / kIters;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "| 指标 | 数值 |\n|---|---|\n"
                  "| 单次 on_partial | %.1f ns |\n"
                  "| 每秒可处理 partial 数 | %.2f 万 |\n\n",
                  ns_per, 1e9 / ns_per / 10000.0);
    md += buf;
  }

  md += "> 结论: 算法本身是亚微秒级, 相对 ASR/MT 的网络与模型延迟可以忽略; "
        "confirmations 选 2 是「稳定」与「不被拖慢」的平衡点, 选 1 会让字幕在识别纠错时回退。\n";
  r.markdown = md;
  return r;
}

}  // namespace sublens_bench

