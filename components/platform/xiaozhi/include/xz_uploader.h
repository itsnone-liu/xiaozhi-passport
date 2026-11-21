// xz_uploader.h — P1-2 无线上传模块(对话文本事件 → HTTPS 批量上报)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动模块(device_id=SN)。此时不占用大块内存;队列与上传任务在
// 云端语音链路建立(见 xz_uploader_on_cloud_ready)且配置启用后才创建。
// 任何一步失败模块都保持"纯串口 TEXTLOG"降级,绝不影响语音。
bool xz_uploader_init(const char *device_id);

// 云端语音链路连接成功后调用一次(xiaozhi.c mqtt 连接点)。
// P1-2 资源纪律:官方语音 TLS 握手优先享有全部内存。
void xz_uploader_on_cloud_ready(void);

// 采集入口(xiaozhi.c stt / tts sentence_start 分支调用;非阻塞)。
void xz_uploader_enqueue(const char *direction, const char *session_id,
                         const char *text);

// 注入 N 条测试事件(session=selftest,无真实对话)。
void xz_uploader_selftest(unsigned count);

// 清除退避并立即尝试发送(测试/运维用)。
void xz_uploader_flush(void);

// AT 配置面
bool xz_uploader_set_url(const char *url);       // 完整 https://…/v1/device-events
bool xz_uploader_set_token(const char *token);   // Bearer 凭证本体
bool xz_uploader_set_enabled(bool on);
bool xz_uploader_enabled(void);
const char *xz_uploader_url(void);
const char *xz_uploader_token_tail(void);        // 只回显末 4 位

// 观测
unsigned xz_uploader_pending(void);
void xz_uploader_stats(uint32_t *pending, uint32_t *dropped, uint32_t *truncated,
                       uint32_t *sent_ok, uint32_t *send_fail,
                       uint32_t *dropped_not_ready);
void xz_uploader_identity(const char **device_id, const char **boot_id,
                          uint32_t *seq_next);

#ifdef __cplusplus
}
#endif
