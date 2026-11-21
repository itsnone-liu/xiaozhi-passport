# P1-2 无线上传 — 家庭 WiFi 端到端交付报告

日期：2026-11-21 ｜ 状态：✅ 完成（含真实对话验证）｜ 关联：plan-v1.md §4/§5/§6

## 1. 结论

设备（unit-2 F53038，triac 2.1.0-xiaozhi.29 基线 + textlog 分支）对话文本事件已实现
**设备 → HTTPS → 自有服务器 → SQLite** 的可靠传输。真实对话已在无人工干预下落库
（03:39:42 两条 AI 应答实测到达）。官方云语音链路不受影响（见 §3 内存纪律）。

## 2. 端到端架构（全部已部署）

```
设备 ESP32-C3
  TEXTLOG 采集点(xiaozhi.c stt / tts sentence_start)
    → xz_uploader 有界环形队列(16槽×224B,堆分配)
    → 相位门控(仅 WELCOME/READY/STANDBY 空闲相位发送;音频相位只入队)
    → esp_http_client POST,cJSON 序列化,Bearer 认证,crt_bundle 完整 CA 校验
      ↓ https
Caddy :443 (TLS-ALPN-01,LE 证书,域名 199.68.217.229.sslip.io,不占用 :80)
    → 127.0.0.1:8990 xz-ingest(systemd 常驻,纯标准库)
    → SQLite WAL /opt/xz-ingest/events.db,UNIQUE(device_id,boot_id,seq) 幂等
```

- 协议：POST /v1/device-events；服务器反向令牌→设备号；ACK 仅在落库后返回；
  `{"accepted":N,"rejected":M}`，重发幂等（accepted 只计新行）。
- 退避：失败 30s 起指数退避至 900s 封顶，±25% 抖动。
- 每批 ≤4 事件；上传栈 HWM 实测 1940B（5KB 栈）。

## 3. 过程中的三个失败与修复（保留记录）

1. **内存回归（最严重）**：首版在开机即分配 ~21KB（队列+任务+BSS），
   MQTT 握手预算被挤占 → 设备屏"语音连接内存不足"，官方语音断连。
   修复=惰性分配：`xz_uploader_init` 零大块内存；mqtt connected 后才由
   `xz_uploader_on_cloud_ready()` 建队列/任务；体积缩至 16×224+5KB 栈+2.6KB 体缓冲。
   实测：mqtt pre-TLS largest 30720（修复后）vs 15360（回归时）。
2. **相位门错判**：开机未开始会话时相位是 `XZ_WELCOME`（STANDBY 是会话结束后），
   首版门只放 STANDBY/READY → 队列永远不发送。修复=门放行 WELCOME/READY/STANDBY，
   并加 `XZU wait-idle` 60s 观测行，杜绝静默。
3. **direction 截断**：结构体 `char direction[4]` 容不下 "user\0" → strlcpy 截为
   "use" → 服务端校验拒收。症状=每批恰好 1 条 rejected、仅 user 方向。
   修复=`char direction[8]`。此 bug 由服务端 ACK 正文回读（`XZU ack` 行）定位。

## 4. 验收记录

| 检查项 | 结果 |
|---|---|
| 自测事件（user+ai）落库 | ✅ boot cb64a0bb seq 0/1，accepted:2 rejected:0 |
| 真实对话落库 | ✅ boot 416d3784："会不会什么呀？""你话说一半，我没听明白。" |
| 幂等（原样重发） | ✅ accepted:0（PC 预演 + 协议级 UNIQUE 保障） |
| 错误令牌 | ✅ HTTP 401 |
| 语音链路共存 | ✅ mqtt connected 在前、队列分配在后；TTS 期间不上传 |
| 证书校验 | ✅ esp-x509-crt-bundle "Certificate validated"（设备日志） |
| 中文 UTF-8 | ✅ 全链路无乱码 |

## 5. 遗留与已知边界

- **cloud 重连窗口**：若官方云断线重连，重握手发生在队列已占用 ~10KB 之后，
  余量 largest≈17KB，实测可过但余量小；alloc_failed 观测行保留监控。
- 深睡唤醒=重启，路径复走"语音先连、队列后建"，无额外风险。
- 单事件文本上限 224B（中文约 74 字），超限截断+trunc 计数。
- 队列满 16 槽丢新+dropped 计数（不丢已入队事件）。
- 服务器 tokens.json 已修复（此前 PowerShell→ssh 嵌套引号写坏 JSON，改 base64 通道写入）。

## 6. 运维速查

- 服务器健康：`curl https://199.68.217.229.sslip.io/health`
- 设备侧状态：串口 115200 发 `at+config=upload,?`（en/url/token尾4位/pending/dropped/sent/fail/boot_id/seq）
- 手动自测：`at+command=upload,selftest`（session=selftest，无儿童数据）
- 立即冲刷：`at+command=upload,send`
- 配置入口（NVS，不入代码库）：`at+config=upload,url,…` / `upload,token,…` / `upload,enable,0|1`

## 7. 脱敏说明

本文档引用的对话文本为设备真实应答的短句，属验收证据；测试库中 session=selftest
行均为合成事件。库内不存任何令牌明文（tokens.json 权限 600，令牌不在任何 git 仓库）。
