# #105 — 处理段解耦/融合：chain_doc stages（T7）

grade: standard
分支：`issue-105-chain-doc-stages`
父 issue：#105（enhancement / ready-for-agent）；上游 #67、下游 #106（T8）

## 目标

让 chain_doc（反推出的处理链文档）把每个插件拆成**可单独取用的处理段**（eq / dynamics / nonlinearity），每段带 provenance（这段结论来自哪次测量、哪条证据）；并提供一个 CLI，能把「A 插件的 EQ 段 + B 插件的压缩段」融成一份新 chain_doc。这是 #106「复刻闭环验证」的前置契约。

## 边界

- 只动 `tools/`（Python，stdlib only）：`describe_schema.py`、`describe_chain.py`、`describe_render.py`、新增 `stage_fusion.py` 与测试。
- **不动 C++ 侧**、不动 `SPEC.md` 的导出 schema（那是 #102/#104 的地盘）、不改 `aggregate_report.py` 的容差与判定。
- 不引入第三方依赖；不自动生成 VST3（与项目「不做自动复刻」边界一致）。
- 处理顺序仍然**永不从测量断言**（沿用 `infer_order` 的 unknown + canonical 建议）。

## 契约变更（v2 → v3）

1. `CONTRACT_VERSION = "3"`。
2. chain_doc 顶层新增 `contract_version: "3"`；**缺失视作 legacy v2**（向后兼容，仍按 v2 规则校验）。
3. 每 plugin 新增 `stages: [{kind, params, provenance}]`，`kind ∈ {eq, dynamics, nonlinearity}`；`params` 为该段的可开发参数摘要；`provenance: {source, basis[]}` 记录来源测量与证据。
4. 顶层可选 `fusion: {inputs:[{alias, path, slug, plugin}], picks:[{alias, stage}]}`（只有 stage_fusion 产物才有）。
5. 未知 `contract_version` → 确定性错误字符串（`doc: contract_version '4' unsupported (known: 2, 3)`），不抛异常。

## Tickets（红-绿循环）

| id | 内容 | 验收 |
| --- | --- | --- |
| T1 | schema v3：`contract_version` + `stages` 校验 + v2 兼容 + 未知版本确定性错误 | `test_describe_schema.py` 新用例全绿，旧用例不退化 |
| T2 | `describe_chain`：`build_*_stage` 产出段 + provenance，`build_chain_doc` 填 `stages` + 顶层 `contract_version` | `test_describe_chain.py` 新用例全绿 |
| T3 | `describe_render`：markdown 渲染 stages 摘要 + fusion 节 | `test_describe_render.py` 新用例全绿 |
| T4 | `stage_fusion.py` CLI：`--pick a:eq --pick b:dynamics --out fused.json` | `test_stage_fusion.py` 全绿；融合文档过 `validate_chain_doc` |
| T5 | 确定性：同输入两次运行字节相同 | 测试断言逐字节相等（无 set/dict 序漂移） |

## 验收

- [ ] `.venv/bin/python -m pytest tools/ -q` 全绿（基线 230 passed / 1 skipped 不得退化）
- [ ] `CONTRACT_VERSION == "3"`；v2 文档校验通过；未知版本确定性报错
- [ ] 融合产物能通过 `validate_chain_doc`；provenance 保留两个输入来源
- [ ] 确定性：字节稳定
- [ ] 只测公开行为（不在测试里断言私有函数/调用次数/内部顺序）

## 裁剪

- 无 CI 新增（仓库已有 `.github/workflows/build.yml`，本次为纯 Python 改动，不新增 job）。
- 双轴 review 不裁剪（standard 档）。

## 变更记录

### 变更点 1（实现中修订）：`stages` 允许空列表

- **原验收**：v3 文档的每个 plugin 必须分解出非空 stages。
- **实际**：测量全失败的退化插件（如 scepter：freq/compression/gr 拟合全失败）确实没有任何可用段；强制非空会逼出伪造段，或让合法文档校验失败。
- **改为**：`usable_as_spec === true` 的 plugin 必须 stages 非空；`usable_as_spec === false` 允许 `stages: []`（诚实表达「没有可用段」）。校验器按此不变式实现，并有专门用例。

### 变更点 2（实现中澄清）：params 是摘要，不是第二份真相

- stage 的 `params` 只放该段可开发参数摘要 + provenance；完整证据块仍留在 `eq`/`dynamics`/`nonlinearity`。
- 融合产物复制源插件的**完整块**，stages 只承载摘要与 `input` 溯源，避免同一事实在两处漂移。

### 变更点 3（实现中决定）：`generated_at` 取输入最新戳

- 融合文档的 `generated_at` 用各输入 `generated_at` 的最大值，而不是墙钟时间——否则「同输入 → 字节相同」不成立（确定性优先于「生成时刻」语义，后者由文件 mtime 表达）。

