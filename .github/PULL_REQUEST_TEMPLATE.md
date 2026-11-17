## 这个 PR 做了什么

<!-- 一句话说明改动目的 -->

## 类型

- [ ] 修复 bug
- [ ] 新功能
- [ ] 性能优化
- [ ] 文档 / 注释
- [ ] 重构(不改变行为)

## 自查清单

- [ ] `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build` 通过
- [ ] `./build/sublens_tests` 全绿(67 个用例)
- [ ] 如果改了延迟相关路径, 附上 `bench.cmd latency` 的前后对比
- [ ] 新增/修改的代码带中文注释, 说明"为什么"而不只是"做了什么"
- [ ] 没有引入第三方依赖(本项目的卖点之一是零依赖)

## 相关 issue

<!-- 例如: Closes #12 -->
