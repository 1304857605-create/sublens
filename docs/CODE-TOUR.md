# 代码导读(CODE TOUR)

> 这份文档帮你**逐块读懂并动手改**这套代码。三条路线, 按你的时间选:
>
> - **30 分钟速览**: 只读第 1、2、3 节 —— 知道每个文件干什么、核心算法在哪;
> - **3 小时精读**: 加第 4 节的 6 个必读片段, 对着代码看一遍;
> - **1 天动手**: 做第 5 节的 3 个改造练习, 改完跑测试 —— 这才是"会了"。

---

## 1. 先记住这张图

```text
  麦克风/合成音源
        │  20ms 一帧 PCM16
        ▼
   ┌─────────────┐   无锁环形队列(32768 样本)
   │  采集线程    │──────────────────────────┐
   └─────────────┘                          ▼
                                   ┌─────────────────┐
                                   │ VAD 线程         │  ← 判断"说完了没有"
                                   │ EnergyVad        │
                                   │ UtteranceDetector│
                                   └────────┬────────┘
                                            │ Utterance(一句音频 + 起止时间)
                                            ▼
                                   ┌─────────────────┐
                                   │ ASR 后端(可换云) │  ← 一句 → 逐词增长的 partial
                                   └────────┬────────┘
                                            │ AsrPartial(可能反复改字!)
                                            ▼
                                   ┌─────────────────┐
                                   │ 稳定前缀对齐     │  ← 只放行"连续两次一致"的前缀
                                   │ PartialStabilizer│
                                   └────────┬────────┘
                          ┌─────────────────┴─────────────────┐
                          │ 稳定片段 → 正式翻译(带整句上下文) │
                          │ 未确认尾 → 草稿翻译(可丢)         │
                          └─────────────────┬─────────────────┘
                                            ▼
                                   ┌─────────────────┐
                                   │ HUD 状态机       │  主行只增不改; 句末校正一次
                                   │ HudState         │
                                   └────────┬────────┘
                                            ▼
                                   ┌─────────────────┐
                                   │ 渲染线程(20fps)  │  终端 ANSI / 眼镜 GLES
                                   └─────────────────┘
```

**改代码时永远先问: 我改的这一层, 上游给我什么、下游要什么?** 接口都在 `include/sublens/*.hpp`。

---

## 2. 文件清单(按建议阅读顺序)

| # | 文件 | 行数 | 一句话职责 |
|---|---|---|---|
| 1 | `include/sublens/common.hpp` | 142 | 单调时钟、日志、延迟直方图、中文宽度 |
| 2 | `include/sublens/pipeline.hpp` | 93 | **全局契约**: 线程模型图 + 指标口径 |
| 3 | `src/app/main.cpp` | 247 | 入口: 解析参数、组装依赖、跑完打印统计 |
| 4 | `include/sublens/ring_buffer.hpp` | 104 | 无锁 SPSC 队列(生产者/消费者各只写一边) |
| 5 | `include/sublens/script.hpp` + `src/common/scenarios.cpp` | 47+190 | 对话脚本: 同时驱动音频、ASR、参考译文 |
| 6 | `include/sublens/audio_source.hpp` + `src/audio/` | 82+382 | 音频抽象 + 合成音源/静音/WAV |
| 7 | `include/sublens/vad.hpp` + `src/vad/` | 106+193 | 自适应能量 VAD + 端点检测 |
| 8 | `include/sublens/asr.hpp` + `src/asr/` | 104+325 | 流式 ASR 抽象 + 模拟器 + 云端骨架 |
| 9 | `include/sublens/mt.hpp` + `src/mt/` | 90+1134 | 翻译抽象 + 离线短语表 + 时延注入 |
| 10 | `include/sublens/sched.hpp` + `src/sched/` | 151+601 | **★ 核心算法**: 稳定前缀 + 双路调度 |
| 11 | `include/sublens/hud.hpp` + `src/hud/` | 125+506 | HUD 状态机 + ANSI 渲染 |
| 12 | `src/app/pipeline.cpp` | 497 | **★ 编排**: 4 条线程 + 收尾顺序 + 统计 |
| 13 | `bench/` | 602 | 6 组基准(数字怎么来的) |
| 14 | `tests/` | 2319 | 67 个用例(边界怎么保证的) |
| 15 | `android/app/src/main/cpp/engine_jni.cpp` | 218 | 真机适配: AAudio + JNI |

> `src/mt/phrase_table_mt.cpp` 有 962 行, 其中 800 行是词表 —— 看开头 100 行和 `translate_sync`
> 就够了, 别从头读到尾。

---

## 3. 30 分钟速览: 只抓三个点

### ① 延迟从哪来(`pipeline.hpp` 顶部注释)

```text
语音结束 → ①端点 hangover 320ms → ②ASR 首字 260ms → ③对齐 ~0 → ④MT 1~180ms → ⑤HUD ≤50ms
```

每一段代码里都有对应实现, 每一段都有基准测它(见 `docs/BENCHMARK.md`)。

### ② 字幕为什么不闪(`src/sched/partial_stabilizer.cpp`)

流式 ASR 会说"我想去火 → 我想去火车站"。**只把连续 N 次 partial 都相同的前缀送翻译**,
剩下的尾巴先攒着。这就是 LocalAgreement-n, `on_partial()` 里 60 行代码。

### ③ 两条线程怎么交接(`src/app/pipeline.cpp` 的 `run()`)

采集 → 环形队列 → VAD → ASR 回调 → 调度器 → HUD 快照。
热路径上没有互斥锁; 唯一的锁在 HUD 状态机和调度器里(低频)。

---

## 4. 6 个必读片段(对着代码看)

### ① `include/sublens/ring_buffer.hpp` — 无锁队列

看 `push()` / `pop()` 两个函数, 注意三件事:

- `head_` / `tail_` 用 `alignas(64)` 分开, 避免两条 cache line 互相拖慢(伪共享);
- 生产者读 `tail_` 用 `memory_order_acquire`, 写 `head_` 用 `release` —— 这就是无锁队列的全部同步;
- `dropped_` 计数: 写不进去的元素要计出来, 否则"过载"会被静默吞掉。

### ② `src/vad/energy_vad.cpp` — 能量 VAD

看 `process()`, 关键在**噪声底只在非语音帧更新**:
```cpp
if (!in_speech_) noise_db_ = (1 - alpha) * noise_db_ + alpha * frame_db_;
```
否则人一说话, 噪声底就被自己带高, 然后立刻判"没在说话"——这是最常见的新手坑。

### ③ `src/vad/utterance_detector.cpp` — 端点检测

看 `push_frame()`:

- pre-roll 环形缓冲(200ms): 起点前的声音一起带上, 否则"你好"的"你"会被吞;
- hangover(320ms): 连续静音超过它才判句末 —— 这是**端侧引入的唯一固定延迟**;
- 一句话的 `speech_end_ms` 就是延迟统计的起点, 全项目都用它。

### ④ `src/sched/partial_stabilizer.cpp:143` — 核心算法

`on_partial()` 干四件事(代码里有编号注释):

1. 算"未提交尾部" `tail`;
2. 取最近 N 个 tail 的**最长公共前缀** → 可安全提交的部分;
3. 标点处强制切分(语义断句), 尾部过长强制切分(延迟兜底);
4. `is_final` 时全提交。

再看 `longest_common_prefix_utf8()`(第 276 行): 为什么要求码点对齐 —— 按字节切会把
"汉"字切成半个, HUD 直接显示问号。

### ⑤ `src/sched/translation_scheduler.cpp:122 / :233` — 双路调度

- `on_asr_partial`(122 行): 稳定片段 → 正式翻译;**请求文本是整句累计前缀**(不是单个片段,
  否则词序会乱); 尾串 → 草稿翻译(带 150ms 节流); 定稿帧再补发一次整句重译。
- `on_mt_result`(233 行): 结果回来与"已上屏译文"求 LCP, 只追加新增部分; 若是句末且被改写,
  发一次 `kReplace` 让 HUD 整行校正。
- 两个函数都**先算完状态再出锁**, `mt_.submit` 和 sink 在锁外调 —— 否则同步回调的 MT 会死锁。

### ⑥ `src/app/pipeline.cpp:270` — 编排与收尾

`run()` 里最不能改的是收尾顺序:

```text
audio EOF → detector.flush() → asr.flush() → mt.flush() → 补 3 帧 → 停 HUD
```

少一步就会出现"最后一句永远不显示"。再往下看 `on_hud_update()`(501 行):
端到端延迟**按 utt 去重只记第一次**, 这是指标口径的关键。

---

## 5. 三个改造练习(改完跑测试, 就算学会了)

### 练习 1: 给 HUD 加"原文行"(30 分钟, 难度 ★)

**目标**: 画面上显示三行 —— 原文 / 译文 / 草稿。

要改的地方:
1. `include/sublens/hud.hpp`: `HudSnapshot` 已经有 `cur_src` 字段, 不用加; 但渲染没画它;
2. `src/hud/ansi_renderer.cpp`: 在 `render_to_string()` 里加一行显示 `s.cur_src`;
3. `src/hud/hud_state.cpp`: `kCommitted` 分支里 `cur_src` 已经 append 了, 检查有没有被覆盖。

**验收**: `test.cmd` 里 `AnsiRenderer_*` 全绿(宽度不能溢出), 跑 `demo.cmd en2zh --fast`,
能看到三行内容。

> 进阶: 原文行用暗灰色, 字号不变(终端里靠颜色区分)。

### 练习 2: 把 hangover 改成"自适应"(1 小时, 难度 ★★)

**目标**: 说话人语速慢时 hangover 短一点(更快), 句子长时自动延长(不切断)。

要改的地方:
1. `include/sublens/vad.hpp`: `DetectorConfig` 加字段(比如 `min_hangover_ms` / `max_hangover_ms`);
2. `src/vad/utterance_detector.cpp`: 判句末时用 `当前语句时长` 估算 hangover;
3. `tests/test_vad.cpp`: 加一个用例, 长句不被切断。

**验收**: `test.cmd Vad` 全绿; `bench.cmd vad` 的端点误差仍然 ≤ 30ms;
`demo.cmd zh2en` 里那句长中文"如果您时间充裕…"没有被切成两句。

> 提示: 自适应会改变延迟预算, 记得同步更新 `docs/LATENCY-BUDGET.md`。

### 练习 3: 加"翻译风格"开关(1 小时, 难度 ★★)

**目标**: `demo.cmd en2zh --style=formal|casual` 输出不同措辞。

要改的地方:
1. `src/app/main.cpp`: 解析 `--style`;
2. `include/sublens/mt.hpp`: `PhraseMtConfig` 加 `style` 字段;
3. `src/mt/phrase_table_mt.cpp`: 模板表按 style 选目标文本(先给 2~3 句写两版译文即可)。

**验收**: 同一个 demo, 两种 style 输出不同但都通顺; `test.cmd Mt_` 全绿。

> 这个练习最接近真实客户需求("我们想要更口语化的翻译"), 面试时讲得出来。

---

## 6. 调试技巧(排查问题的顺序)

| 想知道 | 怎么做 |
|---|---|
| 某个模块的单测 | `test.cmd Scheduler`(名字子串过滤) |
| 只跑某个基准 | `bench.cmd stabilizer` / `bench.cmd latency` |
| 快速回归(不等 24 秒) | `demo.cmd en2zh --fast`(全速, 不记延迟) |
| 看更详细日志 | `main.cpp` 里 `set_log_level(LogLevel::kDebug)`; 日志走 stderr, 不影响 HUD |
| 单独验证一段逻辑 | 仿照 `out/` 里我集成时写的探针: 链接 `build/libsublens_core.a`, 50 行就能跑 |
| 确认延迟口径没被破坏 | 看 `demo` 输出里 `speech_end_to_hud` 的 `n=` 是否等于句数(每句一个样本) |
| 怀疑丢字 | `test.cmd Stabilizer` 里有"提交内容完整性"用例 |

> 集成时的真实例子: HUD 最后一句总是不完整。用 50 行探针打印所有 `HudUpdate` 后发现
> **定稿帧到达时, 最后一个片段的译文早就回来了**(那时还不知道"这句说完了"), 于是补发了
> 一次定稿重译。这就是 `final_requests` 字段的由来 —— 这类 bug 只能靠日志/探针, 靠猜没用。

---

## 7. "我想换 X" 对照表

| 想换/想加 | 改哪 | 工作量 |
|---|---|---|
| ASR 换成阿里云/讯飞/whisper.cpp | 新写一个 `AsrBackend`(`src/asr/cloud_asr_backend.cpp` 是骨架) | 2~5 天 |
| MT 换成云 API / 端侧小模型 | 新写一个 `MtBackend` | 1~3 天 |
| 麦克风采集(真机) | 实现 `IAudioSource`(`android/.../engine_jni.cpp` 里有 AAudio 参考) | 1~2 天 |
| 显示换 GLES / 厂商 HUD SDK | 实现 `HudRenderer` | 2~4 天 |
| 语言对(中↔日/中↔韩) | `PhraseMtConfig` 的 `source/target_lang` + 词表 | 1 天(词表靠翻译服务生成) |
| 加语种自动检测 | 在 `on_utterance` 前插一个检测器, 切换 MT 后端 | 2~3 天 |
| 加说话人分离 | VAD 之后按声纹聚类, `Utterance` 加 `speaker_id` | 1~2 周 |
| 降延迟(再压 100ms) | 见 `docs/LATENCY-BUDGET.md` 第 4 节(降 hangover / 端侧 MT) | 3~5 天 |

---

## 8. 3 分钟面试/甲方讲法

**第一分钟: 问题与方案**
> 我做了智能眼镜上的实时字幕翻译。难点不在模型, 在延迟和可读性:
> 流式识别的临时结果会反复改字, 字幕跟着抖, 眼镜上比手机更难受。
> 我把延迟拆成五段预算逐段压, 再用"稳定前缀对齐"保证字幕只往前走。

**第二分钟: 数据**
> 本机实测端到端 p50 404ms、p95 497ms, 预算 700ms; 引擎能跑 54 倍实时, 单核 1.8%;
> 换成云端 MT 就超预算, 所以我建议客户把 MT 放端侧 —— 这个取舍文档里有。
> 全部数据 `sublens_bench` 一条命令可复现。

**第三分钟: 边界与我踩过的坑**
> 这套代码里模型是模拟的, 换真模型只需要实现一个后端类, 接口已经留好;
> 真正的坑是"定稿帧到达时最后一个片段的译文早就回来了", 导致最后一句永远不完整 ——
> 我在调度器里加了定稿重译。这类问题只能靠可观测性, 靠猜没用。

**结束前打开这三个文件**: `src/sched/partial_stabilizer.cpp`、`docs/LATENCY-BUDGET.md`、
`docs/BENCHMARK.md` —— 分别对应"算法、取舍、证据"。
