# P1-0 现状核对报告（Baseline）

**日期**：2026-11-21
**任务书**：`docs/wireless/plan-v1.md` §7 P1-0（只读核对，不刷机）
**结论先行**：恢复点与源码链路完备，**满足进入 P1-1 的停点条件**；一项待所有者决策（仓库可见性，见 §5）。

---

## 1. 设备资产（两台，一号已退役）

| 代号 | 状态 | 说明 |
|---|---|---|
| unit-1 | **物理损伤，已送售后**（2026-11-21 结案） | TEXTLOG 补丁的首次烧录验证机（2026-11-20 完成）。显示排线接触不良 + 主板级间歇故障 |
| unit-2 | **现役**，P1 全部后续阶段使用 | 原实验机（曾刷自研 thin-terminal 固件），2026-11-21 刷回官方小智 2.1.0-xiaozhi.29 + 恢复原厂 cardid 身份，已在所有者小智账号下在线 |

unit-2 身份恢复方式：从调研期备份中切出 cardid 分区（0x66A000 / 4KB）重新写入；
`mqtt connected` 实测通过，控制台显示在线。绑定关系随原厂身份自动恢复（云端对已绑定设备不下发绑定码，属正常行为）。

## 2. 固件与源码基线

| 项 | 值 |
|---|---|
| 官方基线 | trae_card `2.1.0-xiaozhi.29`（ESP-IDF v5.5.3，MQTT over TLS → 小智官方云） |
| 源码仓库 | 本地 `ai-passport-xiaozhi-custom`，分支 **`textlog`** |
| TEXTLOG 补丁 | commit **`5ae1e1d`**（xiaozhi.c +4 行：`stt` 分支记 user 文本、`tts sentence_start` 分支记 ai 文本，均含 session id） |
| 补丁验证 | 2026-11-20 unit-1 实机烧录，收到带非空会话标识的中文 user/ai 文本 |
| unit-2 当前固件 | `5ae1e1d` + 一次已废弃的 UDP 广播实验（详见 §6，将被 P1-1 探针固件覆盖） |
| 已知未验证项 | 打断处理时的文本事件、深睡唤醒后的采集持续性、无线传输（→ P1-1/P1-3） |

## 3. 备份与恢复（停点硬性要求 ✓）

| 备份 | 路径 | 校验 | 用途 |
|---|---|---|---|
| unit-2 终端实验期全片 | `D:\dsh\backup\experiment-f53038-full-8mb-20261121.bin` | sha256 `61875af1…24c60` | thin-terminal 固件存档（含比 GitHub HEAD 新的二进制，源码在服务器侧） |
| unit-2 P1 baseline full-dump | `D:\dsh\backup\f53038-p1-baseline-full-8mb.bin` | sha256 `7190B2D1…72E78` | **Current-state recovery point** (official firmware + cardid + textlog-line experimental build) |
| unit-2 cardid 切片 | `.feishu-files/…folotoy-key-slice-0x66A000.bin` | sha256 `5EC94649…1D73C` | 身份分区单独恢复（4KB） |
| unit-1 全片（售后前） | `D:\dsh\backup\passport-2.1.0-xiaozhi.29-full-8mb.bin` | — | 仅存档；**禁止刷入 unit-2**（身份克隆冲突） |

**恢复步骤**（unit-2 回到官方基线 + 身份）：
```
esptool write_flash @flash_args        # 官方六分区：bootloader/分区表/app/petart/companion/imgava
esptool write_flash 0x66A000 cardid-slice.bin   # 身份分区
```

## 4. 文档一致性核对（README 旧待办 vs 实际）

| README/旧文档说法 | 实际状态（本报告为准） |
|---|---|
| "源码未找到" | ✅ 已解决：textlog 分支可用，构建链（Windows + IDF 5.5.3）验证过 |
| "补丁未烧录" | ✅ 已过时：11-20 unit-1 验证、11-21 unit-2 现役 |
| 串口采集为主通道 | ❌ 已被否决（手持设备不常连 PC）；V1 路线 = 无线 HTTPS 上报 |

## 5. 仓库可见性（**待所有者决策**）

- 实测（2026-11-21，GitHub API）：`itsnone-liu/xiaozhi-passport` 为 **public**。
- 本仓库现含：探测报告、补丁指南（无凭据、无儿童数据；文档中设备标识已脱敏）。
- 任务书要求：所有者决定是否设为私有；**在决策前本报告不推送任何新提交**。

## 6. 过程记录：一次超前的 UDP 实验（已回退）

2026-11-21 晚曾按旧讨论在 TEXTLOG 两处插入点旁实现 UDP 明文广播（`255.255.255.255:9999`）并刷入 unit-2。
读到 V1 任务书后判定其违背"公网加密 + 禁止明文兜底"原则，已从源码主线回退（git stash `udp-broadcast v0` 留档）。
unit-2 上残留该实验构建，将在 P1-1 探针固件刷写时自然覆盖；其 TEXTLOG 行为与 `5ae1e1d` 一致，不影响基线判定。

## 7. 网络与配网现状

- 配网方式：SoftAP 手填（所有者 2026-11-18 决定维持现状，已收敛不再讨论）。
- 多 SSID / 手机热点切换能力：**未核实** → 按 V1 §4.3 在 P1-1/P1-3 实测（含 2.4GHz 频段限制）。

## 8. 停点判定

| 停点条件 | 状态 |
|---|---|
| 有可恢复固件备份 | ✓（§3 两份全片 + 身份切片） |
| 补丁所基于源码可确定 | ✓（textlog @ 5ae1e1d，构建链验证过） |
| 文档与实际同步 | ✓（本报告 + 后续 README 更新） |

**判定：满足进入 P1-1（通信资源探针）的条件。**
P1-1 前置缺口：需要美国 Ubuntu 服务器侧的访问方式（域名/SSH/部署权限）与一个用于 TLS 的域名，待所有者提供。
