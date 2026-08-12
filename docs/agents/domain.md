# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`CONTEXT.md`** at the repo root — the domain glossary (术语词典): what each recurring term precisely means in this project.
- **`docs/adr/`** — read ADRs that touch the area you're about to work in.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs`) creates them lazily when terms or decisions actually get resolved.

## File structure

Single-context repo:

```
/
├── CONTEXT.md          # 领域词汇表
├── docs/adr/           # 架构决策记录 (ADR)
├── DESIGN.md           # 设计文档
├── SPEC.md             # 工程文档（8 类导出 JSON schema + IPC 协议契约）
├── STATUS.md           # 状态文档（决策史 + 阶段记录 + 已知限制）
└── AGENTS.md           # 开发约束（agent 指令）
```

> 与本地「三文档体系」(DESIGN/SPEC/STATUS + AGENTS.md 约束)并存：CONTEXT.md 与 docs/adr/ 为领域建模/决策载体，由 `/domain-modeling` 维护。决策史主记录仍在 STATUS.md；docs/adr/ 用于结构化的逐条 ADR。
