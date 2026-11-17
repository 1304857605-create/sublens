/// @file bench_pipeline.cpp  端到端流水线基准
///
/// 这是作品集里最重要的一组数字:
///   - 端到端延迟: "语音结束" → "译文出现在 HUD" 的 p50/p95, 实时节奏下测量;
///   - 吞吐: 引擎自身(去掉模型时延)能跑多少倍实时, 对应眼镜端 CPU 占用;
///   - A/B: 开/关草稿行、本地/云端 MT 对延迟与"闪变次数"的影响。

#include <memory>
#include <string>

#include "bench.hpp"
#include "sublens/audio_source.hpp"
#include "sublens/asr.hpp"
#include "sublens/hud.hpp"
#include "sublens/mt.hpp"
#include "sublens/pipeline.hpp"
#include "sublens/script.hpp"

namespace sublens_bench {
namespace {

/// 取脚本的前 n 句(端到端延迟实验只跑 4 句, 控制等待时间)
sublens::ConversationScript slice(const sublens::ConversationScript& src, int n) {
  sublens::ConversationScript out = src;
  if (static_cast<int>(out.utterances.size()) > n) out.utterances.resize(n);
  return out;
}

struct CaseResult {
  std::string name;
  sublens::PipelineStats stats;
};

/// 跑一个端到端场景
CaseResult run_case(const std::string& name, const sublens::ConversationScript& script,
                    int mt_base_ms, bool draft_enabled) {
  using namespace sublens;
  PipelineConfig pc;
  pc.name = name;
  pc.realtime_pacing = true;  // 端到端延迟必须在真实节奏下测
  pc.draft_enabled = draft_enabled;
  pc.verbose = false;

  ScriptedAudioConfig acfg;
  acfg.script = &script;
  acfg.realtime_pacing = true;

  ScriptedAsrConfig ascfg;
  ascfg.script = &script;

  PhraseMtConfig mcfg;
  mcfg.source_lang = script.source_lang;
  mcfg.target_lang = script.target_lang;

  Pipeline::Deps deps;
  deps.audio = std::make_unique<ScriptedAudioSource>(acfg);
  deps.asr = std::make_unique<ScriptedAsrBackend>(ascfg);
  if (mt_base_ms > 0) {
    deps.mt = std::make_unique<LatencyInjectingMt>(
        std::make_unique<PhraseTableMt>(mcfg), LatencyInjectConfig{mt_base_ms, 80, 11});
  } else {
    deps.mt = std::make_unique<PhraseTableMt>(mcfg);
  }
  deps.hud = std::make_unique<NullHudRenderer>();

  Pipeline pipeline(pc, std::move(deps));
  pipeline.run();
  return CaseResult{name, pipeline.stats()};
}

/// 跑一个"引擎自身吞吐"场景(注入时延全部为 0)
CaseResult run_throughput_case(const std::string& name, const sublens::ConversationScript& script) {
  using namespace sublens;
  PipelineConfig pc;
  pc.name = name;
  pc.realtime_pacing = false;  // 全速跑
  pc.verbose = false;

  ScriptedAudioConfig acfg;
  acfg.script = &script;
  acfg.realtime_pacing = false;

  ScriptedAsrConfig ascfg;
  ascfg.script = &script;
  ascfg.first_partial_delay_ms = 0;
  ascfg.word_step_ms = 0;
  ascfg.final_extra_ms = 0;
  ascfg.jitter_ms = 0;

  PhraseMtConfig mcfg;
  mcfg.source_lang = script.source_lang;
  mcfg.target_lang = script.target_lang;

  Pipeline::Deps deps;
  deps.audio = std::make_unique<ScriptedAudioSource>(acfg);
  deps.asr = std::make_unique<ScriptedAsrBackend>(ascfg);
  deps.mt = std::make_unique<PhraseTableMt>(mcfg);
  deps.hud = std::make_unique<NullHudRenderer>();

  Pipeline pipeline(pc, std::move(deps));
  pipeline.run();
  return CaseResult{name, pipeline.stats()};
}

}  // namespace

BenchReport bench_pipeline_latency() {
  using namespace sublens;
  BenchReport r;
  r.id = "latency";
  r.title = "端到端延迟(实时节奏, 合成音频 + 模拟首字延迟 260ms)";

  const ConversationScript script = slice(demo_scenario_en2zh(), 4);

  std::vector<CaseResult> cases;
  cases.push_back(run_case("A 本地 MT + 草稿行", script, 0, true));
  cases.push_back(run_case("B 云端 MT(注入 180±80ms) + 草稿行", script, 180, true));
  cases.push_back(run_case("C 本地 MT + 关闭草稿行", script, 0, false));

  std::string md;
  md += "延迟口径: 语音结束(端点判定) → 第一段译文应用到 HUD。"
        "实时节奏下测量, 单位为 ms。\n\n";
  md += "| 场景 | 端到端 p50 | p95 | p99 | 首 partial p50 | MT 往返 p50 | 提交片段 | HUD 整行替换 | 是否达标(≤700ms) |\n";
  md += "|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& c : cases) {
    const auto& s = c.stats;
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "| %s | %lld | %lld | %lld | %lld | %lld | %llu | %llu | %s |\n",
                  c.name.c_str(), static_cast<long long>(s.speech_end_to_hud.p50),
                  static_cast<long long>(s.speech_end_to_hud.p95),
                  static_cast<long long>(s.speech_end_to_hud.p99),
                  static_cast<long long>(s.speech_end_to_first_partial.p50),
                  static_cast<long long>(s.mt_roundtrip.p50),
                  static_cast<unsigned long long>(s.commits),
                  static_cast<unsigned long long>(s.hud_revisions),
                  s.speech_end_to_hud.count == 0
                      ? "无样本"
                      : (s.speech_end_to_hud.p95 <= 700 ? "达标" : "超标"));
    md += buf;
  }
  md += "\n";
  md += "> 结论: 端到端延迟的绝对大头是 ASR 首字延迟(260ms); 本地 MT 只占个位数毫秒; "
        "云端 MT 会再加 180ms 左右。草稿行会把 HUD 整行替换次数明显抬高(换来更早看到内容), "
        "这是产品上要权衡的点 —— 本项目的默认策略是「稳定优先, 草稿可关」。\n";
  r.markdown = md;
  return r;
}

BenchReport bench_pipeline_throughput() {
  using namespace sublens;
  BenchReport r;
  r.id = "throughput";
  r.title = "引擎自身吞吐(注入时延置 0, 全速跑 36 句 / 约 130 秒音频)";

  const ConversationScript script = bench_scenario();
  const CaseResult c = run_throughput_case("36 句压测", script);

  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "| 指标 | 数值 |\n|---|---|\n"
                "| 音频总时长 | %.1f 秒 |\n"
                "| 墙钟耗时 | %.3f 秒 |\n"
                "| 实时倍率 | %.1fx |\n"
                "| 单核 CPU 占用(估算) | %.2f%% |\n"
                "| 检出句数 | %llu |\n"
                "| 提交翻译片段 | %llu |\n"
                "| 音频丢帧(样本) | %llu |\n"
                "| VAD 单帧 p95 | %lld us |\n\n",
                c.stats.audio_s, c.stats.elapsed_s, c.stats.realtime_factor,
                100.0 / (c.stats.realtime_factor > 0 ? c.stats.realtime_factor : 1),
                static_cast<unsigned long long>(c.stats.utterances),
                static_cast<unsigned long long>(c.stats.commits),
                static_cast<unsigned long long>(c.stats.dropped_samples),
                static_cast<long long>(c.stats.vad_frame.p95));
  r.markdown = std::string(buf) +
               "> 说明: 这里把 ASR 首字延迟与 MT 时延都置 0, 测的是「纯引擎」能跑多快。"
               "实时倍率越高, 眼镜端留给系统其它任务(显示/传感器/功耗)的 CPU 余量越大。\n";
  return r;
}

}  // namespace sublens_bench
