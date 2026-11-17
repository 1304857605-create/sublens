/// @file ansi_renderer.cpp  终端 ANSI HUD: 模拟"眼镜第一视角"的两行字幕
///
/// 设计目标有两个, 而且互相牵制:
///   1) 录屏效果: 要在终端里看起来像一块悬在眼前的 HUD, 只能原地重绘,
///      绝不能每帧往下滚 —— 滚动画面录出来是"日志", 不是"字幕";
///   2) 可测性: 提供不带任何控制字符的纯文本帧, 测试可以断言每行显示宽度。
///
/// 为什么边框用 ASCII 的 ".---." 而不是 Unicode 圆角框线:
/// common.hpp 的 display_width() 把所有 3 字节 UTF-8 字符一律算 2 列,
/// 而框线字符(U+2500~)在终端里实际只占 1 列。用它做边框会让"记账宽度"
/// 和"物理宽度"不一致, 每行都会错位一格。ASCII 边框两边口径一致。
/// 同理, 截断用 "..." 而不是单字符省略号 "…"(U+2026 也会被多算 1 列)。

#include "sublens/hud.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sublens/common.hpp"

namespace sublens {
namespace {

// --- ANSI 转义片段 ---------------------------------------------------------
constexpr const char* kReset = "\x1b[0m";
constexpr const char* kDim = "\x1b[2;36m";      // 边框/底栏: 暗青色, 不抢注意力
constexpr const char* kHeader = "\x1b[1;36m";   // 标题: 高亮青色
constexpr const char* kMain = "\x1b[1;97m";     // 译文主行: 最高对比度
constexpr const char* kDraft = "\x1b[2;37m";    // 草稿行: 灰暗, 明确"还没定"
constexpr const char* kHint = "\x1b[2;37m";     // 占位提示

constexpr int kMinWidth = 24;
constexpr int kMaxWidth = 160;

struct RenderedLine {
  std::string text;    ///< 纯文本(不含转义序列), 显示宽度 <= width
  const char* style;   ///< 着色用的 ANSI 前缀, 空指针=不着色
};

/// 一个码点相对 display_width() 的列宽(必须与 display_width 完全一致,
/// 否则"裁剪 + 补空格"之后整行宽度会和终端对不上)。
struct Codepoint {
  size_t offset = 0;
  size_t bytes = 1;
  int width = 1;
};

std::vector<Codepoint> split_codepoints(const std::string& s) {
  std::vector<Codepoint> out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    Codepoint cp;
    cp.offset = i;
    if (c < 0x80) {
      cp.bytes = 1;
      cp.width = 1;
    } else if ((c >> 5) == 0x6) {
      cp.bytes = 2;
      cp.width = 1;
    } else if ((c >> 4) == 0xE) {
      cp.bytes = 3;
      cp.width = 2;
    } else {
      cp.bytes = 4;
      cp.width = 2;
    }
    // 非法 UTF-8(尾字节缺失)时退化成单字节, 保证不会越界/死循环
    if (i + cp.bytes > s.size()) cp.bytes = 1;
    i += cp.bytes;
    out.push_back(cp);
  }
  return out;
}

/// 补空格到指定显示宽度(调用方已保证 s 不会超宽)。
std::string pad_to_width(const std::string& s, int width) {
  const size_t w = display_width(s);
  if (w >= static_cast<size_t>(width)) return s;
  return s + std::string(static_cast<size_t>(width) - w, ' ');
}

/// 保留开头、末尾用 "..." 截断(草稿行用: 未定稿的内容从句子开头开始读)。
std::string clip_keep_head(const std::string& s, int max_width) {
  if (max_width <= 0) return std::string();
  if (display_width(s) <= static_cast<size_t>(max_width)) return s;
  const int budget = max_width - 3;  // 给 "..." 留 3 列
  if (budget <= 0) return "...";
  const auto cps = split_codepoints(s);
  int used = 0;
  size_t end = 0;
  for (const auto& cp : cps) {
    if (used + cp.width > budget) break;
    used += cp.width;
    end = cp.offset + cp.bytes;
  }
  return s.substr(0, end) + "...";
}

/// 保留结尾、开头用 "..." 截断(译文主行用: 渐进字幕里最新的字最重要)。
std::string clip_keep_tail(const std::string& s, int max_width) {
  if (max_width <= 0) return std::string();
  if (display_width(s) <= static_cast<size_t>(max_width)) return s;
  const int budget = max_width - 3;
  if (budget <= 0) return "...";
  const auto cps = split_codepoints(s);
  int used = 0;
  size_t start = s.size();
  for (size_t i = cps.size(); i-- > 0;) {
    if (used + cps[i].width > budget) break;
    used += cps[i].width;
    start = cps[i].offset;
  }
  if (start >= s.size()) return "...";
  return "..." + s.substr(start);
}

/// 内容行: "|" + 内容(裁剪并补到 inner 列) + "|"。
/// 宽度记账全部基于 display_width, 保证纯文本帧每行恰好 width 列。
void make_row(const std::string& content, int inner, const char* style, RenderedLine* out) {
  const std::string body = pad_to_width(clip_keep_head(content, inner), inner);
  out->text = "|" + body + "|";
  out->style = style;
}

/// 标题行: 左标题 + 状态, 右对齐"当前句号 + 最近延迟"。
std::string make_header_content(const std::string& header, const std::string& status,
                                const HudSnapshot& s, int inner) {
  std::string right;
  if (s.cur_utt_id != 0) right = "#" + std::to_string(s.cur_utt_id);
  if (s.last_latency_ms > 0) {
    if (!right.empty()) right += "  ";
    right += std::to_string(static_cast<long long>(s.last_latency_ms)) + "ms";
  }
  right += ' ';  // 右对齐信息与右边框之间留 1 列, 否则贴边很难看
  std::string left = " " + header + "  -  " + status;
  const size_t right_w = display_width(right);
  // 空间不够时先砍标题(标题永远没有正文重要), 保证右对齐信息完整
  if (display_width(left) + right_w > static_cast<size_t>(inner)) {
    const int left_budget = inner - static_cast<int>(right_w);
    left = left_budget > 0 ? clip_keep_head(left, left_budget) : std::string();
  }
  const size_t left_w = display_width(left);
  const size_t used = left_w + right_w;
  const size_t gap = static_cast<size_t>(inner) > used ? static_cast<size_t>(inner) - used : 0;
  return left + std::string(gap, ' ') + right;
}

/// 画一帧。返回固定 8 行, 行高不随内容变化 ——
/// 行高恒定是在终端里"原地重绘"不闪的前提(否则光标每次都要跳)。
std::vector<RenderedLine> build_frame(const HudSnapshot& s, int width, const std::string& header,
                                      bool show_metrics, double fps) {
  width = std::clamp(width, kMinWidth, kMaxWidth);
  const int inner = width - 2;

  std::vector<RenderedLine> lines;
  lines.reserve(8);

  const std::string status =
      s.draft_active ? "listening" : (s.finalized > 0 ? "idle" : "ready");

  // 1) 顶框(用 ASCII 圆角, 理由见文件头的注释)
  lines.push_back({"." + std::string(inner, '-') + ".", kDim});

  // 2) 标题 + 状态
  RenderedLine header_line;
  make_row(make_header_content(header, status, s, inner), inner, kHeader, &header_line);
  lines.push_back(header_line);

  // 3) 分隔线
  lines.push_back({"|" + std::string(inner, '-') + "|", kDim});

  // 4) 译文主行: 已提交内容只增不改, 太长时显示尾部(最新文字)
  {
    std::string content;
    const char* style = kMain;
    if (s.cur_dst.empty()) {
      content = "  (waiting for speech)";
      style = kHint;
    } else {
      content = "  " + clip_keep_tail(s.cur_dst, inner - 2);
    }
    RenderedLine row;
    make_row(content, inner, style, &row);
    lines.push_back(row);
  }

  // 5) 草稿行: 暗色 + "..." 前缀, 明示"这行还会变"
  {
    std::string content;
    const char* style = kDraft;
    if (s.draft_active) {
      const std::string& text = !s.draft_dst.empty() ? s.draft_dst : s.draft_src;
      content = "  ... " + clip_keep_head(text, inner - 6);
    } else {
      content = "  ...";
      style = kHint;
    }
    RenderedLine row;
    make_row(content, inner, style, &row);
    lines.push_back(row);
  }

  // 6) 空行: 把主行/草稿行和底栏在视觉上分开(行高恒定, 见函数头注释)
  {
    RenderedLine row;
    make_row("", inner, kHint, &row);
    lines.push_back(row);
  }

  // 7) 指标条(可选)
  {
    std::string content;
    if (show_metrics) {
      content = "  延迟 " + std::to_string(static_cast<long long>(s.last_latency_ms)) + "ms";
      if (fps >= 0.0) {
        content += "   FPS ";
        if (fps > 0.0) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.1f", fps);
          content += buf;
        } else {
          content += "--";  // 样本还不够, 不显示假帧率
        }
      }
      content += "   已定稿 " + std::to_string(static_cast<unsigned long long>(s.finalized)) + " 句";
    }
    RenderedLine row;
    make_row(content, inner, kDim, &row);
    lines.push_back(row);
  }

  // 8) 底框
  lines.push_back({"'" + std::string(inner, '-') + "'", kDim});

  return lines;
}

/// 把一帧拼成纯文本(行间用 '\n', 末尾不加换行, 方便测试按行断言)。
std::string frame_to_text(const std::vector<RenderedLine>& lines, bool color) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) out += '\n';
    if (color && lines[i].style != nullptr) {
      out += lines[i].style;
      out += lines[i].text;
      out += kReset;
    } else {
      out += lines[i].text;
    }
  }
  return out;
}

void write_stdout(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

}  // namespace

// ---------------------------------------------------------------------------
// AnsiHudRenderer
// ---------------------------------------------------------------------------

struct AnsiHudRenderer::Impl {
  Config cfg;
  bool started = false;
  uint64_t frames = 0;
  double fps = 0.0;
  int64_t last_render_ms = 0;
  int64_t last_paint_ms = 0;
  std::string last_signature;  ///< 快照签名: 内容没变就不重绘, 省终端带宽和 CPU

  // 帧率用"最近 N 个间隔"的滑动平均, 而不是从 start() 起算的累计平均值:
  // 累计平均在启动阶段会被一个 1ms 的间隔拉成 1000FPS, 然后一路下滑, 观感很业余。
  // 滑动窗口在 1.5s 内就稳定在 hud_fps 附近, 单帧抖动也被平均掉。
  static constexpr int kFpsWindow = 30;
  int64_t dt_window[kFpsWindow] = {};
  int dt_count = 0;
  int dt_pos = 0;
};

AnsiHudRenderer::AnsiHudRenderer(Config cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = std::move(cfg);
}

AnsiHudRenderer::~AnsiHudRenderer() = default;

bool AnsiHudRenderer::start() {
  if (impl_->started) return true;
  impl_->started = true;
  impl_->frames = 0;
  impl_->fps = 0.0;
  impl_->last_render_ms = 0;
  impl_->last_paint_ms = 0;
  impl_->dt_count = 0;
  impl_->dt_pos = 0;
  impl_->last_signature.clear();
  // 隐藏光标 + 清屏回家: 光标是"这是日志"的视觉锚点, HUD 里不能出现;
  // 清屏是为了把之前的 shell 输出赶出去, 录屏开头就是一块干净画面。
  write_stdout(std::string("\x1b[?25l") + "\x1b[2J" + "\x1b[H" + kReset);
  return true;
}

void AnsiHudRenderer::stop() {
  if (!impl_->started) return;
  impl_->started = false;
  // 恢复颜色与光标, 再换一行, 让 shell 提示符从 HUD 下方开始
  write_stdout(std::string(kReset) + "\x1b[?25h" + "\n");
}

void AnsiHudRenderer::render(const HudSnapshot& s, int64_t now_ms_value) {
  if (!impl_->started) return;

  // 帧率 = 最近 kFpsWindow 个渲染间隔的滑动平均。小于 5ms 的间隔视为
  // "同一拍内的重复调用"(收尾补帧/重复 render), 不计入, 否则会把帧率瞬间拉爆。
  if (impl_->last_render_ms != 0) {
    const int64_t dt = now_ms_value - impl_->last_render_ms;
    if (dt >= 5) {
      impl_->dt_window[impl_->dt_pos] = dt;
      impl_->dt_pos = (impl_->dt_pos + 1) % Impl::kFpsWindow;
      if (impl_->dt_count < Impl::kFpsWindow) ++impl_->dt_count;
    }
  }
  impl_->last_render_ms = now_ms_value;
  ++impl_->frames;

  if (impl_->dt_count >= 3) {
    int64_t sum_ms = 0;
    for (int i = 0; i < impl_->dt_count; ++i) sum_ms += impl_->dt_window[i];
    // 注意 sum 里是 dt_count 个间隔, 所以帧率 = 间隔数 / 总时长
    impl_->fps = sum_ms > 0 ? 1000.0 * static_cast<double>(impl_->dt_count) /
                                  static_cast<double>(sum_ms)
                            : 0.0;
  } else {
    impl_->fps = 0.0;  // 样本不足时不显示数字, 避免一启动就跳出一个假帧率
  }

  // 差量重绘: 快照没变时不必每帧都往终端写(20fps × 8 行的写入在慢终端上会拖慢流水线),
  // 但留 250ms 心跳, 保证 FPS 指标还在动、画面不会被认为是卡死。
  std::string signature;
  signature.reserve(256);
  signature += s.cur_dst;
  signature += '\x1f';
  signature += s.draft_dst;
  signature += '\x1f';
  signature += s.draft_active ? '1' : '0';
  signature += '\x1f';
  signature += std::to_string(s.finalized);
  signature += '\x1f';
  signature += std::to_string(static_cast<long long>(s.last_latency_ms));
  signature += '\x1f';
  signature += std::to_string(s.cur_utt_id);
  const bool unchanged = signature == impl_->last_signature;
  if (unchanged && now_ms_value - impl_->last_paint_ms < 250) return;
  impl_->last_signature = std::move(signature);
  impl_->last_paint_ms = now_ms_value;

  const auto lines = build_frame(s, impl_->cfg.width, impl_->cfg.header, impl_->cfg.show_metrics,
                                 impl_->fps);

  // 原地重绘: 先回家(\x1b[H), 再逐行写内容并用 \x1b[K 清掉本行剩余字符。
  // 不在这里用 \x1b[2J 清屏 —— 那会闪; 靠 \x1b[K 覆盖旧内容即可。
  std::string out = "\x1b[H";
  for (size_t i = 0; i < lines.size(); ++i) {
    if (impl_->cfg.color && lines[i].style != nullptr) {
      out += lines[i].style;
      out += lines[i].text;
      out += kReset;
    } else {
      out += lines[i].text;
    }
    out += "\x1b[K";
    if (i + 1 != lines.size()) out += '\n';
  }
  write_stdout(out);
}

std::string AnsiHudRenderer::render_to_string(const HudSnapshot& s, int width, bool color) {
  const auto lines = build_frame(s, width, "SubLens HUD", true, -1.0);
  return frame_to_text(lines, color);
}

}  // namespace sublens
