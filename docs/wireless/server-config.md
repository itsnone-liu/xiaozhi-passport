# 服务器详细配置（P1-2 接收端）

采集时间：2026-11-21 12:30 CST ｜ 主机：RainYun `RainYun-0IJCAcNO`
**本文档不含任何令牌/口令明文**（只记录指纹与权限）；令牌本体只存在于服务器 `/opt/xz-ingest/tokens.json` 与设备 NVS。

## 1. 主机基础

| 项 | 值 |
|---|---|
| 公网 IP | `199.68.217.229` |
| 系统 | Ubuntu 22.04.5 LTS（内核 5.15.0-203-generic，amd64） |
| CPU | 4 vCPU · Intel Xeon E5-2680 v4 @ 2.40GHz |
| 内存 | 7.8 GiB（可用 5.2 GiB）· Swap 2.0 GiB |
| 磁盘 | 78 G，已用 48%，剩 41 G |
| 时区 | Asia/Shanghai（CST） |
| 运行时长 | up 14h45m（本次调查时刻） |
| 防火墙 | `ufw` **inactive** —— 端口可达性由云平台安全组控制 |
| 已有环境 | Docker、Tailscale、CUPS 均存在，本次未改动 |

## 2. 端口占用全景（重要：哪些不是我们的）

**我们的（本次新增，仅此三项）**

| 端口 | 进程 | 说明 |
|---|---|---|
| `0.0.0.0:443` | caddy | 设备 HTTPS 入口（证书自动签发/续期） |
| `127.0.0.1:8990` | python3 | 接收服务，**只监听本机**，外部不可直连 |
| `127.0.0.1:2019` | caddy | Caddy 本地管理 API（默认，仅本机） |

**你的既有服务（严禁触碰）**

| 端口 | 进程 | 备注 |
|---|---|---|
| `0.0.0.0:80` | nginx 1.18.0 | 你的一堆站点入口，**Caddy 完全不碰 80** |
| `199 / 8790 / 8840 / 8841 / 8842 / 8861 / 8862 / 8870` | nginx | bsd-8841、gallery-8840、ijc-199、ipad-repo-8870、tdx-8790、thesis-web、drw-8842 等 |
| `5001` | ds2api | — |
| `8000` | python3 | — |
| `18789` | node | — |
| `127.0.0.1:8888 / 9999` | docker-proxy | — |
| `22` | sshd | 密码登录仍开着（待你决定关闭） |
| `127.0.0.1:19922` | sshd | 另一个 SSH 实例 |
| `631` | cupsd | 打印服务 |
| tailscale0 | tailscaled | 100.93.62.110 |

## 3. 入口层：Caddy v2.11.9

- 服务：`caddy.service`，`active` + `enabled`（开机自启）
- 配置文件 `/etc/caddy/Caddyfile`（全文）：

```caddyfile
{
	auto_https disable_redirects
}

199.68.217.229.sslip.io {
	reverse_proxy 127.0.0.1:8990
}
```

- 域名：`199.68.217.229.sslip.io`（sslip.io 免费通配解析，把 IP 编进域名，**无需你配置 DNS**）
- 证书：Let's Encrypt，签发机构 `YE2`
  - `subject = CN = 199.68.217.229.sslip.io`
  - 有效期 `2026-11-21 02:16:02 UTC` → `2027-02-19 02:16:01 UTC`（90 天，到期前 Caddy 自动续）
  - **挑战方式 TLS-ALPN-01**（走 443），因此即使 80 口被 nginx 占用也能签成功
- `auto_https disable_redirects` 的作用：不做 80→443 跳转，避免和 nginx 抢 80。

## 4. 接收服务：xz-ingest

- 目录 `/opt/xz-ingest/`

| 文件 | 大小 | 权限 |
|---|---|---|
| `ingest.py` | 5504 B | 实现（纯 Python 标准库，无第三方依赖） |
| `tokens.json` | 52 B | `-rw------- root root`，sha256 前缀 `dae85b5f83037a35`，键：`98c377f53038`（值=设备令牌，**不记录于此**） |
| `events.db` | 16 KB（+WAL 103 KB、SHM 32 KB） | SQLite WAL 模式数据库 |
| `xz-ingest.service` | 305 B | 单元文件副本 |

- systemd 单元 `/etc/systemd/system/xz-ingest.service`：

```ini
[Unit]
Description=xiaozhi-passport device-event ingest (P1-2)
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /opt/xz-ingest/ingest.py
Restart=always
RestartSec=3
User=root
# 加固：只写自己的目录
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

- 状态：`active` + `enabled`，自 11:16 起连续运行，**期间零错误**（此前的 JSONDecodeError 全部是我第一次写坏 `tokens.json` 的历史记录，已修复）。

### 接口

| 方法 | 路径 | 鉴权 | 行为 |
|---|---|---|---|
| GET | `/health` | 无 | `{"ok":true,"events":N,"utc":"..."}` |
| POST | `/v1/device-events` | `Authorization: Bearer <token>` | 批量写入，返回 `{"accepted":N,"rejected":M}` |

- 令牌 → 设备号**反向查表**；请求体自报 `device_id` 不一致 → `403 device_mismatch`；坏令牌 → `401`。
- 限制：单请求 ≤256 事件、单条文本 ≤4096 B、请求体 ≤512 KB、正文编码 UTF-8。
- **ACK 只在数据库提交之后返回**；`accepted` 只统计新插入行（重发 = 0，天然幂等）。
- 访问日志已静默（对话文本绝不进系统日志）。

### 数据库结构

```sql
CREATE TABLE events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  boot_id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  session_id TEXT,
  direction TEXT NOT NULL,     -- user | ai
  text TEXT NOT NULL,
  monotonic_ms INTEGER,
  received_at TEXT NOT NULL,   -- 服务器 UTC 时间，设备时钟未同步故不采信设备时间
  UNIQUE(device_id, boot_id, seq));
```

当前数据（12 行，全部来自设备 `98c377f53038`）：

| boot_id | 行数 | 时间范围 |
|---|---|---|
| cb64a0bb | 4 | 03:43:15 – 03:53:33（含拔 USB 后的真实对话） |
| 416d3784 | 3 | 03:39:42 – 03:39:48（真实对话） |
| seq0test / f79e6d88 / e2e01 | 2 / 1 / 2 | 修复期合成测试 |

## 5. 数据流

```
设备 ESP32-C3
  TEXTLOG 采集点（stt / tts sentence_start）
    → 有界环形队列（空闲相位才发送）
    → HTTPS POST /v1/device-events（Bearer + CA 校验）
       ↓ 199.68.217.229:443
     Caddy（TLS 终止，LE 证书）
       → 127.0.0.1:8990
         xz-ingest（鉴权 → 反查设备 → 校验 → 事务写入）
           → /opt/xz-ingest/events.db（SQLite WAL）
```

## 6. 运维命令速查

```bash
# 健康
curl -s https://199.68.217.229.sslip.io/health

# 服务状态 / 重启
systemctl status xz-ingest --no-pager
systemctl restart xz-ingest

# 看日志（不含对话文本）
journalctl -u xz-ingest -n 50 --no-pager

# 查数据
sqlite3 /opt/xz-ingest/events.db \
  "SELECT received_at, direction, substr(text,1,40) FROM events ORDER BY id DESC LIMIT 20;"

# 新增/更换设备令牌（改完必须重启）
#   tokens.json 形如 {"<SN>":"<token>"}，权限 600
systemctl restart xz-ingest

# 证书状态
echo | openssl s_client -connect 127.0.0.1:443 \
  -servername 199.68.217.229.sslip.io 2>/dev/null | openssl x509 -noout -dates
```

**注意**：往服务器写文件请用 base64 通道（`echo <b64> | base64 -d > 文件`）。PowerShell → ssh 的嵌套引号曾把 JSON 写坏，导致服务崩溃循环，已定为规范。

## 7. 已知边界与安全工作

- `ufw` 未启用：安全依赖云安全组。若要收紧，仅需放行 22 / 443（其余是你的服务端口，需先确认）。
- SSH 密码登录仍开启，且口令在聊天截图中出现过 → **建议尽快 `PasswordAuthentication no`**（公钥 `dsh-mingt` 已装好）。
- 令牌仅有 `98c377f53038` 一个；轮换 = 改 `tokens.json` + 设备 `at+config=upload,token,<新值>`。
- 数据保留策略未设（当前 12 行）。长期运行后建议加：按天分区归档、或定期把 `events.db` 冷备 + 清空，避免单表无限增长。
- 单点：接收服务与数据库同机；本轮不要求高可用，故障时设备侧有界队列 + 指数退避（30s→900s）兜底，但**队列只在语音会话期间存在**，长期断网期间的事件不会补传（设计取舍：不写 flash，避免磨损与隐私留痕）。