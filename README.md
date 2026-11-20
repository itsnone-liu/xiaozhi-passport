# 小智 Passport

AI Passport 硬件（FoloToy ESP32-C3，BLE 注册名 AI PASSPORT）× 小智固件（trae_card v2.1.0-xiaozhi.29，MQTT over TLS → 小智官方云）。

目标：为「小智儿童陪伴长期记忆」打通**设备侧 STT/TTS 文本自动采集**。

## 当前进展（2026-11-20）

| 交付物 | 说明 |
|---|---|
| [docs/device-text-capture-probe.md](docs/device-text-capture-probe.md) | **可行性探针报告（P0 结案）**：串口 5 轮对话零文本 × 屏幕照片证实渲染对话文字 × 协议文档 × 上游源码，四方证据判定 `needs_firmware_modification` |
| [docs/firmware-text-log-patch.md](docs/firmware-text-log-patch.md) | **固件补丁指南**：改哪里（OnIncomingJson 的 stt / tts sentence_start 分支）、TEXTLOG 输出格式、PC 采集适配器设计、OTA/IDF 风险、烧录后验证清单 |

## 结论一句话

对话文本**确实下发到设备**（屏幕气泡即为实证），原版 xiaozhi-esp32 固件默认就打文本日志（application.cc L686/L702），trae_card 魔改日志层时丢弃了它们——找回预计 ≤10 行改动。

## 待办

- [ ] trae_card 源码入库（当前缺，补丁无法落行）
- [ ] 落 TEXTLOG 补丁 + 烧录验证（5 项清单见补丁指南 §5）
- [ ] PC 端采集适配器（看门狗串口捕获 + TEXTLOG 解析 + 脱敏落盘，不依赖固件改动可先行）
- [ ] 长期记忆管道（memory.save / memory.search 的 MCP 路线与设备侧路线汇合）

## 注意

- 本仓库**保持私有**：文档含家庭内网环境信息（IP / MAC / SSID）。
- 串口日志含儿童对话明文，任何采集器输出均按敏感数据对待（任务书约束）。
