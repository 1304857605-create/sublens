/// @file scenarios.cpp  内置对话脚本(演示/基准/测试共用)
///
/// 一份脚本同时驱动三样东西: 合成音频、模拟 ASR、参考译文。
/// 时间线是"真实语速"量级(中文约 4~5 字/秒, 英文约 2.5~3.5 词/秒),
/// 因此 VAD 的端点检测、ASR 的 partial 节奏、延迟统计都和真机接近。

#include <algorithm>
#include <cctype>
#include <string>

#include "sublens/script.hpp"

namespace sublens {
namespace {

/// 把 words 拼回"整句原文", 保证 **源文本 == 逐词累加结果**。
///
/// 为什么必须保证: 流式 ASR 的 partial 是按词累加出来的, 稳定前缀对齐器
/// 会把它和最终定稿文本比对。如果定稿文本里多一个空格(或逗号后多一个空格),
/// 对齐器就会认为"ASR 改写了已提交区"——逻辑上没错, 但演示会看到满屏警告,
/// 时间轴和文本也就不再是同一份数据了。
///
/// 规则: 拉丁文按空格连接(英文词之间本来就有空格), 中日韩按直接拼接。
std::string join_words(const std::vector<std::pair<std::string, int>>& words) {
  bool latin = false;
  for (const auto& [text, dur] : words) {
    for (char c : text) {
      if (std::isalpha(static_cast<unsigned char>(c))) latin = true;
    }
  }
  std::string out;
  for (const auto& [text, dur] : words) {
    out += text;
    if (latin) out += ' ';
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

/// 构造一句: 文本以 words 为准, source 参数只用于人工核对。
ScriptUtterance make_utterance(uint64_t id, int start_ms, int gap_after_ms,
                               const std::string& source, const std::string& target,
                               const std::vector<std::pair<std::string, int>>& words) {
  ScriptUtterance u;
  u.id = id;
  u.start_ms = start_ms;
  u.gap_after_ms = gap_after_ms;
  u.target_text = target;
  for (const auto& [text, dur] : words) u.words.push_back(ScriptWord{text, dur});
  u.source_text = join_words(words);
  // source 只作可读性参考; 若与 words 拼接结果不同, 以 words 为准(时间轴一致优先)
  (void)source;
  return u;
}

/// 场景 1: 外宾说英文, 眼镜把英文字幕翻成中文 —— 最常见的出境旅游场景。
ConversationScript build_en2zh() {
  ConversationScript s;
  s.name = "境外旅游: 英→中";
  s.source_lang = "en";
  s.target_lang = "zh";

  s.utterances.push_back(make_utterance(
      1, 0, 700, "Excuse me, could you tell me how to get to the train station?",
      "打扰一下, 请问火车站怎么走?",
      {{"Excuse", 260}, {"me,", 220}, {"could", 240}, {"you", 180}, {"tell", 220},
       {"me", 150}, {"how", 220}, {"to", 120}, {"get", 200}, {"to", 120},
       {"the", 120}, {"train", 300}, {"station?", 360}}));

  s.utterances.push_back(make_utterance(
      2, 4900, 600, "It is about fifteen minutes on foot from here.",
      "从这里走过去大约十五分钟。",
      {{"It", 140}, {"is", 120}, {"about", 240}, {"fifteen", 320}, {"minutes", 320},
       {"on", 150}, {"foot", 280}, {"from", 180}, {"here.", 320}}));

  s.utterances.push_back(make_utterance(
      3, 8600, 800, "Do you know if the museum is open today?",
      "你知道博物馆今天开门吗?",
      {{"Do", 150}, {"you", 160}, {"know", 260}, {"if", 140}, {"the", 130},
       {"museum", 340}, {"is", 120}, {"open", 280}, {"today?", 340}}));

  s.utterances.push_back(make_utterance(
      4, 12200, 900, "I would like a large latte with no sugar, please.",
      "我要一杯大杯拿铁, 不加糖, 谢谢。",
      {{"I", 140}, {"would", 220}, {"like", 220}, {"a", 130}, {"large", 300},
       {"latte", 340}, {"with", 180}, {"no", 180}, {"sugar,", 300}, {"please.", 340}}));

  s.utterances.push_back(make_utterance(
      5, 16600, 650, "The weather is really nice for a walk today.",
      "今天天气很适合散步。",
      {{"The", 140}, {"weather", 320}, {"is", 120}, {"really", 280}, {"nice", 260},
       {"for", 160}, {"a", 120}, {"walk", 280}, {"today.", 320}}));

  s.utterances.push_back(make_utterance(
      6, 20200, 900, "Thank you so much for your help!",
      "非常感谢你的帮助!",
      {{"Thank", 280}, {"you", 180}, {"so", 200}, {"much", 260}, {"for", 160},
       {"your", 180}, {"help!", 340}}));
  return s;
}

/// 场景 2: 中文说话人, 眼镜把中文字幕翻成英文 —— 反向场景, 验证双向能力。
ConversationScript build_zh2en() {
  ConversationScript s;
  s.name = "中文讲解: 中→英";
  s.source_lang = "zh";
  s.target_lang = "en";

  s.utterances.push_back(make_utterance(
      1, 0, 750, "前面路口右转, 大约三百米就到了。",
      "Turn right at the next intersection, it is about three hundred meters away.",
      {{"前面", 320}, {"路口", 300}, {"右转,", 340}, {"大约", 300},
       {"三百米", 380}, {"就到了。", 420}}));

  s.utterances.push_back(make_utterance(
      2, 3900, 620, "这座桥建于一九零八年, 已经有一百多年历史。",
      "This bridge was built in 1908 and has a history of more than a hundred years.",
      {{"这座桥", 340}, {"建于", 300}, {"一九零八年,", 480}, {"已经有", 300},
       {"一百多年", 380}, {"历史。", 360}}));

  s.utterances.push_back(make_utterance(
      3, 8000, 700, "门票是四十元, 学生可以打半价。",
      "The ticket is forty yuan, and students get half price.",
      {{"门票", 280}, {"是", 180}, {"四十元,", 380}, {"学生", 300},
       {"可以", 260}, {"打半价。", 380}}));

  s.utterances.push_back(make_utterance(
      4, 11600, 800, "如果您时间充裕, 建议先去后山看日落。",
      "If you have enough time, I suggest seeing the sunset from the back hill first.",
      {{"如果您", 300}, {"时间", 280}, {"充裕,", 340}, {"建议", 300},
       {"先去", 260}, {"后山", 280}, {"看日落。", 400}}));

  s.utterances.push_back(make_utterance(
      5, 15900, 700, "最后一班车是晚上九点半, 千万别错过。",
      "The last bus is at half past nine in the evening, do not miss it.",
      {{"最后一班车", 400}, {"是", 160}, {"晚上", 280}, {"九点半,", 400},
       {"千万别", 300}, {"错过。", 340}}));

  s.utterances.push_back(make_utterance(
      6, 19500, 900, "祝您在这里玩得开心!",
      "I hope you enjoy your stay here!",
      {{"祝您", 260}, {"在这里", 300}, {"玩得", 260}, {"开心!", 360}}));
  return s;
}

/// 基准场景: 把两个演示场景各重复 3 遍并缩短句间停顿, 得到 ~36 句连续语音,
/// 用来把流水线压满, 测吞吐与长跑稳定性。
ConversationScript build_bench() {
  ConversationScript s;
  s.name = "压测: 36 句连续对话";
  s.source_lang = "en";
  s.target_lang = "zh";

  const ConversationScript a = build_en2zh();
  const ConversationScript b = build_zh2en();
  int cursor = 0;
  uint64_t id = 1;
  for (int round = 0; round < 3; ++round) {
    for (int k = 0; k < 2; ++k) {
      const ConversationScript& src = (k == 0) ? a : b;
      for (const auto& u : src.utterances) {
        ScriptUtterance copy = u;
        copy.id = id++;
        copy.start_ms = cursor;
        copy.gap_after_ms = 500;  // 压测时说话更密, 但仍要 > hangover(320ms) 才能切开
        s.utterances.push_back(std::move(copy));
        cursor += u.speech_ms() + copy.gap_after_ms;
      }
    }
  }
  s.source_lang = "zh";  // 压测场景中英混排, 语言字段仅作展示
  s.target_lang = "en";
  return s;
}

}  // namespace

int ScriptUtterance::speech_ms() const {
  int total = 0;
  for (const auto& w : words) total += w.dur_ms;
  return total;
}

int ConversationScript::total_ms() const {
  if (utterances.empty()) return 0;
  int end = 0;
  for (const auto& u : utterances) {
    end = std::max(end, u.start_ms + u.speech_ms() + u.gap_after_ms);
  }
  return end;
}

std::vector<int64_t> word_end_offsets(const ScriptUtterance& u) {
  std::vector<int64_t> out;
  out.reserve(u.words.size());
  int64_t acc = 0;
  for (const auto& w : u.words) {
    acc += w.dur_ms;
    out.push_back(acc);
  }
  return out;
}

const ConversationScript& demo_scenario_en2zh() {
  static const ConversationScript kScript = build_en2zh();
  return kScript;
}

const ConversationScript& demo_scenario_zh2en() {
  static const ConversationScript kScript = build_zh2en();
  return kScript;
}

const ConversationScript& bench_scenario() {
  static const ConversationScript kScript = build_bench();
  return kScript;
}

}  // namespace sublens
