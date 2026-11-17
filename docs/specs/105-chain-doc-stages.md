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
| T2 | `describe_chain`：`build_stages`（内部按段产出 params/provenance/blockers）+`build_chain_doc` 填 `stages` + 顶层 `contract_version` | `test_describe_chain.py` 新用例全绿 |
| T3 | `describe_render`：markdown 渲染 stages 摘要 + fusion 节 | `test_describe_render.py` 新用例全绿 |
| T4 | `stage_fusion.py` CLI：`--pick a:eq --pick b:dynamics --out fused.json` | `test_stage_fusion.py` 全绿；融合文档过 `validate_chain_doc` |
| T5 | 确定性：同输入两次运行字节相同 | 测试断言逐字节相等（无 set/dict 序漂移） |

## 验收

- [x] `.venv/bin/python -m pytest tools/ -q` 全绿（**290 passed / 1 skipped**；基线 230 / 1 未退化）
- [x] `CONTRACT_VERSION == "3"`；v2 文档校验通过；未知版本确定性报错
- [x] 融合产物能通过 `validate_chain_doc`；provenance 保留两个输入来源
- [x] 确定性：字节稳定
- [x] 只测公开行为（不在测试里断言私有函数/调用次数/内部顺序）

## 裁剪

- 无 CI 新增（仓库已有 `.github/workflows/build.yml`，本次为纯 Python 改动，不新增 job）。
- 双轴 review 不裁剪（standard 档）。

## 变更记录

### 变更点 1（实现中修订）：`stages` 允许空列表

- **原验收**：v3 文档的每个 plugin 必须分解出非空 stages。
- **实际**：同一个插件可能**一段都没测到**（freq/compression/gr 拟合全失败、harmonic 也 not-measured）；此时强制非空只能逼出伪造段。
- **改为**：`usable_as_spec === true` 的 plugin 必须 stages 非空；`usable_as_spec === false` 允许 `stages: []`。校验器按此不变式实现，并有专门用例（`upgrade_to_v3(..., stages=[])` 构造）。
- 注：scepter 这类退化插件**仍会产出** `nonlinearity` 段（它有 rig 级 THD 数据，只是 verdict=artifact），所以空列表的真实触发者是「连谐波都没测」的插件，而不是 scepter——初稿措辞已按此更正。

### 变更点 2（实现中澄清）：params 是摘要，不是第二份真相

- stage 的 `params` 只放该段可开发参数摘要 + provenance；完整证据块仍留在 `eq`/`dynamics`/`nonlinearity`。
- 融合产物复制源插件的**完整块**，stages 只承载摘要与 `input` 溯源，避免同一事实在两处漂移。

### 变更点 4（双轴 review 后修订）：blocker 单一真相

- stage 新增 `blockers[]` + `usable`，由 `describe_chain` 从 plugin 的 `why_not_spec` 按段过滤生成；`stage_fusion` 只做并集，**不从 params 重推**。
- 理由：初版融合层自行按三条规则重推，会洗掉 `not-exercised`（#100）与 tau 不可信两类阻塞，反向还会把 #79 已豁免的 saturation conflict 重新变成阻塞。
- 校验器把 `blockers`/`usable` 列为 v3 必填，并新增「每 plugin 每种段唯一」。

### 变更点 5（双轴 review 后修订）：错误契约与渲染判据

- `--out`/`--markdown` 写失败 → 退出 2、不留半成品；融合文档记录**调用处写法**的路径（不把本机绝对布局写进文档）。
- 未选中的段不下任何判据：空 dynamics 块不再写 `release_plausible=False`，渲染也只在确有 `release_ms` 时提示「release implausible」。
- v2 文档若带 `stages`/`fusion` 一律校验（向后兼容=可缺省，≠可塞垃圾）。

### 变更点 3（实现中决定）：`generated_at` 取输入最新戳

- 融合文档的 `generated_at` 用各输入 `generated_at` 的最大值，而不是墙钟时间——否则「同输入 → 字节相同」不成立（确定性优先于「生成时刻」语义，后者由文件 mtime 表达）。

