# 从这里开始(START HERE)

> 这份文档回答三个问题: **怎么跑起来 / 代码在哪 / 想改一处该怎么改。**
> 如果你想"从零自己写一遍", 看 [HOW-TO-BUILD-IT.md](HOW-TO-BUILD-IT.md)。

---

## 0. 项目在哪

```text
C:\Users\qiyi\Documents\deepseek-harness\default-workspace\sublens
```

已经编译好了, 直接能跑。**最省事的办法: 打开文件夹, 双击 `demo.cmd`。**

或者在终端里:

```bat
cd /d C:\Users\qiyi\Documents\deepseek-harness\default-workspace\sublens
demo.cmd en2zh
```

> 说明: 本机 PowerShell 的执行策略禁止运行 `.ps1` 脚本(会报 "running scripts is disabled"),
> 所以仓库根目录准备了 **`.cmd` 版本**(双击就能用, 不受该限制):
> `build.cmd` / `demo.cmd` / `test.cmd` / `bench.cmd`。
> 如果你习惯 PowerShell, 也可以先执行一次
> `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` 再用 `.\scripts\*.ps1`。

---

## 1. 五分钟跑通

```bat
cd /d C:\Users\qiyi\Documents\deepseek-harness\default-workspace\sublens

demo.cmd                  :: ① 看它到底长什么样(约 24 秒, 演完 6 句对话)
demo.cmd zh2en            :: ② 反向: 中文说, 眼前出英文字幕
test.cmd                  :: ③ 看测试全绿(67 个用例)
bench.cmd                 :: ④ 看实测数据(约 1 分钟)
```

如果 `build\` 被删了、换电脑了、或者改了代码, 一条命令重建:

```bat
build.cmd
```

> 想用原来的 PowerShell 脚本也可以: `.\scripts\build.ps1 -Test`(需先改执行策略, 见上面说明)。

**运行中你会看到**(终端里原地刷新, 不是一行行滚):

```text
.------------------------------------------------------------------.
| SubLens HUD  EN -> ZH  -  listening                    #5  758ms |
|------------------------------------------------------------------|
|  今天天气很适合散步。            ← 主行: 已定稿译文, 只增不改        |
|  ... 感谢                       ← 草稿行: 还没确认的推测译文(琥珀色) |
|                                                                  |
|  延迟 758ms   FPS 20.0   已定稿 5 句   ← 底栏: 延迟/FPS/句数        |
'------------------------------------------------------------------'
```

跑完会打印一张统计表, 其中 **`speech_end_to_hud (端到端)` 的 p95** 就是核心指标。

---

## 2. 代码地图: 我想看什么, 去哪个文件

| 我想… | 去这里 |
|---|---|
| 看懂整个设计 | [ARCHITECTURE.md](ARCHITECTURE.md)(先看"线程模型"那张图) |
| 看懂 700ms 怎么算出来的 | [LATENCY-BUDGET.md](LATENCY-BUDGET.md) |
| 看最核心的算法(字幕为什么不闪) | `src/sched/partial_stabilizer.cpp` + `include/sublens/sched.hpp` 的注释 |
| 看 4 条线程怎么跑 | `src/app/pipeline.cpp` |
| 看音频怎么采集/切句 | `src/audio/`、`src/vad/` |
| 看识别/翻译怎么接 | `src/asr/`、`src/mt/` |
| 看界面怎么画 | `src/hud/` |
| 看真机(眼镜)怎么接 | `android/README.md` + `android/app/src/main/cpp/engine_jni.cpp` |
| 看实测数字怎么来的 | `bench/` + [BENCHMARK.md](BENCHMARK.md) |
| 看接真实云服务的协议 | [CLOUD-INTEGRATION.md](CLOUD-INTEGRATION.md) |
| 想参与贡献 / 提第一个 PR | [../CONTRIBUTING.md](../CONTRIBUTING.md) |

**入口在 `src/app/main.cpp`**: 从 `main()` 往下读, 它会组装音频源/ASR/MT/HUD, 然后 `pipeline.run()`。

---

## 3. 五个"改一下试试"(每个 1~2 分钟)

### ① 改一句台词

打开 `src/common/scenarios.cpp`, 找到 `build_en2zh()`, 把某句的英文改掉, 例如:

```cpp
s.utterances.push_back(make_utterance(
    6, 20200, 900, "Thank you so much for your help!",
    "非常感谢你的帮助!",
    {{"Thank", 280}, {"you", 180}, {"so", 200}, {"much", 260}, {"for", 160},
     {"your", 180}, {"help!", 340}}));
```

然后重新编译 + 跑:

```bat
build.cmd
demo.cmd en2zh
```

> 注意: 离线翻译器是"整句模板 + 短语表", 你新写的句子如果没有对应模板/词表, 会退化成
> "未命中内容保留原文"。要让它翻译出来, 得去 `src/mt/phrase_table_mt.cpp` 的词表里加词条
> (文档 [CLOUD-INTEGRATION.md](CLOUD-INTEGRATION.md) 说了怎么换成真翻译服务, 那才是正路)。

### ② 关掉草稿行(看"稳定优先"长什么样)

```bat
demo.cmd en2zh --no-draft
```

对比一下: 主行照样长出来, 但第二行没了, "整行替换"指标变成 0。

### ③ 把端点判定调激进(延迟更低, 但可能切碎句子)

打开 `include/sublens/vad.hpp`, 把 `DetectorConfig::hangover_ms` 从 `320` 改成 `240`, 重新编译。
再跑 demo, 看 `speech_end_to_first_partial` 是不是变小了、句子有没有被切成两半。

### ④ 模拟"翻译在云端"(看延迟怎么超预算)

打开 `bench/bench_pipeline.cpp`, 找到 `run_case("B 云端 MT...", script, 180, true)` 里的 `180`,
改成 `80` 或 `300`, 然后:

```bat
cmake --build build
bench.cmd latency
```

你会看到端到端 p95 跟着 MT 时延线性变化 —— 这就是"延迟预算"的直观感受。

### ⑤ 改 HUD 配色/宽度

打开 `src/hud/ansi_renderer.cpp`, 搜颜色转义(`\x1b[`), 或者改 `AnsiHudConfig::width`(默认 68 列)。
重新编译即可。

---

## 4. 常见的"跑不起来"与解法

| 现象 | 原因 / 解法 |
|---|---|
| `.\build\sublens_demo.exe : 无法将...识别为 cmdlet` | 你在 cmd 里用了 `.\`。用 PowerShell, 或改成 `build\sublens_demo.exe` |
| 终端里中文是乱码/方块 | 先执行 `chcp 65001`; 推荐用 Windows Terminal(自带 UTF-8) |
| `... cannot be loaded because running scripts is disabled` | 本机 PowerShell 执行策略限制。用 `.cmd` 版本(`build.cmd`/`demo.cmd`/`test.cmd`/`bench.cmd`), 或先 `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` |
| 编译报 `cmake 不是内部或外部命令` | 没装 CMake 或没加 PATH; `build.cmd` 会调用本机已有的 MinGW 版 cmake |
| 链接报 `cannot open output file ...exe: Permission denied` | 上一次的 demo 还在跑, 或杀毒软件占用; 关掉窗口再编译 |
| 没跑出画面, 只有一行行日志 | 你用的是重定向/管道(比如 `| more`); HUD 需要能原地刷新的终端, 直接运行即可 |
| 想看纯文本帧(方便截图/贴文档) | 加 `--no-color`, 跑完会打印一帧静态画面 |

---

## 5. 想参与这个开源项目, 做三件事就够

1. **跑通并理解**: 按 [GUIDED.md](GUIDED.md) 的 8 讲走一遍, 每讲都有自测题;
2. **提第一个 PR**: 从 [CONTRIBUTING.md](../CONTRIBUTING.md) 的"适合上手的任务"里挑一个
   (补词表、加基准、补文档注释都算);
3. **记录你的改动**: 在 [CHANGELOG.md](../CHANGELOG.md) 的 Unreleased 段落加一行,
   让别人知道你改了什么。

---

## 6. 一句话总结这套代码的分层

```text
main.cpp            ← 组装与入口(你改参数从这里看)
  └ pipeline.cpp    ← 4 条线程的编排(它调用下面所有东西)
      ├ audio/ vad/ ← 音频从哪来、什么时候说完
      ├ asr/        ← 说完的那句话变成文字(可换云端/本地模型)
      ├ sched/      ← ★ 文字怎么变成"不闪的字幕"(本项目核心)
      ├ mt/         ← 文字怎么变成另一种语言(可换云端 API)
      └ hud/        ← 字幕怎么画到眼前
```

**所有跨模块的连接都在 `include/sublens/*.hpp` 里**, 每个头文件开头都有大段中文注释说明
"为什么这么设计"。改动任何一个模块, 只要不破坏接口, 其他模块不用动 —— 这就是它为什么
能"桌面/眼镜/云端"三种形态共用一套代码。

