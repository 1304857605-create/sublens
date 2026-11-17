/// @file bench_hud.cpp  HUD 状态机与渲染基准
///
/// 眼镜端的 HUD 是 20fps 左右的固定刷新, 每帧必须远小于 50ms;
/// 这里测"折叠一次更新"和"画一帧"的 CPU 成本。
///
/// 注意: "草稿整行替换次数"这类体验指标不在本文件测 —— 它取决于真实的
/// 时间分布(节流是按时钟算的), 紧凑循环里测出来没有意义。那些数字由
/// bench_pipeline_latency 在实时节奏下给出。

#include <string>

#include "bench.hpp"
#include "sublens/hud.hpp"

namespace sublens_bench {

BenchReport bench_hud() {
  using namespace sublens;
  BenchReport r;
  r.id = "hud";
  r.title = "HUD 状态机 / 渲染";

  const int kIters = 1'000'000;

  // 1) 已定稿更新(最常走的路径: 主行追加)
  double ns_commit = 0;
  {
    HudState state;
    HudUpdate update;
    update.kind = HudUpdate::Kind::kCommitted;
    update.utt_id = 1;
    update.src = "the train station";
    update.dst = "火车站";
    Stopwatch sw;
    for (int i = 0; i < kIters; ++i) state.apply(update);
    ns_commit = static_cast<double>(sw.ns()) / kIters;
  }

  // 2) 草稿更新(受节流的路径: 大部分会被丢弃, 所以更便宜)
  double ns_draft = 0;
  {
    HudState state;
    HudUpdate update;
    update.kind = HudUpdate::Kind::kDraft;
    update.utt_id = 1;
    update.src = "could you tell me";
    update.dst = "你能告诉我";
    Stopwatch sw;
    for (int i = 0; i < kIters; ++i) state.apply(update);
    ns_draft = static_cast<double>(sw.ns()) / kIters;
  }

  // 3) 画一帧(纯文本渲染, 68 列的中英混排)
  HudSnapshot snap;
  snap.cur_dst = "打扰一下, 请问火车站怎么走?";
  snap.draft_dst = "你能告诉我";
  snap.draft_active = true;
  snap.finalized = 3;
  snap.last_latency_ms = 419;
  const int kFrames = 20'000;
  double us_per_frame = 0;
  {
    Stopwatch sw;
    std::string out;
    for (int i = 0; i < kFrames; ++i) out = AnsiHudRenderer::render_to_string(snap, 68, false);
    us_per_frame = static_cast<double>(sw.ns()) / kFrames / 1000.0;
    (void)out.size();
  }

  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "| 指标 | 数值 |\n|---|---|\n"
                "| HudState::apply (kCommitted) | %.1f ns/次 |\n"
                "| HudState::apply (kDraft) | %.1f ns/次 |\n"
                "| render_to_string | %.2f us/帧 |\n"
                "| 20fps 每帧预算占用 | %.4f%% |\n\n",
                ns_commit, ns_draft, us_per_frame, us_per_frame / 50'000.0 * 100.0);
  r.markdown = std::string(buf) +
               "> 结论: HUD 每帧成本远小于 20fps 的 50ms 预算, 渲染不会是瓶颈; "
               "真正的瓶颈永远是网络与模型。\n";
  return r;
}

}  // namespace sublens_bench
