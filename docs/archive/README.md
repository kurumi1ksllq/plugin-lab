# docs/archive — 已完成计划归档

已完成实施计划的历史文档。内容为决策过程记录，**当前状态以 STATUS.md / roadmap-next.md / AGENTS.md 知识库为准**，本目录仅备查。

| 文档 | 对应工作 | 完成证据 |
|------|----------|----------|
| `plan-phase2-5.md` | 阶段 2-5（信号增强/参数扫描/动态压缩/建模） | DESIGN.md §9.3 全部勾选；STATUS.md「阶段 2-5 完成记录」 |
| `plan-scan-optimization.md` | 扫描优化（缓存/线程/看门狗/黑名单） | STATUS.md「扫描优化专项」10 项决策记录（155/155 绿） |
| `plan-batch-pipeline.md` | 块 A 批量采集管线（dataset IPC + Python 管线） | roadmap 块 2 ✅；真机验收 S1/S4 通过 |
| `plan-block-c-stability.md` | 块 C 稳定加固（异常保护/黑名单/schema） | roadmap 块 0 ✅；186 测试全绿 + 双轴审查 |
| `plan-block-e-measurement-quality.md` | 块 E 测量质量（MLS 频响 / MultiTone 相位 / runMultiple 验证） | roadmap 块 1 ✅；199 测试双跑绿 + 真机 MLS vs sweep \|Δ\|<0.5dB + 双轴审查 |

活跃计划（未完成）在 docs/ 根：`roadmap-next.md` / `plan-block-b-recording.md`（块 3 B，当前块，确认即开工）/ `plan-block-d-out-of-process.md`（块 4 D 设计门）。
