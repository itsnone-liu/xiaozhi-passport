// tls_probe.c — P1-1a 裸 TLS 握手资源探针（V1 任务书 §7 P1-1）
//
// 目的：在"已有一条 MQTT over TLS 常连接 + 音频系统运行"的真实固件里，
//       回答"第二条完整 TLS 握手是否塞得下"——一次性、可复现、可对照。
// 触发：串口 at+command=tlsprobe,<label>   (label 例: idle / tts / mic)
// 输出：TLSPROBE 前缀的机器可读行（见下），供 P1-1 报告直接引用。
// 约束：不含任何真实对话数据；不触碰音频路径；只做一次连接即退出。
//
// 探测目标选 valid-isrgrootx1.letsencrypt.org（Let's Encrypt 官方验证端点，
// 常规 LE 证书链 + 200 响应），与未来生产端点的证书链形态同类。

#include "tls_probe.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

static const char *TAG = "tlsprobe";
#define PROBE_URL   "https://valid-isrgrootx1.letsencrypt.org/"
#define PROBE_STACK 10240          // 探针任务栈，实测 hwm 后可回收判断
#define PROBE_PRIO  3              // 低优先级，不与音频/云端任务抢
#define LABEL_MAX   15

static volatile bool s_busy;

static uint32_t largest_block(void) {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

static void tls_probe_task(void *raw) {
    char label[LABEL_MAX + 1];
    strlcpy(label, (const char *)raw, sizeof label);
    free(raw);

    const uint32_t free_b = esp_get_free_heap_size();
    const uint32_t minever_b = esp_get_minimum_free_heap_size();
    const uint32_t large_b = largest_block();
    ESP_LOGI(TAG, "PROBE start label=%s free=%u large=%u minever=%u",
             label, free_b, large_b, minever_b);

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,  // 完整 CA 校验，禁止关闭
        .timeout_ms = 15000,
        .non_block = false,
    };
    esp_tls_t *tls = esp_tls_init();
    int64_t t0 = esp_timer_get_time();
    int ret = esp_tls_conn_http_new_sync(PROBE_URL, &cfg, tls);
    int64_t hs_us = esp_timer_get_time() - t0;

    if (ret < 0) {
        int tls_code = 0, tls_flags = 0;
        esp_tls_error_handle_t eh = NULL;
        esp_tls_get_error_handle(tls, &eh);
        esp_tls_get_and_clear_last_error(eh, &tls_code, &tls_flags);
        ESP_LOGI(TAG, "PROBE fail label=%s hs_ms=%lld tls_code=%d flags=0x%x",
                 label, hs_us / 1000, tls_code, tls_flags);
        ESP_LOGI(TAG, "PROBE mem label=%s free_after=%u large_after=%u minever=%u stack_hwm=%u",
                 label, esp_get_free_heap_size(), largest_block(),
                 esp_get_minimum_free_heap_size(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        esp_tls_conn_destroy(tls);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 一次最小 HTTP 请求/响应，验证会话确实可用（不只是握手成功）
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: valid-isrgrootx1.letsencrypt.org\r\n\r\n";
    int w = esp_tls_conn_write(tls, req, sizeof req - 1);
    char buf[160] = {0};
    int r = esp_tls_conn_read(tls, buf, sizeof buf - 1);
    esp_tls_conn_destroy(tls);

    const uint32_t free_a = esp_get_free_heap_size();
    const uint32_t minever_a = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "PROBE ok label=%s hs_ms=%lld write=%d read=%d resp=\"%.32s\"",
             label, hs_us / 1000, w, r, r > 0 ? buf : "-");
    ESP_LOGI(TAG, "PROBE mem label=%s free_before=%u free_after=%u delta=%d "
             "large_before=%u large_after=%u minever_before=%u minever_after=%u "
             "peak_drop=%u stack_hwm=%u",
             label, free_b, free_a, (int)(free_a - free_b),
             large_b, largest_block(), minever_b, minever_a,
             (uint32_t)(minever_b - minever_a),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_busy = false;
    vTaskDelete(NULL);
}

bool tls_probe_start(const char *label) {
    if (s_busy) return false;
    char *arg = malloc(LABEL_MAX + 1);
    if (!arg) return false;
    strlcpy(arg, label ? label : "nolabel", LABEL_MAX + 1);
    s_busy = true;
    if (xTaskCreate(tls_probe_task, "tlsprobe", PROBE_STACK, arg, PROBE_PRIO, NULL)
        != pdPASS) {
        free(arg);
        s_busy = false;
        return false;
    }
    return true;
}
