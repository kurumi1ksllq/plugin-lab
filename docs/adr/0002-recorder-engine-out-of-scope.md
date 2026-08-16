# 0002 — RecorderEngine 独立类判定不实现（职责被现有结构吸收）

- **日期**: 2026-08-15
- **状态**: 已接受

## 背景

DESIGN.md §8.2 曾规划三层测量目的分层：SweepRunner（不感知目的）→ MeasurementSession（感知"测什么"：type+source 二维）→ 上层 RecorderEngine（感知"为什么测"）。RecorderEngine 依 plan-phase2-5 P2-13 显式延后，此后**从未落地**。

原记录位于 `.out-of-scope/recorder-engine.md`（2026-08-15 随文档体系维护并入本 ADR 后删除该文件）。

## 决策

不实现独立的 RecorderEngine 编排类。其规划职责已被现有结构吸收：

- **批量编排**（"测什么"）：`dataset` 命令已聚合 4 类型 battery + 可选 `scan` / `compression_family` 块，导出为 `Export::Dataset` 自包含数据包
- **目的语义**（"为什么测"）：`dataset` 固定映射（gr_timeline→dynamic 保证 τ 有效）+ 调用方显式指定 type/source/excitation 承担
- **场景化意图**：本质是 #9 AI 反推闭环的消费端逻辑——"为什么测"由 AI 决定（复刻某插件行为需哪些测量），属于 AI 侧编排而非 C++ 宿主侧

**AnalysisStrategy 前例**：同为 §8.2 设计组件，从未独立成类——分析分流由 MeasurementSession 的 type+source 二维 + CommandParser 显式 source 指定承担。RecorderEngine 走同一路径：设计概念由现有结构吸收，不新增过薄的壳（Ousterhout：删掉实现复杂度也随之消失的模块就是过薄的壳）。

## 备选

- 实现独立 RecorderEngine 类：与 dataset + Export::Dataset 重复；"为什么测"的意图语义在 #9 定型前是猜测性设计。

## 理由

- 现有 `dataset` + `Export::Dataset` 已覆盖"测什么 + 批量编排"，新增独立类必然重复
- "为什么测"与 #9 的 AI 消费端重叠——放 C++ 侧是猜测性设计（#9 未定型前不知道 AI 需要什么场景 API）
- 符合项目明示原则「不做过度设计」

## 若未来需要

仅当 #9 反推闭环定型后暴露出宿主侧确需场景化编排 API（而非 AI 侧脚本编排）时，才考虑重新评估。删除本 ADR 即可重新开放该概念。

## 关联

- #17 —— RecorderEngine 上层需求（issue 保持 open；承接方式 = `dataset` 批量编排 + #9 AI 侧，不新增 C++ 编排类）
- #9 —— AI 反推闭环（"为什么测"的消费端所在）
