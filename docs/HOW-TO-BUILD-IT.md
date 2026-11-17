# 从零写一遍:SubLens 的实现顺序

> 这份文档是"怎么写"的答案: 如果你要从空目录把这个项目写出来(或者面试时被要求
> 讲"你是怎么想到这么做的"), 按这 10 步走, 每一步都有明确产出和验收标准。
> 每一步对应的真实代码都在本仓库里, 路径已标出。

**核心原则: 先把"延迟口径"定下来, 再写代码。** 没有口径的延迟优化全是玄学。

---

## 第 0 步:先把口径写进文档(半天)

在写第一行代码前, 先在纸/文档上定死四件事:

1. **端到端延迟 = 说话人闭嘴 → 译文出现在眼前**(不是"说话开始", 那会把句子长度算进去);
2. 测量用**单调时钟**, 不用系统时间(系统时间会被 NTP 跳变);
3. 每句只记**第一次上屏**的延迟(否则一句话说 3 秒, 最后一段必然晚 3 秒, p95 被拖爆);
4. 一个"稳不稳"的指标: **草稿行整行替换次数**(revisions)。

本仓库的落地: `include/sublens/pipeline.hpp` 里 `PipelineStats` 的字段注释、
`docs/LATENCY-BUDGET.md`。这一步决定了后面所有代码长什么样。

> 坑: 很多人先写功能, 最后才想怎么测, 结果指标全是"感觉快了"。

---

## 第 1 步:公共底座(1 天)

**目标**: 有计时、有日志、有延迟直方图, 后面每一步都能立刻量化。

写 `include/sublens/common.hpp` + `src/common/stats.cpp`:

```cpp
// 单调毫秒: 只用于测延迟
inline int64_t now_ms();

// 定宽桶直方图: O(1) 记录、固定内存、能出 p50/p95/p99
class LatencyHistogram {
  void record(int64_t v);
  Snapshot snapshot() const;   // count/min/p50/p95/p99/max/mean
};
```

**验收**: 写个 `main` 往直方图塞 100 万个随机数, `p50` 和手动排序结果差 < 桶宽。

> 坑: 用 `std::vector` 存全部样本, 长跑程序内存会涨; 用 map 存分位数则慢。

---

## 第 2 步:音频通道 + 合成音源(1~2 天)

**目标**: 不依赖麦克风也能稳定产出 20ms 一帧的 PCM16 数据, 并且**时间线可控**。

1. `include/sublens/ring_buffer.hpp`: 单生产者/单消费者无锁环形队列, 容量 2 的幂。
   两条 atomic 各占一条 cache line(防伪共享), 生产者只写 `head_`, 消费者只写 `tail_`。
2. `include/sublens/audio_source.hpp`: 抽象 `IAudioSource { read(int16_t*, int) }`,
   返回 `>0` 样本数 / `0` 暂无数据 / `-1` 结束。
3. `src/audio/scripted_audio_source.cpp`: 按"对话脚本"合成音频 —— 句内谐波+包络,
   句间静音, `realtime_pacing` 用绝对时刻 `sleep_until` 控制节奏(不要每帧 `sleep(20ms)`,
   误差会累加)。

**为什么先做合成音源**: 没有真麦克风、没有音频素材时, 你必须有一个**可复现**的输入,
否则后面所有测试都不稳定。

**验收**: 读 22.7 秒音频 → 1135 帧 × 320 样本, 且每句的语音起止与脚本误差 < 20ms。
这一条已经固化成测试: `tests/test_audio.cpp` 的 `ScriptedAudioSource_timeline_*`。

> 坑: 合成音频的底噪幅度要标定到 VAD 的初始噪声底, 否则 VAD 会"永远在说话"。
> 本仓库踩过: 0.015 的幅度实际是 -41dBFS, 而 VAD 初始噪声底按 -58dBFS 算, 差 17dB。

---

## 第 3 步:VAD + 端点检测(1~2 天)

**目标**: 从连续音频流里切出"一句话", 并且准确知道**它什么时候说完**。

- `src/vad/energy_vad.cpp`: 每帧算 RMS→dBFS; 噪声底只在**非语音帧**更新(否则人声会把底噪带跑);
  起始要连续 `onset_frames` 帧超门限(抗突发噪声)。
- `src/vad/utterance_detector.cpp`: 维护 pre-roll 环形缓冲(起点前 200ms 一起带上, 防吞首字);
  连续静音超过 `hangover_ms`(320ms) → 判定句末, 回调交出 `Utterance{id, start, end, pcm}`。

**关键设计**: 端点判定**必须放端侧**。云端不知道你什么时候说完, 等云端判句就多一个往返。

**验收**: 6 句合成音频 → 切出 6 句, 每句 `speech_duration` 与脚本误差 < 250ms(实测 ≤10ms)。
见 `tests/test_vad.cpp`。

> 坑: 时间戳必须用**音频域**(第 i 帧 = i*20ms)。全速跑(非实时)时墙钟几乎不动,
> 用 `now_ms()` 会让 hangover 永远到不了, 句子被 max_utterance 硬切。

---

## 第 4 步:ASR 抽象 + 模拟器(1~2 天)

**目标**: 上层只看到"partial 事件流", 换云厂商不改流水线; 同时在没有云账号时也能跑。

`include/sublens/asr.hpp`:
```cpp
struct AsrPartial {
  uint64_t utt_id; std::string text;   // 注意是"整句当前假设", 不是增量
  bool is_final; int64_t speech_end_ms; int64_t emit_ms;
};
class AsrBackend { submit(Utterance); set_callback(Callback); flush(); };
```

`src/asr/scripted_asr_backend.cpp`: 单工作线程; 先 sleep 首字延迟(260ms),
然后每 `word_step_ms` 吐一个逐词增长的 partial, 最后 `is_final`;
`sleep_ms` 要能被 `stop()` 打断, 并保证 `stop()` 返回后不再回调。

**验收**: 首字延迟落在设定值 ± 抖动内; partial 文本严格前缀递增; final 恰好一个。
见 `tests/test_asr.cpp`。

> 坑: 回调里有锁 → 上层在回调里再调 `submit` 会死锁。回调必须在锁外执行。

---

## 第 5 步:翻译抽象 + 离线翻译 + 时延注入(1~2 天)

1. `include/sublens/mt.hpp`: `MtBackend { submit(MtRequest{draft}), set_callback }`。
   **`draft` 字段从第一天就要有**: 后面"草稿/正式双路"全靠它。
2. `src/mt/phrase_table_mt.cpp`: 整句模板表 + 多词最长匹配短语表(离线可用)。
   质量不求高, 只求"整条链路能真跑起来", 而且延迟是**真实 CPU 时间**。
3. `src/mt/latency_injecting_mt.cpp`: 包一层, 注入 `base + jitter` 延迟,
   用来在单机上复现"云侧往返 180ms"。

**验收**: 12 句 demo 全命中模板; 任何前缀都能翻出目标语言、长度单调;
注入器实测往返 ≥ base。见 `tests/test_mt.cpp`。

> 坑: 注入器如果用单线程串行 sleep, 高并发时延迟里会混进排队时间 —— 要么加并发,
> 要么在文档里写清楚这是"单连接模型"(本项目选了后者, 并在 BENCHMARK.md 里注明)。

---

## 第 6 步:稳定前缀对齐(核心算法, 2~3 天)

**目标**: 解决"流式 ASR 的 partial 会反复改字, 字幕跟着抖"。

先写单测, 再写实现。`src/sched/partial_stabilizer.cpp`:

```text
状态: 该句已提交文本 committed_ + 最近 N 个"未提交尾部" tails_
每个 partial:
  tail = full_text 去掉 committed_ 前缀
  候选 = tails_ 全部元素的最长公共前缀        ← LocalAgreement-n
  若 tail 里有标点 → 候选 = 到最后一个标点(含)  ← 语义断句, 强边界
  若 tail 长度 ≥ max_chunk → 强制提交到最后边界  ← 延迟兜底
  若 is_final → 提交全部
  提交长度不足 min_chunk_chars → 先攒着
```

三个必须处理的边界:
- **UTF-8 码点对齐**: 公共前缀按字节算完后要回退到字符边界, 否则会切出半个汉字;
- **ASR 改写已提交区**(新文本不以 committed 为前缀): 保留已提交内容**不回退**, 只警告一次;
- **换句**: `utt_id` 变化时清空状态。

**验收**: 增长序列、标点强制、final 全提交、UTF-8 不切半、改写不回退。见 `tests/test_stabilizer.cpp`。

> 坑: 提交粒度太小(逐字)会让翻译质量崩; 太大则延迟涨。`min_chunk_chars=2` 是个起点。

---

## 第 7 步:调度器:正式/草稿双路 + 差量追加(2~3 天)

`src/sched/translation_scheduler.cpp` 做四件事:

1. **正式路**: 稳定片段 → 翻译。请求文本发**该句累计原文前缀**(带上下文, 别发单个片段,
   否则词序会乱); 结果回来与"已上屏译文"求 LCP, 只追加新增部分。
2. **草稿路**: 未确认尾串 → 草稿翻译, 只更新第二行; 带 150ms 节流, 过时就丢弃。
3. **定稿帧补发**: 最后一个片段的译文往往在 `is_final` 到达前就回来了, 那时还不知道
   "这句说完了", 没法做整行校正。所以定稿时要再补发一次整句翻译。
4. **锁外调用**: `mt_.submit` 和 sink 都在锁外, 否则同步回调的 MT 后端会自己锁死自己。

**关于"句末整行校正"**: 短语级 MT 对"逐段拼出来的前缀"和"整句"给出的译文不是前缀关系,
句末会重排词序。所以 HUD 契约是: 句内只追加, 句末允许**一次**整行替换(`HudUpdate::kReplace`)。
这是产品取舍, 要写进文档 —— 见 `docs/ARCHITECTURE.md` 5.3。

**验收**: 差量追加不重复; 改写不回退; 句末主行 == 完整译文; 草稿被节流; 过时草稿丢弃。
见 `tests/test_scheduler.cpp`。

---

## 第 8 步:流水线编排(2 天)

`src/app/pipeline.cpp`: 4 条线程 + 一条固定的收尾顺序。

```text
采集线程:  audio.read() → ring.push()        (队列满 → 让 1ms 重试, 不丢帧)
VAD 线程:  ring.pop() → detector → asr.submit()
ASR 线程:  后端内部 → 回调 → scheduler.on_asr_partial()
HUD 线程:  固定 fps → hud.render(hud_state.snapshot())

收尾顺序(不要改, 改了会丢最后一句):
  audio EOF → detector.flush() → asr.flush() → mt.flush() → 补 3 帧 → 停 HUD
```

延迟统计在 sink 里做: `speech_end_to_hud = hud_update.at_ms - utt.speech_end_ms`,
**按 utt 去重只记第一次**; 非实时模式不记(口径失真)。

**验收**: 6 句全部检出; 端到端 p95 有数; 无丢帧; 收尾后最后一句译文在屏上。
用 `core` 的 `Pipeline` + 自己的 `HudRenderer` 就能测, 不需要终端。

---

## 第 9 步:HUD 渲染 + 演示程序(1~2 天)

- `src/hud/hud_state.cpp`: 把 `HudUpdate` 折叠成 `HudSnapshot`。硬规则: 主行 append-only
  (除句末校正); 草稿行整行替换 + 最小间隔节流; 指标: `revisions` / `drafts_dropped`。
- `src/hud/ansi_renderer.cpp`: ANSI 转义原地重绘(`\x1b[H` 回家 + `\x1b[K` 清行尾),
  列宽按显示宽度算(中文 2 列), 不能换行错位。
- `src/app/main.cpp`: 参数解析、Windows UTF-8 控制台、Ctrl+C → `request_stop()`、
  收官帧 + 统计表打印。

**验收**: 终端里 HUD 原地刷新不闪; 中文不错位; 跑 24 秒能看到 6 句完整对话。

> 坑: 别用 `_setmode(_O_U8TEXT)`(宽字符模式会和 ANSI 转义打架); 用 UTF-8 字节 +
> `SetConsoleOutputCP(CP_UTF8)` 最稳。

---

## 第 10 步:把"能跑"变成"能证明"(1~2 天)

这一步才是作品和玩具的区别:

1. `bench/bench_ring.cpp`、`bench_vad.cpp`、`bench_stabilizer.cpp`、`bench_hud.cpp`、
   `bench_pipeline.cpp`: 每写一个模块就配一个可复现的基准;
2. 基准输出 Markdown, 直接粘进 `docs/BENCHMARK.md`;
3. 基准里要区分**实测**(CPU/算法/时序)和**注入模型**(云 ASR/MT 时延), 并在文档里标清楚;
4. `tests/`: 67 个用例覆盖边界(UTF-8、改写、过时草稿、并发、堆越界)。

**验收**: 别人 clone 下来, `cmake -B build && cmake --build build && ./build/sublens_bench` 就能复现你的数字。

---

## 附:总时间与难点排序

| 阶段 | 时间(单人) | 难点 |
|---|---|---|
| 0~1 口径 + 底座 | 1.5 天 | 想清楚指标 |
| 2~3 音频 + VAD | 3 天 | 时间戳口径、底噪标定 |
| 4~5 ASR/MT 抽象 | 3 天 | 线程/回调/生命周期 |
| 6 稳定前缀 | 2.5 天 | UTF-8 边界 + 改写处理 |
| 7 调度器 | 3 天 | 差量追加 + 句末校正 + 锁外调用 |
| 8 流水线 | 2 天 | 收尾顺序、反压 |
| 9 HUD | 1.5 天 | ANSI 排版、中文宽度 |
| 10 基准/文档 | 2 天 | 诚实标注口径 |

**合计约 18 个工作日。** 这个数字你自己心里要有数: 客栈上报价"3 周交付端侧引擎"是有依据的,
不是拍脑袋。反过来, 如果客户只给一周, 你要砍的是"HUD 精修 + 云端接入", 不是砍测试。

---

## 最后:怎么讲给别人听(面试/甲方)

一句话版本:

> 我做了一个跑在智能眼镜上的实时字幕翻译引擎。难点不在模型, 而在两件事:
> **延迟怎么压进 700ms**, 和**流式识别的 partial 会改字、字幕怎么才能不闪**。
> 前者我拆成 5 段预算并逐段实测; 后者我用 LocalAgreement-n 稳定前缀 + 双行 HUD +
> 句末一次校正解决, 全部有可复现的基准和单测。

被追问时, 打开这三个文件就够了:

1. `src/sched/partial_stabilizer.cpp` —— 算法核心;
2. `docs/LATENCY-BUDGET.md` —— 延迟怎么拆、怎么测;
3. `bench/bench_pipeline.cpp` + `docs/BENCHMARK.md` —— 数字怎么来的。
