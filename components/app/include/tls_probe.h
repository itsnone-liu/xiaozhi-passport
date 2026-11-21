// tls_probe.h — P1-1a TLS 握手资源探针（详见 tls_probe.c 头注释）
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动一次探针（异步，串口输出 TLSPROBE 报告行）。
// label 仅用于报告关联（idle/tts/mic 等业务时机标签）。
// 返回 false = 已有探针在跑或资源不足。
bool tls_probe_start(const char *label);

#ifdef __cplusplus
}
#endif
