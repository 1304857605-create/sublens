#pragma once
/// @file script.hpp
/// @brief "对话脚本": 演示与回归测试的唯一数据源。
///
/// 离线演示没有真实麦克风也没有云端模型, 于是整条链路用同一份时间线驱动:
///   - ScriptedAudioSource 按这份脚本合成音频(句内有语音、句间静音),
///   - ScriptedAsrBackend 按这份脚本逐词吐 partial,
///   - 单元测试用 source_text / target_text 做参考对比。
/// 三者共享同一个时间线, 所以 VAD 切出的语句边界和 ASR 的 partial 是对齐的,
/// 演示时看起来就是"真的在实时翻译", 而不是一段硬编码动画。

#include <cstdint>
#include <string>
#include <vector>

namespace sublens {

/// 脚本里的一个"词": 中文按词/短语, 英文按单词, 带朗读时长。
struct ScriptWord {
  std::string text;
  int dur_ms = 220;
};

/// 一句话。
struct ScriptUtterance {
  uint64_t id = 0;
  int start_ms = 0;         ///< 相对对话起点的开口时间
  int gap_after_ms = 550;   ///< 说完后的静音时长
  std::string source_text;  ///< 完整原文(参考)
  std::string target_text;  ///< 参考译文(参考, 离线翻译器不保证逐字相同)
  std::vector<ScriptWord> words;

  /// 这句话从开口到闭嘴的时长
  int speech_ms() const;
};

struct ConversationScript {
  std::string name;
  std::string source_lang = "zh";
  std::string target_lang = "en";
  std::vector<ScriptUtterance> utterances;

  /// 整段对话的总时长(含句间静音)
  int total_ms() const;
};

/// 内置场景: 中文说话人 ← 英文外宾(眼镜把英文翻成中文字幕)
const ConversationScript& demo_scenario_en2zh();
/// 内置场景: 中文说话人 → 英文(眼镜把中文翻成英文字幕)
const ConversationScript& demo_scenario_zh2en();
/// 压测场景: 30 句连续对话, 用于流水线基准测试
const ConversationScript& bench_scenario();

/// 解析脚本里的词, 得到逐词结束时间(相对本句开口), 供 ASR 模拟器使用。
std::vector<int64_t> word_end_offsets(const ScriptUtterance& u);

}  // namespace sublens
