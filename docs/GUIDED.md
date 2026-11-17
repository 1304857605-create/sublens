# 带读计划(8 讲):把这份代码变成你自己的

> 目标不是"看过", 而是**合上代码能讲出来、打开代码能改对**。
> 用法: 一讲一讲来, 每讲先读指定代码, 再回答"自测题", 答不上就回去读那一节。
> 8 讲全部答对 + 做完 3 个改造练习, 这套代码就是你的了。

**建议节奏**: 每天 1 讲(约 1~1.5 小时), 8 天完成; 想快就 2 讲/天。
每讲的结构固定: 读什么 → 必须搞懂的 3 件事 → 自测题 → 动手实验。

---

## 总览

| 讲 | 主题 | 核心文件 | 难度 |
|---|---|---|---|
| 1 | 延迟口径与一次完整数据流 | `include/sublens/pipeline.hpp`、`src/app/pipeline.cpp` | ★ |
| 2 | 音频时间轴与无锁队列 | `ring_buffer.hpp`、`audio_source.hpp`、`scripted_audio_source.cpp` | ★★ |
| 3 | 端点检测:VAD 与 hangover | `vad.hpp`、`energy_vad.cpp`、`utterance_detector.cpp` | ★★ |
| 4 | 流式 ASR 的语义与生命周期 | `asr.hpp`、`scripted_asr_backend.cpp` | ★★ |
| 5 | **稳定前缀对齐(核心算法)** | `sched.hpp`、`partial_stabilizer.cpp` | ★★★ |
| 6 | **双路调度与差量追加(核心算法)** | `translation_scheduler.cpp` | ★★★ |
| 7 | 线程编排、收尾与可观测性 | `pipeline.cpp`(全文) | ★★★ |
| 8 | 换真模型 / 上真机:接口与验收 | `cloud_asr_backend.cpp`、`engine_jni.cpp` | ★★ |

> 顺序为什么这么排: 先建立"时间与数据怎么流动"的直觉(1~4), 再攻两个核心算法(5~6),
> 最后看整体工程与落地(7~8)。跳着读会在第 5 讲卡住。

---

## 第 1 讲:延迟口径与一次完整数据流

**读**: `include/sublens/pipeline.hpp` 顶部注释(线程模型 + 延迟口径)
→ `src/app/pipeline.cpp` 的 `run()`(第 270 行起)、`on_hud_update()`(第 501 行)。

**必须搞懂的 3 件事**

1. **四个时间戳的分工**
   - `speech_start_ms`: 语音起点(含 pre-roll 之前), 端侧产生;
   - `speech_end_ms`: **判定说完的那一刻** —— 整条延迟链的起点, 端侧产生;
   - `emit_ms`/`at_ms`: ASR/MT/HUD 事件产生的墙钟时刻;
   - 端到端延迟 = `at_ms - speech_end_ms`。
2. **为什么按 utt 去重只记第一次**: 一句话会提交十几个片段, 全记的话 p95 变成"整句翻完"的时间。
3. **为什么非实时模式不记延迟**: 全速跑时音频时间被压缩, `speech_end_ms` 与墙钟不是同一把尺子。

**自测题**

1. 如果每个 `kCommitted` 都记进 `speech_end_to_hud`, 而一句话说 3 秒、最后一段译文在说完后 1.2 秒才到,
   那么 p95 会变成多少? 这个数字对"用户体验"有解释力吗?
2. 收尾顺序 `audio EOF → detector.flush() → asr.flush() → mt.flush()` 中, 把 `asr.flush()`
   挪到 `mt.flush()` 后面, 会发生什么? 为什么?
3. 为什么"延迟统计"放在 sink(`on_hud_update`)里, 而不是放在 HUD 渲染线程里?

**动手**: 跑 `demo.cmd en2zh`, 观察输出里 `speech_end_to_hud` 的 `n=` 是否等于句数(6)。
再跑 `demo.cmd en2zh --fast`, 确认这些直方图变成"(无样本)"。

---

## 第 2 讲:音频时间轴与无锁队列

**读**: `include/sublens/ring_buffer.hpp` 全文 → `include/sublens/audio_source.hpp`
→ `src/audio/scripted_audio_source.cpp` 的 `render_sample()` 与 `read()`。

**必须搞懂的 3 件事**

1. **为什么时间戳必须是"音频域"**: 第 i 帧结束 = i*20ms。全速跑时几十毫秒就读完 22 秒音频,
   用 `now_ms()` 喂给端点检测, hangover(320ms)永远到不了。
2. **SPSC 无锁队列怎么做到不阻塞**: 生产者只写 `head_`, 消费者只写 `tail_`,
   各读对方的位置; `release/acquire` 保证可见性; 两条 cache line 分开防伪共享。
3. **缓存行对齐为什么重要**: 两个 atomic 在同一 cache line 上会互相失效, 吞吐掉几倍。

**自测题**

1. 队列满时 `push()` 返回什么? `dropped()` 记的是什么? 为什么"反压重试"比"直接丢"更适合音频?
   (提示: 想想 20ms 音频丢了意味着什么)
2. 合成音源里 `pos` 为什么必须**逐样本**递增, 而不是每帧加一次? 不这样做会看到什么现象?
   (这条是真踩过的 bug: 每句首帧只有底噪 → VAD 抓不到句首)
3. 如果音频源每帧返回的样本数不固定(比如 160), 整条链路哪些地方需要改?

**动手**: 打开 `src/audio/scripted_audio_source.cpp`, 把 `kEnvFloor` 从 0.45 改成 0.05, 重编译跑 demo,
观察 VAD 切句是否还会正常(会变差)。改回来。

---

## 第 3 讲:端点检测

**读**: `include/sublens/vad.hpp`(两个 Config 的注释) → `src/vad/energy_vad.cpp` 全文
→ `src/vad/utterance_detector.cpp` 的 `push_frame()`。

**必须搞懂的 3 件事**

1. **噪声底只在非语音帧更新**: 否则人一说话底噪被自己抬高, 立刻误判"没说话"。
2. **hangover 是端侧唯一固定延迟**(320ms): 越小响应越快, 但句中停顿会被切断。
3. **pre-roll(200ms)**: 起点前的音频一起带上, 否则"你好"的"你"被吞。

**自测题**

1. `onset_frames=2` 解决什么问题? 改成 1 会怎样?
2. 为什么 `speech_duration_ms` 要单独算(不直接用 end-start)? 它和 `speech_end_ms` 的口径差在哪?
3. 如果用户说话前有 2 秒键盘声, 哪几个参数能避免把键盘声当成一句话?

**动手**: 把 `DetectorConfig::hangover_ms` 改成 240, 跑 `bench.cmd vad`, 看端点误差和检出句数;
再改回 320。把两次结果记在笔记里 —— 这就是"延迟 vs 误切"的权衡。

---

## 第 4 讲:流式 ASR 的语义

**读**: `include/sublens/asr.hpp` 全文 → `src/asr/scripted_asr_backend.cpp` 的
`run()`、`process()`、`emit_event()`。

**必须搞懂的 3 件事**

1. **partial 是"整句当前假设", 不是增量**: 所以不能拼起来, 只能用最新一条。
2. **两个生命周期约束**: `submit()` 非阻塞; `stop()` 返回后绝不能再回调(`emit_event` 里检查 `stopping`)。
3. **为什么回调必须在锁外**: 回调里如果再调 `submit()`(比如测试或重入), 持锁就死锁。

**自测题**

1. 首字延迟(260ms)和"定稿延迟"(final)分别影响用户的什么感受?
2. `flush()` 和 `stop()` 的区别是什么? 收尾时为什么用 `flush()` 而不是 `stop()`?
3. 如果 ASR 后端每 50ms 吐一个 partial, 但文本完全没变, 上游会收到什么? 会不会产生重复翻译?
   (提示: 看第 5 讲的 `on_partial` 怎么处理"没有新内容")

**动手**: 把 `ScriptedAsrConfig::inject_errors` 设成 true, 跑 demo, 观察**已上屏的字幕有没有回退**
(答案应该是没有)。改回来。

---

## 第 5 讲:稳定前缀对齐(核心)

**读**: `include/sublens/sched.hpp` 里 `PartialStabilizer` 的注释 →
`src/sched/partial_stabilizer.cpp` 的 `on_partial()`(第 143 行)与
`longest_common_prefix_utf8()`(第 276 行)。

**必须搞懂的 3 件事**

1. **LocalAgreement-n**: 取最近 N 个"未提交尾部"的最长公共前缀 —— 只有稳定下来的字才送翻译。
2. **提交优先级**: `final > 标点 > max_chunk 兜底 > LCP`。标点让字幕按语义断句; max_chunk 防长句拖延迟。
3. **UTF-8 码点对齐**: 按字节切会把汉字切成半个, HUD 显示问号。

**自测题**

1. `confirmations` 从 2 改成 3, 延迟和稳定性分别怎么变? 用 `bench.cmd stabilizer` 的数据回答。
2. 如果 ASR 把已提交的字改掉了(比如"火车站"改成"火车东站"), 当前实现怎么处理? 为什么不能简单覆盖?
3. 为什么"提交长度小于 min_chunk_chars 就先攒着"? 如果改成 1 会怎样(想想逐字翻译的结果)?

**动手**: 打开 `tests/test_stabilizer.cpp`, 找到"改写已提交区"那个用例, 把它改成"期望回退",
跑测试看它变红 —— 体会这条不变式到底在守什么。然后改回来。

---

## 第 6 讲:双路调度与差量追加(核心)

**读**: `include/sublens/sched.hpp` 的 `TranslationScheduler` 注释 →
`src/sched/translation_scheduler.cpp` 的 `on_asr_partial()`(122)、`on_mt_result()`(233)。

**必须搞懂的 3 件事**

1. **为什么请求发"整句累计前缀"而不是片段**: 翻译需要上下文, 片段级翻译会得到"能你 tellme"这种词序。
2. **差量追加**: 结果与"已上屏译文"求 LCP, 只追加新增部分; 已上屏内容永不重复/回退。
3. **句末整行校正(`kReplace`)+ 定稿补发**: 短语级 MT 的整句译法与增量拼接不是前缀关系,
   所以定稿时补发一次整句请求, 让 HUD 做**一次**整行替换。

**自测题**

1. 为什么定稿帧要"补发"一次翻译请求? 不补发会看到什么现象?
   (这条是真 bug: 最后一句永远停在前半句)
2. 草稿路的"过时判定"依据是什么? 为什么宁可不显示草稿, 也不让它在正式行之后到达?
3. `mt_.submit()` 和 sink 为什么必须在锁外调用? 举一个在锁内调用会出事的场景。

**动手**: 跑 `demo.cmd en2zh --no-draft` 和默认模式, 对比"HUD 草稿整行替换"次数;
再看每句最后一行是不是完整译文(而不是半句)。

---

## 第 7 讲:线程编排、收尾与可观测性

**读**: `src/app/pipeline.cpp` 全文(重点 `run()`、`capture_loop()`、`vad_loop()`、`hud_loop()`、
`collect_stats()`)。

**必须搞懂的 3 件事**

1. **反压**: 环形队列满时采集线程让出 1ms 重试, 而不是丢帧(丢 20ms = 吞掉半个字)。
2. **收尾顺序**: 少一步就"最后一句永远不显示"; `flush_grace_ms` 是兜底等待。
3. **统计口径**: 直方图的 `n` 应该等于句数(每句一个样本); 不对就说明口径错了。

**自测题**

1. 为什么 HUD 线程用固定 fps 而不是"有更新就渲染"? 两种做法各自的问题是什么?
2. `capture_loop()` 里如果 `read()` 返回 0(暂时无数据), 应该怎么办? 为什么不能忙等?
3. 如果要把这套引擎放到手机上跑, 哪条线程需要提到最高优先级? 为什么?

**动手**: 在 `on_hud_update()` 里临时加一行 `SL_LOG(Debug)` 打印 `u.kind` 和 `u.utt_id`,
跑 `demo.cmd en2zh --fast`, 观察 kCommitted/kDraft/kReplace 的分布。看完删掉。

---

## 第 8 讲:换真模型 / 上真机

**读**: `src/asr/cloud_asr_backend.cpp`(骨架) → `docs/CLOUD-INTEGRATION.md`
→ `android/app/src/main/cpp/engine_jni.cpp`(AAudio 与 JNI)。

**必须搞懂的 3 件事**

1. 接云 ASR 要处理: 鉴权、重连、partial 语义、final 超时、并发句的 `utt_id`。
2. 接云 MT 要处理: 草稿请求的合并/去抖、超时降级、成本控制。
3. 真机适配只有两个切口: `IAudioSource`(AAudio)和 `HudRenderer`(GLES/厂商 SDK), 引擎一行不用改。

**自测题**

1. 为什么 `CloudAsrBackend` 没配 endpoint 时 `start()` 要返回 false, 而不是"静默降级成模拟器"?
2. 端侧小模型(whisper.cpp)相比云端, 延迟和质量的取舍在哪? 什么场景必须端侧?
3. 眼镜端最大的工程风险是什么(提示: 不是延迟)?

**动手**: 按 `docs/CLOUD-INTEGRATION.md` 画一张"真实链路"的时序图(纸笔即可),
标出每个可能的超时点。这是接单时给甲方看的图。

---

## 掌握度自检(能不看代码回答这 10 条, 就算会了)

1. 端到端延迟从哪个时间点算到哪个时间点? 为什么这么定义?
2. 4 条线程分别是什么、各自的阻塞点在哪?
3. 队列满时的反压策略是什么? 为什么不丢帧?
4. VAD 的噪声底为什么只在非语音帧更新?
5. hangover 为什么是 320ms? 调小会怎样?
6. LocalAgreement-n 的"n"是什么? 提交优先级有哪四级?
7. 为什么正式翻译请求发"整句前缀"而不是片段?
8. 句末为什么需要一次整行校正? `kReplace` 与 `kCommitted` 的区别?
9. 收尾顺序是什么? 少哪一步会出什么问题?
10. 怎么证明你的延迟数字是真的?(口径、去重、非实时模式)

> 答不上来的编号, 就是你需要重读的那一讲。

---

## 三天速成版(如果时间很紧)

| 天 | 任务 |
|---|---|
| 第 1 天 | 第 1、2、3 讲 + 跑一遍 `test.cmd` 和 `bench.cmd` |
| 第 2 天 | 第 4、5、6 讲 + 做完练习 1(HUD 加原文行) |
| 第 3 天 | 第 7、8 讲 + 练习 2 或 3 + 把"掌握度自检"10 条讲给一个外行听 |
