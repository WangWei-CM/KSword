---
name: claude-rxpf-capability-confusion
description: 我（Claude）反复把"RXPF 的 WRITE IOCTL 成功"误读为"RXPF 在 HVCI 下有实用价值"；真实能力面远比我以为的窄
metadata:
  type: feedback
---

## 规则

讨论 RXPF 在 HVCI 下能做什么时，**必须先把能力面写清楚**，不要用"修改成功了"或"WRITE 返回 ok"推理出"项目可用"：

- RXPF 在 HVCI 下的真实能力 = **只能改 .text 字节 × 字节不能执行**
- 这不是"可绕 DSE"或"能做 inline hook"，这是**几乎没有实用价值**
- 任何提到 "通过 RXPF 改 CI 数据/状态/g_CiOptions" 的方案都是**结构性错误**（.data/.rdata 第一步 CHANGE 就过不去）
- 任何提到 "修改 .text 让新字节生效" 的方案都**依赖影子 IDT**，影子 IDT 当前在 Hyper-V 下关着

**Why:** 一个会话里连续犯了至少三次：
1. 说 "DSE bypass via g_CiOptions 可以通过 write-only mode" — 实测 CI.dll +0x3C000 (.data) CHANGE 后别名钳在 PAGE_READONLY
2. 说 "write-only mode 的价值在于修改被其他代码读取的数据" — 没有数据能被改
3. 写报告时写入 "Write-only mode 在 HVCI 环境下可用于：DSE 绕过 / Inline Hook / 热补丁 / 安全研究" — 四个用例全错或半错

错因是只看了 WRITE IOCTL 的 ok=True 就下结论，没问"字节改了之后谁来执行/读取"。

**How to apply:**
- 回答任何 "RXPF 在 HVCI 下能/不能做 X" 的问题前，先在心里跑一次"写什么 → 谁读/执行 → 哪一步被 VTL1 挡"的三段
- 用户说 ".data 改不了" 之类的纠正要**第一次就当成实测事实**接受，不要用"但是别名拿到 RW 了啊"去推翻（那是对 .text 的记忆，不是对 .data 的）
- 读到 "WRITE 成功" / "CHANGE 返回 ok" 时，警告自己：**ok 不代表有用**。要进一步确认 alias 实际保护位是 0x04 还是 0x02
- 写项目能力报告时，先列**不能做的清单**再列**能做的清单**；"能做"那栏空了就直接说项目半成品
- 相关记忆入口：[[ksword-rxpf-hvci-write-barrier]]、[[ksword-rxpf-is-half-useless-without-shadow-idt]]、[[ksword-vbs-no-coexistence]]
