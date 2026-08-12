# 0001 — 文档体系:三文档 + 领域文档布局

- **日期**: 2026-08-11
- **状态**: 已接受

## 背景

项目文档曾分散于根文档 + `docs/`(data-schema/roadmap/archive 计划)。用户决策:本地只维护三个文档(DESIGN 设计 / SPEC 工程 / STATUS 状态)+ AGENTS.md(开发约束),需求与待开发全部走 GitHub issue。随后按标准流程(skill 套件)引入领域建模载体。

## 决策

1. 本地文档体系:**DESIGN.md**(设计)/ **SPEC.md**(工程,原 data-schema)/ **STATUS.md**(状态)+ **AGENTS.md**(agent 开发约束)。
2. **CONTEXT.md**(领域词汇表)+ **docs/adr/**(结构化 ADR)与三文档并存,由 `/domain-modeling` 维护——词汇表统一术语,ADR 记录逐条架构决策(决策史主记录仍在 STATUS.md)。
3. **docs/agents/**:skill 套件配置(issue-tracker / triage-labels / domain)。
4. 需求与待开发:**GitHub issue**(triage 标签:needs-triage/needs-info/ready-for-agent/ready-for-human/wontfix)。
5. 历史计划/路线图(已完成)删除,内容在 git 历史与 PR 记录中可追溯。

## 备选

- 严格三文档无 CONTEXT/adr:领域建模失去结构化载体,词汇表挤入 DESIGN.md。
- 全保留 docs/:文档分散,与"需求走 issue"决策矛盾。

## 理由

- 三文档聚焦"当前事实",历史进 git/issue 可追溯,agent 上下文加载轻。
- CONTEXT/adr 是技能流程(grill-with-docs → to-spec → to-tickets)的读写载体,缺则流程无法落地。
