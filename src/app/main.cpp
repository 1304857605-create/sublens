/// @file main.cpp  sublens_demo: 可录屏的终端演示程序
///
/// 一个"能跑起来给人看"的 demo 比一堆单元测试更能说明问题, 所以这个 main
/// 只做四件事: 解析参数 → 按场景组装依赖 → 跑流水线 → 打印最终画面与统计。
///
/// Windows 终端有两个坑, 这里一次性处理掉:
///   1) 默认代码页不是 UTF-8, 中文/边框会变乱码 → SetConsoleOutputCP(CP_UTF8),
///      并且全链路只输出 UTF-8 字节(不做 _O_U8TEXT 宽字符重定向, 那会破坏 printf);
///   2) 老 conhost 不解析 ANSI 转义 → 打开 ENABLE_VIRTUAL_TERMINAL_PROCESSING,
///      否则 HUD 的原地重绘会变成满屏的 "[H[K"。

#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "sublens/asr.hpp"
#include "sublens/audio_source.hpp"
#include "sublens/common.hpp"
#include "sublens/hud.hpp"
#include "sublens/mt.hpp"
#include "sublens/pipeline.hpp"
#include "sublens/script.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// Ctrl+C 要能从"任意线程(控制台事件线程)"通知流水线, 所以用一个全局原子指针。
// 不用信号量/回调而是原子指针: 处理函数里只做一次 request_stop(), 足够简单也足够快。
std::atomic<sublens::Pipeline*> g_pipeline{nullptr};

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
    sublens::Pipeline* p = g_pipeline.load(std::memory_order_relaxed);
    if (p != nullptr) p->request_stop();
    return TRUE;  // 吞掉事件: 让流水线自己按顺序收尾, 而不是把进程直接杀掉
  }
  return FALSE;
}
#else
void posix_sigint_handler(int) {
  sublens::Pipeline* p = g_pipeline.load(std::memory_order_relaxed);
  if (p != nullptr) p->request_stop();
}
#endif

void setup_console() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
  SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
  std::signal(SIGINT, posix_sigint_handler);
#endif
}

struct DemoOptions {
  std::string scenario = "en2zh";
  bool fast = false;     ///< 关掉实时节奏, 全速跑
  bool color = true;     ///< 是否给 HUD 上色
  bool draft = true;     ///< 是否启用草稿行(--no-draft 时关)
  int fps = 20;          ///< HUD 刷新率
};

void print_usage(const char* argv0) {
  std::fprintf(stderr,
               "用法: %s [en2zh|zh2en|bench] [--fast] [--no-color] [--no-draft] [--fps=N]\n"
               "  en2zh    英→中 旅游对话(默认, 实时节奏, 约 25 秒, 适合录屏)\n"
               "  zh2en    中→英 讲解对话\n"
               "  bench    30 句连续对话压测(默认全速)\n"
               "  --fast   关掉实时节奏, 尽快跑完(基准/回归)\n"
               "  --no-color   HUD 不着色(录屏/日志重定向时更干净)\n"
               "  --no-draft   关闭草稿行推测翻译(用于 A/B 对比)\n"
               "  --fps=N      HUD 刷新率(默认 20)\n",
               argv0);
}

bool parse_args(int argc, char** argv, DemoOptions* opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "--fast") {
      opt->fast = true;
    } else if (arg == "--no-color") {
      opt->color = false;
    } else if (arg == "--no-draft") {
      opt->draft = false;
    } else if (arg.rfind("--fps=", 0) == 0) {
      opt->fps = std::atoi(arg.c_str() + 6);
    } else if (arg == "--fps") {
      if (i + 1 >= argc) return false;
      opt->fps = std::atoi(argv[++i]);
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
      return false;
    } else {
      opt->scenario = arg;
    }
  }
  if (opt->scenario != "en2zh" && opt->scenario != "zh2en" && opt->scenario != "bench") {
    std::fprintf(stderr, "未知场景: %s\n", opt->scenario.c_str());
    return false;
  }
  if (opt->fps < 1 || opt->fps > 120) {
    std::fprintf(stderr, "非法 HUD 帧率: %d (允许 1~120)\n", opt->fps);
    return false;
  }
  return true;
}

std::string upper_lang(const std::string& lang) {
  std::string out = lang;
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

/// 包一层渲染器: 一边把帧转给真正的 ANSI 渲染器, 一边把"最能代表产品体验"的
/// 那一帧存下来。程序结束后打印成纯文本, 既是 demo 的"收官画面",
/// 也方便直接贴进 README/汇报里(终端截图/录屏之外的一份可复制证据)。
class CapturingHudRenderer final : public sublens::HudRenderer {
 public:
  explicit CapturingHudRenderer(std::unique_ptr<sublens::HudRenderer> inner)
      : inner_(std::move(inner)) {}

  bool start() override { return inner_->start(); }
  void stop() override { inner_->stop(); }

  void render(const sublens::HudSnapshot& s, int64_t now_ms) override {
    last_ = s;
    // 留"主行是完整句子 + 下一句正在出草稿"的画面: 这是最能体现"渐进式字幕"
    // 的一帧(已定稿的一行 + 正在推演的草稿行)。不断用更晚的候选覆盖, 收官
    // 画面因此取自对话中后段, 而不是第一句刚出来时的空画面。
    const bool complete_line =
        s.cur_dst.ends_with("。") || s.cur_dst.ends_with("！") || s.cur_dst.ends_with("？") ||
        s.cur_dst.ends_with("!") || s.cur_dst.ends_with("?") || s.cur_dst.ends_with(".");
    if (complete_line && s.draft_active) {
      showcase_ = s;
      have_showcase_ = true;
    }
    inner_->render(s, now_ms);
  }

  const char* name() const override { return inner_->name(); }

  /// render() 只在 HUD 线程 / 收尾线程里调用; run() 返回后由 main 读取是安全的。
  const sublens::HudSnapshot& showcase() const { return have_showcase_ ? showcase_ : last_; }

 private:
  std::unique_ptr<sublens::HudRenderer> inner_;
  sublens::HudSnapshot last_;
  sublens::HudSnapshot showcase_;
  bool have_showcase_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  setup_console();

  DemoOptions opt;
  if (!parse_args(argc, argv, &opt)) {
    print_usage(argv[0]);
    return 1;
  }

  // ---- 选场景: 一份脚本同时驱动音频、ASR、参考译文 ----
  const sublens::ConversationScript* script = nullptr;
  if (opt.scenario == "zh2en") {
    script = &sublens::demo_scenario_zh2en();
  } else if (opt.scenario == "bench") {
    script = &sublens::bench_scenario();
  } else {
    script = &sublens::demo_scenario_en2zh();
  }

  // bench 是压测场景, 默认全速; 演示场景默认按真实时间走(录屏才像直播)
  const bool realtime = !opt.fast && opt.scenario != "bench";

  SL_LOG(Info) << "场景: " << script->name << " | 实时节奏=" << (realtime ? "on" : "off")
               << " | 草稿=" << (opt.draft ? "on" : "off") << " | HUD " << opt.fps << "fps";

  // ---- 组装依赖(全部走接口注入: 真机上换成云端 ASR/MT 也是同一份编排代码) ----
  sublens::ScriptedAudioConfig audio_cfg;
  audio_cfg.script = script;
  audio_cfg.realtime_pacing = realtime;
  auto audio = std::make_unique<sublens::ScriptedAudioSource>(audio_cfg);

  sublens::ScriptedAsrConfig asr_cfg;
  asr_cfg.script = script;
  auto asr = std::make_unique<sublens::ScriptedAsrBackend>(asr_cfg);

  sublens::PhraseMtConfig mt_cfg;
  mt_cfg.source_lang = script->source_lang;
  mt_cfg.target_lang = script->target_lang;
  auto mt = std::make_unique<sublens::PhraseTableMt>(mt_cfg);

  sublens::AnsiHudConfig hud_cfg;
  hud_cfg.width = 68;
  hud_cfg.show_metrics = true;
  hud_cfg.color = opt.color;
  hud_cfg.header = "SubLens HUD  " + upper_lang(script->source_lang) + " -> " +
                   upper_lang(script->target_lang);
  auto hud = std::make_unique<CapturingHudRenderer>(
      std::make_unique<sublens::AnsiHudRenderer>(hud_cfg));
  CapturingHudRenderer* hud_view = hud.get();  // 生命周期跟着 Pipeline::Deps 走, main 结束前都有效

  sublens::PipelineConfig pc;
  pc.name = std::string("sublens-demo/") + opt.scenario;
  pc.hud_fps = opt.fps;
  pc.realtime_pacing = realtime;
  pc.draft_enabled = opt.draft;  // --no-draft 直接透传给调度器(契约: PipelineConfig.draft_enabled)
  pc.target_latency_ms = 700;
  pc.verbose = true;

  sublens::Pipeline::Deps deps;
  deps.audio = std::move(audio);
  deps.asr = std::move(asr);
  deps.mt = std::move(mt);
  deps.hud = std::move(hud);

  {
    sublens::Pipeline pipeline(pc, std::move(deps));
    g_pipeline.store(&pipeline, std::memory_order_relaxed);
    pipeline.run();  // 阻塞: 音频结束/Ctrl+C 后按固定顺序收尾
    g_pipeline.store(nullptr, std::memory_order_relaxed);

    // to_table/to_markdown 里没有 to_table 之外的统计, 统一在 run() 返回后取一次快照
    const sublens::PipelineStats stats = pipeline.stats();

    // ---- 收官画面 + 统计表 ----
    // 先打印"最后一帧 HUD 的纯文本版本", 相当于一张可复制的截图;
    // 再打印对齐的统计表, 最后给一句是否达标的结论。
    std::printf("\n");
    const std::string frame =
        sublens::AnsiHudRenderer::render_to_string(hud_view->showcase(), hud_cfg.width, false);
    std::printf("%s\n\n", frame.c_str());
    std::printf("%s\n\n", stats.to_table().c_str());

    if (!realtime) {
      // 全速模式下音频比墙钟走得快, "语音结束 → 上屏"的墙钟差值没有物理意义,
      // pipeline 在非实时模式下不记延迟直方图(见 pipeline.cpp), 这里也不下达标结论。
      std::printf(
          "结论: 非实时模式(realtime_pacing=false), 延迟直方图不记录(口径会失真), "
          "本模式只看计数与吞吐; 要跑达标结论请用实时模式(去掉 --fast)。\n");
      return 0;
    }
    if (stats.speech_end_to_hud.count == 0) {
      std::printf("结论: 没有采集到端到端延迟样本(音频/回调可能没跑起来), 无法判断是否达标。\n");
      return 1;
    }
    const bool met = stats.speech_end_to_hud.p95 <= pc.target_latency_ms;
    std::printf(
        "结论: speech_end_to_hud p95 = %lldms (目标 %dms) -> %s%s\n",
        static_cast<long long>(stats.speech_end_to_hud.p95), pc.target_latency_ms,
        met ? "达标" : "未达标",
        opt.draft ? "" : " [--no-draft: 草稿行已关闭]");
    return 0;
  }
}

