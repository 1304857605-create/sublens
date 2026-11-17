# 贡献指南(Contributing)

感谢你愿意花时间。这个项目的目标很具体:**把"说完到上屏"压进预算, 并让字幕不闪。**
所以 PR 的判断标准也很具体 —— 有没有让这两件事更好, 或者更可测。

---

## 快速开始

```bash
git clone https://github.com/1304857605-create/sublens.git
cd sublens

# Linux / macOS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/sublens_tests        # 应看到 67 passed

# Windows
build.cmd
test.cmd
```

依赖只有 **C++20 编译器 + CMake ≥ 3.20**。请不要引入第三方依赖(零依赖是项目卖点之一)。

---

## 代码风格

| 项 | 约定 |
|---|---|
| 缩进 | 2 空格, 不用 Tab |
| 编码 | UTF-8, LF 换行(`.cmd` 用 CRLF) |
| 注释 | **中文**, 解释"为什么"而不是复述代码 |
| 命名 | 类型 `PascalCase`, 函数/变量 `snake_case`, 常量 `kPascalCase` |
| 错误处理 | 热路径不抛异常; 失败要可观测(日志或计数器), 不静默降级 |
| 线程 | 热路径不加锁; 跨线程通信优先用有界队列 + 反压 |
| 头文件 | 公共接口在 `include/sublens/`, 改动需同步中文注释 |

---

## 提交 PR 前

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/sublens_tests                 # 必须全绿
./build/sublens_bench latency         # 如果改了延迟相关路径, 附前后对比
```

PR 描述里请写清楚:

1. 解决了什么问题(而不是"改了什么文件");
2. 怎么验证的(命令 + 关键输出);
3. 如果有取舍, 取舍是什么。

---

## 适合上手的任务

不需要先读懂整个引擎:

- **词表/语言对**: `src/mt/phrase_table_mt.cpp` 的短语表按最长匹配工作, 加词条 + 加测试即可;
- **新的 ASR 后端**: 实现 `AsrBackend`(`include/sublens/asr.hpp` 有完整线程契约说明),
  参考 `src/asr/cloud_asr_backend.cpp` 的骨架;
- **新的渲染器**: 实现 `HudRenderer`, 例如 OpenGL ES / 厂商 HUD SDK;
- **基准**: `bench/` 下每个文件对应一个模块, 可以加对照配置;
- **文档**: 任何让你第一次读代码时卡住的地方, 都值得补进 `docs/`.

---

## 不要做的事

- 不要引入新的第三方依赖(包括测试框架);
- 不要在没有基准数据的情况下声称"更快" —— 延迟数字必须有口径、可复现;
- 不要把模拟链路说成真实模型: 项目对边界的表述是**刻意**的(见 README "项目状态与边界")。

---

## 提交信息

参考 Conventional Commits, 中英文都可以:

```text
feat(sched): 支持按标点自适应调整 min_chunk
fix(vad): 修复全速模式下 hangover 不生效
docs: 补充云端接入的 final 超时说明
bench: 增加 4 线程 vs 1 线程的对照
```

---

## 行为准则

参与本项目即表示你同意遵守 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。
