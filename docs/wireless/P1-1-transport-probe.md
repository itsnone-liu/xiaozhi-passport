# P1-1 通信资源探针报告（Transport Probe）

**日期**：2026-11-21
**任务书**：`docs/wireless/plan-v1.md` §7 P1-1（按评审意见拆为 P1-1a 裸握手探测先行）
**固件**：textlog 基线 + `tls_probe.c`（探针 ~110 行，`at+command=tlsprobe,<label>` 串口触发，独立 10KB 栈低优先级任务）
**探测目标**：`https://valid-isrgrootx1.letsencrypt.org/`（Let's Encrypt 官方验证端点，与生产端点同类证书链；完整 CA 校验开启，未做任何弱化）

## 结论（先读这里）

**HTTPS 路线：有条件通过。条件 = 上传只在非音频相位执行（攒批/轮次间隙），音频进行中禁止发起 TLS 握手。**

| 相位 | 握手 | 关键数据 | 判定 |
|---|---|---|---|
| 空闲（官方 MQTT over TLS 常连接在跑） | ✅ 2435ms 成功，HTTP 200 | free 64900→64624（净耗 276B）；largest 21504 不变；**minever 峰值零下降**；栈 HWM 7420/10240 | 生产可用 |
| TTS 播放中（state=8，解码器占用中） | ❌ 557ms 失败（TLS 分配失败） | 起始 free=21132、largest=**10240**；握手尝试把 largest 压到 3072、free 到 9840、**minever 砸穿到 544B** | 禁区：距 OOM 一步之遥 |
| 麦克风流中 | 未采到（状态号误判，自动触发未命中） | — | 非必要：TTS 已是最坏相位；按"音频中禁发"规则无需此数据 |

## 失败记录（任务书要求如实保留）

- TTS 相位握手失败本身（见上表）——这不是探针事故，是被测事实。
- MIC 相位探针未触发：自动触发脚本对 state 号的假设错误。不影响结论（最坏相位已覆盖），P1-2 若需补测再补。
- 构建期三次编译失败（esp_crt_bundle 组件名在本树归 mbedtls；esp_tls_t 私有化改 getter；error API 参数修正）——均已解决，固件探针本身零运行时事故、零崩溃、零 watchdog。

## 对既有云语音链路的影响

- 空闲相位探针：无任何可观察影响（状态机、heap 曲线与基线一致）。
- TTS 相位探针：**TTS 播放全程连续（state=8 无中断、无重启、无 watchdog），主观听感待所有者确认**；但 minever=544B 表明若并发其他分配即可能 OOM——该相位必须禁止上传，规则写死进固件而非依赖运气。

## 与评审意见（P1-1a/1b 拆分）的对账

| 评审预判 | 实测结果 |
|---|---|
| "30KB 塞不下第二条握手，大概率不够" | **音频中：对**（largest 10240 连一个 16KB TLS 缓冲都放不下，实际砸到 544B） |
| 空闲余量不足以握手 | **错——空闲 free=65KB，全握手峰值零消耗**（旧 30KB 数据来自终端实验固件时代，不适用于官方固件） |
| 攒批可把握手摊薄、避开音频 | 采纳为硬规则：**事件在音频相位只入有界队列，轮次间隙批量 flush** |
| AEAD/UDP 作为备选 | 降级为"不需要"：空闲窗口 HTTPS 余量充足；仅当未来出现"音频中必须上传"的需求再启用 |

## 设备端上传任务的设计约束（P1-2 直接引用）

1. 相位门：仅 `XZ_STANDBY`/空闲态允许发起 HTTPS；SPEAK/LISTEN/THINK/解码期间只入队。
2. 有界队列（容量按"一轮长对话=stt 1 条 + tts 分句 N 条"估），满则显式丢弃+计数。
3. 攒批：≥10 条或会话结束（轮次回到空闲）即发；短连接即可（握手 2.4s 可接受，每轮至多一次）。
4. 探针代码保留在源码树（`at+command=tlsprobe`），P1-2/P1-3 长跑随时复测。

## 复现

```
串口 115200 → at+command=tlsprobe,idle   （或 tts/mic）
输出前缀 "tlsprobe: PROBE …"，含 free/largest/minever/stack_hwm/hs_ms/HTTP 状态
```

探针源码：`components/app/src/tls_probe.c`（ai-passport-xiaozhi-custom，textlog 分支）
