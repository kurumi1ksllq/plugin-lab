# Spec: README 初始化（分支 docs/readme-init）

grade: standard（1 个内容文件新增 + 2 个流程文件；README 预计 >30 行）

## 目标

新增仓库根 `README.md`，让仓库首页可读懂：项目是什么、能做什么、怎么构建 / 运行 / 测试、文档在哪。内容全部取自现有文档（AGENTS / DESIGN / SPEC / STATUS / CONTEXT），不发明新事实。

## 边界

- 只新增 `README.md` 与流程文件（本 spec + `.flow` 状态）；**不改 `AGENTS.md`**（用户已确认保留不动）
- 不新增 LICENSE / CONTRIBUTING（仓库无对应事实来源，不虚构）

## 验收

- `README.md` 存在于仓库根，中文内容 U+FFFD 计数为 0
- 构建 / 测试命令与 `AGENTS.md` COMMANDS 节一致；IPC 命令名与 `DESIGN.md`「实际协议」注记一致
- 文档导航引用的文件全部真实存在（写入前逐项 existsSync 核验）

## 裁剪

- **跳过 TDD**：依据「纯文档型改动无代码行为可测」（FLOW TDD 节 trivial 免测试同理）；仓库无 pnpm/package.json，四件套中 test / verify / lint / typecheck 本机均不适用，且本机无 cmake（Windows/MSVC 项目在 Linux 环境），构建验证落在 CI
- **gh 未认证**：无法建 issue / PR → 分支名走仓库惯例 `docs/*`（FLOW 硬规则的 `issue-<n>-*` 因缺 issue 号降级），双轴 review 以本地 diff `git diff main...docs/readme-init` 为材料执行

## 变更点

（无）
