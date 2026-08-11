## 变更内容 (What)

<!-- 一句话说清这个 PR 改了什么。参考提交风格：feat(ipc): ... / fix(host): ... -->

## 动机 (Why)

<!-- 为什么需要这个改动？对应哪个 spec / ticket / plan-block？ -->

## 变更点 (How)

<!--
要点式列出具体改动与涉及文件。遵循项目铁律：
- 生产路径不用 C++ 异常；每个 catch 必须 CRASH_LOG
- 不改 SweepRunner（冻结边界）；新增 .cpp 必须同时进根 + tests 的 CMakeLists
- /WX 下零编译警告
-->

## 验证 (Verification)

<!-- 必须在跑过之后勾选（证据优先于断言） -->
- [ ] `cmake --build build --config Release` 通过（/W4 /WX 零警告）
- [ ] `ctest --test-dir build -C Release --timeout 180` 连跑 2 次全绿
- [ ] 手工/脚本验证涉及路径（tools/ 脚本、IPC 客户端等）

## 关联

- Blocking: <!-- 被此 PR 阻塞的 ticket / 依赖此 PR 的 plan -->
- 测试计数变化：<!-- 265 → N，若涉及 -->
