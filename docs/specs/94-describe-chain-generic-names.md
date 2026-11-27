# #94 — describe_chain 泛化参数名误判（Gain/Power）

grade: standard
分支：`fix/issue-94-describe-chain-misjudge`
父 issue：#94（bug / needs-triage）；上游 #80（键表匹配）、#73（dynamics not exercised）、#100（直通检测）

## 目标

`classify_plugin_type` 只靠参数名子串判类型，泛化词横跨多类型：

1. **Ozone 12 Equalizer** 的 `EQ: Stereo/Main Gain N` 命中 dynamics 键表 → `dynamics-only (high)`；
2. **Gem EQP** 的 `Power`/`EQP Power`/`HPF Power` 命中非线性键表 → `saturation (low)`，而其 freq 测量有真实 EQ 曲线。

修完后：泛化名不再单独定性；名字只剩泛化词时**弃权**（unknown + low），由**测量数据**定性。

## 复现（修前，HEAD 版 describe_chain.py）

```
$ python3 -c "import sys; sys.path.insert(0,'/tmp/before94'); sys.path.insert(0,'tools'); \
  import describe_chain_before as dc; from test_data.chain_fixtures import *; ..."
Ozone 12 Equalizer -> dynamics-only high
   - dynamics keys present: EQ: Main Gain 1, ... EQ: Stereo Gain 8
Gem EQP -> saturation low
   - nonlinear/saturation keys present: EQP Power, HPF Power, LPF Power, Power
```

修后同一输入：

```
Ozone 12 Equalizer -> unknown low
   - generic keys only (16): EQ: Main Gain 1, ... EQ: Stereo Gain 8
   - generic Gain/Power names are not family evidence (issue #94)
   - plugin not exercised under measurement (passthrough)
Gem EQP -> eq-only low
   - generic keys only (4): EQP Power, HPF Power, LPF Power, Power
   - generic Gain/Power names are not family evidence (issue #94)
   - measured frequency response fits a plausible EQ peak (100 Hz, 8.75 dB)
```

## 方向选择：为什么选 (b) 而不是 (a)

- **(b) 泛化名降权 → 弃权，再由测量定性**：`Gain`/`Power` 从"家族证据"表里摘掉（保留 `Threshold/Ratio/Attack/Release/Makeup` 与 `Drive/Character/Machine/Tape/Saturation/Param`），命中泛化名时 unknown + low，随后用 row 的 freq 段（status ok 且 `describe_quality.classify_freq_peak` 判 plausible）定性 eq-only。
- **(a) 直接拿测量重判类型**会与 #78 的既定语义冲突：非 EQ 插件（压缩/饱和）的单峰拟合经常"plausible"却无意义，若允许测量盖过参数面，会把这些插件判成 EQ。弃权 + 测量补位把测量只用在**参数面不可辨识**的场景，改动面最小。
- 弃权而非"低置信猜一个"，符合项目"诚实描述"原则：宁可说不知道，也不贴错标签。

## 边界

- 只动 `tools/`（stdlib only）：`describe_chain.py`、`test_describe_chain.py`、`test_data/chain_fixtures.py`；不引入依赖。
- 不动 `aggregate_report.py`（直通检测 #100 已在数据侧定性）、不动契约版本（chain_doc v3 字段不变，kind 取值仍在 `KIND_VOCAB` 内）。
- 不动 C++ 侧。

## Tickets（红-绿）

| id | 内容 | 验收 |
| --- | --- | --- |
| T1 | 复现 + 定位（fixture 化 Ozone/Gem 参数面与 row） | 修前输出留证：dynamics-only high / saturation low |
| T2 | 红：新增 7 个用例（泛化名不判 dynamics-only / 不判 saturation / Gem 由 freq 判 eq-only / 不可信拟合不补位 / Ozone 直通 not-exercised / 两条 chain_doc 端到端） | 先红：6 failed |
| T3 | 绿：泛化名降权 + 弃权 + 测量补位 | 全绿；`pytest tools/ -q` 298 passed |
| T4 | 回归：#73/#78/#79/#80/#86 既有用例不退化 | 基线 290 passed → 298 passed，0 failed |
| T5 | PR + CI 绿 | PR 描述含复现/理由/测试证据 |

## 验收

- [x] Ozone 12 Equalizer 不再判 dynamics-only（unknown + low，basis 记"not exercised"），chain_doc `usable_as_spec=False`（#100 的 not-exercised 拦截）
- [x] Gem EQP 判回 eq-only（low，basis 记测量曲线 100 Hz / 8.75 dB），chain_doc eq overall clean、usable_as_spec=True
- [x] 回归：`PYTHONPATH=... python3 -m pytest tools/ -q --ignore=tools/gui_test.py` → **298 passed**
- [x] 直通行（#100 `not-exercised`）优先于测量补位：没有判别力的数据也不许拿来定性
- [x] 只测公开行为（`classify_plugin_type` / `build_chain_doc`），不测私有函数与调用次数

## 裁剪

- 无 CI 新增（纯 Python 改动，沿用既有 job）。
- 不新增"低置信 + 测量"的组合 kind：kind 词汇表不动，避免下游 describe_render/schema 连锁改动。

## 变更记录

- 2026-11-27：开工，T1 复现完成（HEAD 版输出留证），T2 红（6 failed）。
- 2026-11-27：T3 绿——`_DYNAMICS_SUBSTRINGS` 去掉 `Gain`、`_NONLINEAR_STRONG` 拆出（`Power` 单独不算证据）、新增 `_GENERIC_SUBSTRINGS` / `_dynamics_not_exercised` / `_measured_eq_fit`；`classify_plugin_type` 末尾加弃权 + 测量补位分支。`pytest tools/` 297 passed。
- 2026-11-27：自查补强——直通行（`not-exercised`）必须先于测量补位返回（否则直通数据里的"看似 plausible"拟合会反被判 eq-only，与 #100 冲突）；补 1 个用例，298 passed。
