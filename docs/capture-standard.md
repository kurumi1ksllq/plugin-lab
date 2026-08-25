# PluginLab 采集标准（Capture Standard）

> 更新: 2026-08-25 · Issue: #97（块 A T1）· 关联: DESIGN.md §三 测量策略、SPEC.md 8 类 schema
>
> **目的**：定义每类插件"采什么、扫多少档、什么电平、什么密度、怎么算成功"的规范，让采集从临场拍脑袋变成可复制的标准流程。本文件是采集 SOP 的权威来源（`batch_collect.py` 的 config 书写依据）。

---

## 0. 采集总则（适用于所有插件）

1. **独立实例**：每插件必须独立 app 实例采集（`batch_collect --launch --quit`），禁同实例连续加载依赖 getParams 的流程（#54 串扰实锤）
2. **非默认态**：采集前必须探参并设置非默认参数（默认态可能直通/退化，无判别力）
3. **探参先行**：任何插件采集前，先跑 `tools/probe_plugin.py <plugin>`（T2 #99）确认参数面真实性 + 识别可设参数
4. **假面剔除**：host 不暴露真实参数面（通用 63 参数 / param_id 空或重复 / 测得字节相同 dataset）→ **不可采集**，记录剔除原因
5. **已知不可测**：参数面真实但 headless 处理不生效（直通模式）→ 标记 not-exercised（T4 #100），保留记录不进入 usable
6. **每类验收**：反推结果须达到该类模板的验收标准（见各节），否则视为该插件测量失败

### 采集标准流程（五步）

```
探参（probe_plugin.py）→ 写 configs（tools/configs/<slug>.json）→ 采集（batch_collect --launch --quit）
→ 验证（aggregate_report + describe_chain + 复现性检查 T3）→ 入库（configs + 报告）
```

### 档位密度规则（连续旋钮）

参数值 0..1 归一化。扫描档位密度参考：

| 参数行为 | 档位策略 | 示例 |
|---|---|---|
| 线性连续（增益/频点） | 3-5 档覆盖工作区间 | Gain: [0.5, 0.6, 0.7] |
| 非线性强（drive/ratio） | 5-7 档，低密度向高密度过渡 | Drive: [0.3, 0.5, 0.7, 0.85, 1.0] |
| 阈值型（threshold） | 覆盖"未触发 → 触发 → 深压缩" | Threshold: [0.2, 0.4, 0.6, 0.8] |
| 旋钮连续多档（1.0-10.0 的 1.1/1.2） | **不逐档全扫**——按 3-5 档代表值 + 反推插值 | Ratio: [0.3, 0.5, 0.7] |

> 原则：**扫描是抽样不是枚举**。连续旋钮的无数档位无法全采，采 3-5 档代表值 + 反推插值即可表征处理曲线（DESIGN 3.1 黑盒原则）。步进旋钮（明确离散档）可按档位全采。

---

## 1. 话放 / preamp（前置放大器）

**类别特征**：模拟麦克风/设备音色，同时包含频段 + 失真 + 饱和 + 压缩的混合处理。如 UADx 610-A/B、Neve 类通道条。

### 标准采集模板

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| frequency_response | ✅ 必测 | sweep 或 MLS，默认电平 0.5 |
| harmonic | ✅ 必测 | 单音多电平（见下） |
| compression | ✅ 必测 | ToneBurst 9 电平默认 |
| gr_timeline | 可选 | 若 compression 显示动态行为则测 |

### 参数扫描建议

- 扫 **Input/Gain/Drive**（话放核心——驱动电平决定饱和/失真量）
- 档位：5-7 档 [0.3, 0.5, 0.7, 0.85, 1.0]，重点覆盖高驱动区（饱和激活）
- 若有 Tone/阻抗选择开关：各档位扫一组（如 610-A/B 的 50k/200k 阻抗）

### 多电平配置（T5 落地后）

话放是"电平 → 非线性"最强的类别，必须多电平扫描（-20/-10/0dB）观察：
- 低电平：接近线性（频响为主）
- 高电平：饱和 + 压缩出现

### 验收标准

- freq：测得频段特征（若有 EQ 部分）
- harmonic：高驱动档 THD > 1%（饱和被激活的证据）
- compression：高电平压缩比 > 1.2（若有压缩段）
- **混合判定**：同时看到频响 + 谐波 + 压缩 → 话放特征确认

---

## 2. EQ（均衡器）

**类别特征**：线性频响处理，频点/Q/增益。如 Pro-Q 4、Ozone 12 Equalizer、Gem EQP。

### 标准采集模板

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| frequency_response | ✅ 必测 | sweep（Farina 反卷积）或 MLS，默认电平 |
| harmonic | ✅ 必测 | 确认无谐波（EQ 应 THD≈0） |
| compression | 可选 | 确认无动态（EQ 应 unity） |
| gr_timeline | 否 | EQ 无动态 |

### 参数扫描建议

- 扫 **band 的 Gain**（提升/衰减量）
- 档位：3 档 [0.55, 0.6, 0.65]（围绕反推目标）
- 每个 band 需**单独开启**（Enable=1）再扫（单峰 EQ 模型一次测一个 band，DESIGN 已知限制）

### 多电平配置

EQ 是线性处理——**多电平非必需**（多电平结果应一致，可作为线性验证）。

### 验收标准

- freq：反推频点 + 增益 + Q 与设定值一致（误差 < 2% 频点 / < 0.5dB 增益，对齐 #82 规格样板）
- harmonic：THD ≈ 0（< 0.1%）——若 THD 高，说明是"谐波饱和 EQ"类混合，转 §6 处理
- compression：unity（ratio ≈ 1）

### 已知边界

- **headless 直通**：部分 EQ（Ozone 12 Equalizer 实测）headless 下处理不生效 → not-exercised（#100），不判 usable
- **单峰模型**：一次只能反推一个 band（每 band 一次测量）

---

## 3. 压缩器（Compressor）

**类别特征**：动态增益控制，threshold/ratio/knee/attack/release。如 Pro-C 3、Gem Comp76、Gem Comp LA、Ozone 12 Vintage Compressor。

### 标准采集模板

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| frequency_response | ✅ 必测 | 确认是否有频率依赖（现代压缩器多带侧链滤波） |
| harmonic | ✅ 必测 | 确认压缩是否产生谐波（opto/vari-mu 类会） |
| compression | ✅ 必测 | ToneBurst 9 电平（输入-输出曲线） |
| gr_timeline | ✅ 必测 | 动态信号 + GR 时间线 → attack/release τ |

### 参数扫描建议

- 扫 **Ratio**（压缩比）或 **Threshold**（触发深度）
- 档位：5 档 [0.3, 0.5, 0.7, 0.85, 1.0] 覆盖"浅压缩 → 深压缩"
- 扫 **Attack/Release** 观察 τ 变化（GR 时间线扫描）

### 多电平配置（T5 落地后）

压缩是非线性（电平依赖）——必须多电平：
- 低电平（低于 threshold）：unity（未触发）
- 高电平（高于 threshold）：压缩出现
- 多电平曲线 → 完整 threshold/ratio 拟合

### 验收标准

- compression：反推 threshold + ratio 与设定一致（threshold ±1dB / ratio ±25%，对齐 LOCKED_TOLERANCES）
- gr_timeline：attack/release τ 合理（valid 标志）
- **类型判定**：compressor (high) 且 usable=True（对齐 Pro-C 3 / Vintage Compressor 实测）

### 已知边界

- 1176 类（无 threshold 旋钮，只有 Input/Ratio）：compression 曲线仍可测（ratio 13.6:1 实测），但参数名无 "Threshold" → describe_chain 判 dynamics-only 而非 compressor（#94 泛化名误判）
- opto/vari-mu 类：attack/release 随输入电平变化 → 需多电平 GR 时间线（T5）

---

## 4. 限制器（Limiter）

**类别特征**：极端压缩比（>10:1）的峰值控制，如 Pro-L 2、Ozone 12 Maximizer。

### 标准采集模板

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| frequency_response | ✅ 必测 | 确认无 EQ（限制器应频响平直） |
| harmonic | ✅ 必测 | 确认削波谐波（限制器本质是软削波） |
| compression | ✅ 必测 | 高压缩比曲线 |
| gr_timeline | ✅ 必测 | GR 时间线（限制器 attack 通常 <1ms） |

### 参数扫描建议

- 扫 **Input Gain / Ceiling**（触发深度）
- 档位：3-5 档 [0.6, 0.8, 1.0]（限制器触发阈值通常在高电平区）

### 验收标准

- compression：ratio > 10（限制器特征）
- freq：平直（无 EQ 峰）——Pro-L 2 实测 freq 有假峰（rd 单峰模型不适用），标记 derivation-failed 属已知
- harmonic：THD 随电平上升（削波激活）

### 已知边界

- **rd 单峰模型不适用**：无 EQ 峰限制器（Pro-L 2）reverse_derive 退出码 1——compression 反推仍可用，freq 反推标 derivation-failed（已知）

---

## 5. clip 削波器（Clipper）

**类别特征**：静态非线性传输函数（waveshaper），y=f(x) 不依赖时间。硬削波切平、软削波弯曲。如 KClip、StandardClip、Pulsar Mu 的 clip 段。

### 本质洞察

**clip 削波器 = compression 曲线（传输函数）+ 多电平 THD**——不需要新测量类型（T5 多电平落地即覆盖）。

### 标准采集模板

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| frequency_response | ✅ 必测 | 小信号（未触发削波）确认线性区频响 |
| harmonic | ✅ 必测 | **多电平 THD**（阈值后骤增特征） |
| compression | ✅ 必测 | ToneBurst 9 电平 → **这就是传输曲线** |
| gr_timeline | 否 | clip 无时间依赖（静态） |

### 参数扫描建议

- 扫 **Drive / Input / Ceiling**（触发阈值）
- 档位：5-7 档 [0.3, 0.5, 0.7, 0.85, 1.0]，重点覆盖阈值附近

### 多电平配置（**关键**，T5 落地后必测）

- **THD vs 输入电平曲线**是区分削波类型的关键：
  - 硬削波：阈值前 THD≈0%，阈值后**骤增**到 30%+（高次谐波）
  - 软削波：低电平就开始**平滑渐增**（低次谐波为主）

### 验收标准

- compression 传输曲线：阈值后输出封顶（硬）或平滑弯曲（软）→ 反推削波类型
- harmonic：多电平 THD 曲线形态与削波类型匹配
- freq：小信号线性区平直（若非线性区有频响变化 → 混合类，转 §6）

---

## 6. tape 磁带 / 饱和器（Saturation，含谐波饱和）

**类别特征**：软削波饱和（Saturn 2、Vintage Tape）+ 谐波饱和/exciter（Ozone Exciter、Aphex Aural Exciter）。用户术语：失真类 = "饱和失真"，谐波生成类 = "谐波饱和"。

### 概念区分（测量角度）

| 子类 | 谐波特征 | 电平依赖 |
|---|---|---|
| 硬失真 | 高次谐波（H5+）占比大 | 阈值后骤增 |
| 软饱和 | 低次谐波（H2/H3）为主 | 平滑渐增 |
| 谐波饱和/exciter | 特定谐波被选择性增强（H2=温暖，H7+=空气感） | 可调 mix，非纯电平触发 |

### 标准采集模板

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| frequency_response | ✅ 必测 | 确认是否有频段处理（磁带常有高频衰减） |
| harmonic | ✅ 必测 | **多电平 THD + 谐波结构**（关键） |
| compression | ✅ 必测 | 饱和器在高驱动下有增益压缩（Saturn 2 实测 ratio 74） |
| gr_timeline | 可选 | 若有动态行为 |

### 参数扫描建议

- 扫 **Drive / Amount / Harmonics**（饱和强度）
- 档位：5-7 档 [0.3, 0.5, 0.7, 0.85, 1.0]
- exciter 类额外扫 **Mix**（干湿混合）

### 多电平配置（**关键**，T5 落地后必测）

- 多电平 THD 曲线区分软/硬/谐波饱和（见上表）
- **谐波结构随电平变化**：逐档测 H2/H3/H4 百分比
  - 偶次为主（H2/H4）→ 磁带/电子管
  - 奇次为主（H3/H5）→ 硬削波/晶体管

### 验收标准

- harmonic：THD 曲线形态 + 谐波结构匹配子类
- compression：高驱动有压缩（若有）
- 强饱和插件 usable_as_spec=False（compression fit conflict + harmonic artifact 拦截）属正确行为——数据仍入库供建模

---

## 7. 声场类效果器（Stereo Width / MidSide / 空间）

**类别特征**：改变左右声道关系——宽度扩展（增 S 分量）、MidSide 处理（M/S 分别 EQ/压缩）、立体声化（单声道 → 有宽度）。

### ⚠️ 测量能力依赖 T6（架构级新测量类型）

本模板定义**信号设计与验收方向**；实际测量能力（MidSide 解码 + 相关度分析）由 T6 落地。

### 标准采集模板（T6 落地后）

| 测量类型 | 是否采集 | 配置 |
|---|---|---|
| **stereo_width**（新，T6） | ✅ 必测 | MidSide 解码：M=(L+R)/2, S=(L-R)/2 分别测频响 |
| **midside**（新，T6） | ✅ 必测 | M/S 分离频响 + 相关度 ρ |
| frequency_response | ✅ 必测 | 单声道视角（现有）确认整体频响 |
| harmonic / compression | 按需 | 若声场插件含饱和/动态 |

### 信号设计（关键）

- **相关度测试**：输入已知相关度信号（L=R ρ=+1；L=-R ρ=-1），测输出 ρ → 宽度变化
- **立体声化检测**：输入纯 M（L=R 相同），测输出 S 分量非零 = 插件制造宽度
- **宽度扫描**：扫 width 参数 0→100%，观察 S 分量增益曲线

### 验收标准（T6 落地后）

- M/S 分离频响：宽度扩展器 S 频响被提升（尤其高频）
- 相关度：输入 +1 → 输出 < +1 = 宽度被扩展
- 立体声化：纯 M 输入 → S 输出非零

---

## 8. 混合类处理（通道条 / 话放等复合插件）

真实插件常同时含多段处理（话放 = 频段 + 饱和 + 压缩）。**模板组合使用**：

1. 先 freq → 识别频段处理（EQ 部分）
2. 再 harmonic 多电平 → 识别饱和/失真（驱动依赖）
3. 再 compression → 识别动态（阈值依赖）
4. 最后 gr_timeline → 识别时域行为

**判定优先级**：频响特征 > 谐波特征 > 动态特征。混合类按"检测到的处理段"逐段反推，chain_doc 输出多段描述（T7 解耦后支持模块级拆解）。

---

## 9. 验收总表

| 类别 | 必测 | 关键反推 | 验收标准 |
|---|---|---|---|
| 话放 | freq+harmonic+compression | 频段+饱和+压缩混合 | 三特征同时可见 |
| EQ | freq+harmonic | 频点/Q/增益 | 误差 <2% 频点 / <0.5dB |
| 压缩 | compression+gr | threshold/ratio/τ | threshold ±1dB / ratio ±25% |
| 限制器 | compression+harmonic | ratio>10 | 高压缩比 + 削波谐波 |
| clip | compression+harmonic(多电平) | 传输曲线+THD 曲线 | 曲线形态匹配削波类型 |
| 饱和 | harmonic(多电平)+compression | THD 曲线+谐波结构 | 子类判定正确 |
| 声场 | midside+相关度（T6） | M/S 频响+ρ | 宽度/立体声化检测 |

---

## 10. 与工具链的关系

| 工具 | 角色 | 状态 |
|---|---|---|
| `tools/probe_plugin.py` | 探参 + 假面检测（T2 #99） | 待入库 |
| `tools/batch_collect.py` | 采集执行 | 已入库，复现性检查 T3 #98 待加 |
| `tools/aggregate_report.py` | 聚合 + 退化检测 + 直通检测（T4 #100） | 已入库，直通检测待加 |
| `tools/describe_chain.py` | 类型判定 + 规格描述 | 已入库，泛化名误判 #94 待修 |
| `tools/compare_all.py` | 复现性对比（T3） | 已入库 |
| `tools/reverse_derive.py` | 单插件反推 | 已入库 |