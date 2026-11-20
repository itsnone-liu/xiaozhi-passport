# trae_card 固件文本日志补丁指南

> **✅ 2026-11-20 状态：已实现、已烧录、已实测验证。**
> 源码 [arraylee/ai-passport-xiaozhi-custom](https://github.com/arraylee/ai-passport-xiaozhi-custom)（FoloToy 官方 play `xiaozhi-ai-chatbot` 的 githubUrl），补丁在 `textlog` 分支 @ 5ae1e1d（+4 行）。
> 实测串口输出（UTF-8 中文完好、会话号配对）：
> ```
> I (452529) xiaozhi: TEXTLOG	user	80fea708	hello helllo.
> I (453164) xiaozhi: TEXTLOG	ai	80fea708	哎呀，爸爸你这是在跟我打招呼呀，还是自己练英语呢？
> ```
> 构建产物与官方 bin 字节数完全一致（5,687,560 B，仅 bootloader/app 不同，资产段相同）。

**目的**：在 trae_card（小智衍生固件，v2.1.0-xiaozhi.29）中恢复对话文本的串口日志输出，使"设备侧自动记忆采集"成为可能。
**依据**：device-text-capture-probe.md 的判定（文本到达设备、日志层被魔改丢弃）。
**上游参照**：xiaozhi-esp32 @ 831ffd1（本地克隆 `D:\dsh\ws\xiaozhi-esp32`）。

## 1. 改哪里

文本消息的唯一消费点是 `Application` 注册给协议层的 `OnIncomingJson` 回调（上游 `main/application.cc` L618–724）：

| 消息 | 上游位置 | trae_card 中的定位方法 |
|---|---|---|
| `{"type":"tts","state":"sentence_start","text":...}` | application.cc **L678–692** | 在源码树 grep `sentence_start` |
| `{"type":"stt","text":...}` | application.cc **L694–708** | grep `"stt"`（注意带引号，避免撞上 stt 其他词）|

上游在这两处**本来就有**文本日志（trae_card 丢弃了它们）：

```cpp
// L686, tts sentence_start 分支内：
ESP_LOGI(TAG, "<< %s", text->valuestring);   // AI 的回答文本
// L702, stt 分支内：
ESP_LOGI(TAG, ">> %s", text->valuestring);   // 用户说的话（STT）
```

## 2. 建议的补丁形态（优于照抄上游）

为方便采集适配器解析，建议输出**机器可读单行**，并把协议中可选的 `session_id` 一并带上：

```cpp
// 在 OnIncomingJson 回调顶部（拿到 root 后）取一次：
auto sid = cJSON_GetObjectItem(root, "session_id");
const char* session = cJSON_IsString(sid) ? sid->valuestring : "";

// tts sentence_start 分支内（trae_card 对应位置）：
ESP_LOGI(TAG, "TEXTLOG {\"dir\":\"ai\",\"sid\":\"%s\",\"text\":\"%s\"}",
         session, text->valuestring);

// stt 分支内：
ESP_LOGI(TAG, "TEXTLOG {\"dir\":\"user\",\"sid\":\"%s\",\"text\":\"%s\"}",
         session, text->valuestring);
```

注意：文本内容来自服务器，**不可信任**——含 `"` 或 `\` 会破坏行内 JSON。稳妥做法是用 cJSON 的 `cJSON_Print` 片段或直接输出制表符分隔格式：

```cpp
ESP_LOGI(TAG, "TEXTLOG\tai\t%s\t%s", session, text->valuestring);
ESP_LOGI(TAG, "TEXTLOG\tuser\t%s\t%s", session, text->valuestring);
```

采集适配器按 `TEXTLOG\t` 前缀过滤行、按制表符切分（文本中禁止出现制表符，可要求服务端或本地清洗）。

## 3. 采集适配器侧（PC 端，零固件依赖，可先行开发）

复用本次探测验证过的看门狗式捕获（`D:\dsh\logs\serial-watch-*.log` 同款脚本）：

1. 轮询 ESP32 CDC 串口（设备深睡会掉线，唤醒自动重连）
2. 过滤 `TEXTLOG` 前缀行 → 结构化入库
3. **脱敏落盘**（任务书约束：儿童对话明文不得离开本机；日志文件视为敏感数据）
4. 无 `session_id` 时用本地轮次计数配对（探针日志已证实轮次可用状态机 `state=8→5→6` 边界清晰区分）

## 4. 风险与前置条件

1. **必须拿到 trae_card 源码**（本机只有上游克隆）。改动预计 ≤10 行。
2. **ESP-IDF 版本**：trae_card 用 v5.5.3；当前上游已要求 IDF ≥ v6.0.1（见上游 AGENTS.md）。**只 patch trae_card 自己的树，不要向上游 rebase**。
3. **OTA 覆盖风险**：小智有云端 OTA，自编固件烧录后需确认版本号/OTA 策略，防止被云推回官方版。
4. **STT 到达性未单独实证**：屏幕照片只证实了 AI 侧气泡；用户侧文本（stt.text）按协议与上游代码应同样到达，烧录后第一时间验证（说一句测试语，看 `TEXTLOG user` 行）。
5. **隐私**：串口日志将含儿童对话明文——采集器与日志目录都要按敏感数据处理。

## 5. 验证清单（烧录后）

- [x] 问一句"今天天气怎么样" → 串口出现 `TEXTLOG user ...` 与 `TEXTLOG ai ...` 各至少一行 ✅ 2026-11-20 实测（12 行 / 6 轮）
- [x] 长回答 → 多行 `TEXTLOG ai`（逐句）且顺序完整 ✅ 实测连续 ai 行成对出现
- [x] `sid` 字段是否为空（空则启用本地轮次配对）✅ 非空且按会话变化（c79913f7、80fea708），本地轮次配对不需要
- [ ] 打断（按"打断"）→ 已收句子不丢、后续句子停止
- [ ] 深睡唤醒 → 看门狗捕获自动续上，TEXTLOG 行无丢失

## 6. 实际落地的构建与烧录配方（Windows）

**补丁实现**（与 §2 建议一致，session 取自 `c->session` 而非 root，两处插入点：tts sentence_start 分支、stt 分支 farewell 判断之前）：
- `components/platform/xiaozhi/xiaozhi.c`：`ESP_LOGI(TAG, "TEXTLOG\tai\t%s\t%s", c->session, line->valuestring);` 与 `ESP_LOGI(TAG, "TEXTLOG\tuser\t%s\t%s", c->session, line->valuestring);`

**构建**（ESP-IDF v5.5.3 装在 `D:\dsh\tools\esp-idf`，完整脚本 `D:\dsh\tools\build-miniapp.cmd`）：
1. `call D:\dsh\tools\esp-idf\export.bat`（**必须 call**，直接 && 链会被 export.bat 的 exit 杀掉）
2. `set "IDF_PATH=D:/dsh/tools/esp-idf"`（**必须正斜杠**：recovery_boot_hook 的 CMakeLists 用 `$ENV{IDF_PATH}` 拼路径，反斜杠 `\d` 会让 CMake 报 "Invalid character escape"）
3. `idf.py -B build-miniapp -DSDKCONFIG=<绝对路径>\build-miniapp\sdkconfig -DPASSPORT_MINIAPP=ON build`（SDKCONFIG 必须绝对路径，cmd 的 %CD% 单行展开是旧目录）
4. 打包：IDF python 跑 `tools/pack_miniapp.py --build-dir build-miniapp --output releases/...-miniapp.bin`（自带资产 SHA 全套校验）

**烧录**（= 官方小程序安装器等效操作，六段写入）：
```
python -m esptool --chip esp32c3 -p COM4 --baud 460800 write_flash "@flash_args"
# 在 build-miniapp 目录：0x0 bootloader / 0x8000 分区表 / 0x10000 trae_card.bin
# / 0x2de000 petart / 0x3bb000 companion / 0x54d000 imgava
```
**不触碰**：nvs(0x9000)、cardid、netcfg、recovery(0x6c0000 常驻安装器)——WiFi、云端身份（MAC 绑定）、微信小程序生态全部保留。

**还原保险**：烧录前全片备份 `D:\dsh\backup\passport-2.1.0-xiaozhi.29-full-8mb.bin`（8,388,608 B）；官方原版 bin 可随时从 play API `/api/download/official/xiaozhi-ai-chatbot` 下载（sha256 40940a3d…）。

**采集注意**：System.IO.Ports.SerialPort 默认 ASCII 编码会把中文读成 `?`，必须设 `sp.Encoding = [Text.Encoding]::UTF8`。
