// xz_uploader.c — P1-2 无线上传模块（V1 任务书 §4 设备端最小改造）
//
// 职责：把 TEXTLOG 两处采集点(stt/tts sentence_start)产生的对话文本事件
//       经有界 RAM 队列 → 相位门控(仅空闲相位) → HTTPS 批量 POST 到自有接收服务。
// 硬规则(P1-1 实测结论,见 docs/wireless/P1-1-transport-probe.md):
//   音频相位(SPEAK/LISTEN/THINK/解码中)禁止 TLS 握手——TTS 中 largest 块仅 ~10KB,
//   强行握手实测把 minever 砸到 544B。因此上传任务只在 XZ_STANDBY/XZ_READY 相位运行。
// 队列策略:满则丢最新并计数(never 静默,never 阻塞回调);文本超限截断并计数。
// 配置:NVS 命名空间 "uploader" (url/token/en),由产线 AT 指令写入,不入代码库。
// 幂等:boot_id(每次开机随机)+seq(单调递增) = 服务器端去重键。
#include "xz_uploader.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "xiaozhi.h"

static const char *TAG = "xzu";

#define XZU_SLOTS     16      // 事件槽数(一批对话≈stt 1 + tts 分句 N;超限丢新+计数)
#define XZU_TEXT_MAX  224     // 单事件文本字节上限(超限截断+计数);中文≈74字
#define XZU_SESSION_MAX 40
#define XZU_URL_MAX   128
#define XZU_TOKEN_MAX 96
#define XZU_BATCH     4       // 单次 POST 事件数上限(控制请求体体积)
#define XZU_BODY_MAX  2600    // JSON 请求体暂存区
#define XZU_TASK_STACK 5120
#define XZU_TICK_MS   1000
#define XZU_BACKOFF_MIN_MS  30000
#define XZU_BACKOFF_MAX_MS  900000

typedef struct {
    uint32_t seq;
    uint32_t mono_ms;
    char session[XZU_SESSION_MAX];
    char direction[8];        // "user"(4+NUL) / "ai" —— 必须容纳 user 全词,截断即被服务端拒收
    uint16_t len;
    char text[XZU_TEXT_MAX];
} xzu_event_t;

typedef struct {
    // 环形队列(初始化时一次性堆分配,避免 BSS 挤占 TLS 握手所需的大块内存)
    xzu_event_t *ring;
    volatile uint8_t head, tail, count;
    portMUX_TYPE lock;
    // 统计
    uint32_t seq_next;
    uint32_t dropped_full;    // 队列满丢新
    uint32_t dropped_not_ready; // 队列尚未建立(云端未连/未启用)期间丢弃
    uint32_t truncated;       // 文本截断次数
    uint32_t sent_ok;         // 成功上传事件数
    uint32_t send_fail;       // POST 失败次数
    // 身份
    char device_id[16];
    char boot_id[9];
    // 配置(NVS 持久)
    char url[XZU_URL_MAX];
    char token[XZU_TOKEN_MAX];
    bool enabled;
    // 运行
    SemaphoreHandle_t kick;
    uint32_t backoff_ms;
    bool inited;
    bool cloud_ready;
} xzu_t;

static xzu_t S;
static void start_now(void);   // 条件满足时才真正占内存(队列+任务)

// ---------- 配置持久化 ----------
static void load_config(void) {
    nvs_handle_t h;
    S.url[0] = S.token[0] = '\0';
    S.enabled = false;
    if (nvs_open("uploader", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof S.url - 1;
    if (nvs_get_str(h, "url", S.url, &n) != ESP_OK) S.url[0] = '\0';
    n = sizeof S.token - 1;
    if (nvs_get_str(h, "token", S.token, &n) != ESP_OK) S.token[0] = '\0';
    uint8_t en = 0;
    if (nvs_get_u8(h, "en", &en) == ESP_OK) S.enabled = en != 0;
    nvs_close(h);
}

bool xz_uploader_set_url(const char *url) {
    if (!url || !url[0]) return false;
    nvs_handle_t h;
    if (nvs_open("uploader", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "url", url) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) strlcpy(S.url, url, sizeof S.url);
    return ok;
}

bool xz_uploader_set_token(const char *token) {
    if (!token || !token[0]) return false;
    nvs_handle_t h;
    if (nvs_open("uploader", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "token", token) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) strlcpy(S.token, token, sizeof S.token);
    return ok;
}

bool xz_uploader_set_enabled(bool on) {
    nvs_handle_t h;
    if (nvs_open("uploader", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_u8(h, "en", on ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        S.enabled = on;
        if (on) {
            start_now();                 // 云端已就绪时立即生效
            if (S.kick) xSemaphoreGive(S.kick);
        }
    }
    return ok;
}

// ---------- 采集侧(被 xiaozhi.c 消息回调调用,绝不允许阻塞) ----------
void xz_uploader_enqueue(const char *direction, const char *session,
                         const char *text) {
    if (!S.inited || !S.ring || !text || !text[0]) {
        if (S.inited && text && text[0]) {
            taskENTER_CRITICAL(&S.lock);
            S.dropped_not_ready++;
            taskEXIT_CRITICAL(&S.lock);
        }
        return;
    }
    taskENTER_CRITICAL(&S.lock);
    if (S.count >= XZU_SLOTS) {
        S.dropped_full++;
        taskEXIT_CRITICAL(&S.lock);
        return;
    }
    xzu_event_t *e = &S.ring[S.head];
    taskEXIT_CRITICAL(&S.lock);

    // 槽位内容只在 count 增加前可见,可安全地在临界区外填充
    memset(e, 0, sizeof *e);
    e->seq = S.seq_next++;
    e->mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(e->direction, direction == NULL ? "?" : direction, sizeof e->direction);
    strlcpy(e->session, session == NULL ? "" : session, XZU_SESSION_MAX);
    size_t len = strlen(text);
    if (len > XZU_TEXT_MAX - 1) {
        len = XZU_TEXT_MAX - 1;
        e->len = (uint16_t)len;
        memcpy(e->text, text, len);
        e->text[len] = '\0';
        taskENTER_CRITICAL(&S.lock); S.truncated++; taskEXIT_CRITICAL(&S.lock);
    } else {
        e->len = (uint16_t)len;
        memcpy(e->text, text, len + 1);
    }

    taskENTER_CRITICAL(&S.lock);
    if (S.count >= XZU_SLOTS) {   // 填充期间队列满:丢弃本条(保守,不覆盖)
        S.dropped_full++;
        taskEXIT_CRITICAL(&S.lock);
        return;
    }
    S.head = (uint8_t)((S.head + 1) % XZU_SLOTS);
    S.count++;
    taskEXIT_CRITICAL(&S.lock);
    if (S.kick) xSemaphoreGive(S.kick);
}

void xz_uploader_selftest(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        char buf[48];
        snprintf(buf, sizeof buf, "自测事件 #%u", i + 1);
        xz_uploader_enqueue(i % 2 ? "ai" : "user", "selftest", buf);
    }
}

// 立即尝试发送(清除退避并唤醒上传任务;测试/运维用)
void xz_uploader_flush(void) {
    if (!S.inited) return;
    S.backoff_ms = 0;
    if (S.kick) xSemaphoreGive(S.kick);
}

unsigned xz_uploader_pending(void) { return S.count; }

void xz_uploader_stats(uint32_t *pending, uint32_t *dropped, uint32_t *truncated,
                       uint32_t *sent_ok, uint32_t *send_fail,
                       uint32_t *dropped_not_ready) {
    if (pending) *pending = S.count;
    if (dropped) *dropped = S.dropped_full;
    if (truncated) *truncated = S.truncated;
    if (sent_ok) *sent_ok = S.sent_ok;
    if (send_fail) *send_fail = S.send_fail;
    if (dropped_not_ready) *dropped_not_ready = S.dropped_not_ready;
}

void xz_uploader_identity(const char **device_id, const char **boot_id,
                          uint32_t *seq_next) {
    if (device_id) *device_id = S.device_id;
    if (boot_id) *boot_id = S.boot_id;
    if (seq_next) *seq_next = S.seq_next;
}

bool xz_uploader_enabled(void) { return S.enabled; }
const char *xz_uploader_url(void) { return S.url; }
const char *xz_uploader_token_tail(void) {
    size_t n = strlen(S.token);
    return n > 4 ? S.token + n - 4 : S.token;
}

// ---------- 上传侧(独立低优先级任务) ----------
static bool audio_safe_phase(void) {
    xz_view_t view;
    xiaozhi_snapshot(&view);
    // 允许发送的相位:欢迎屏(开机未开始会话)/会话就绪/待机(会话间)。
    // 任何音频活动相位(SPEAK/LISTEN/THINK/RADIO/FAREWELL/WAKING)一律不发。
    switch (view.phase) {
        case XZ_WELCOME:
        case XZ_READY:
        case XZ_STANDBY:
            return true;
        default:
            return false;
    }
}

// 弹出 n 条已发送条目
static void pop_events(uint8_t n) {
    taskENTER_CRITICAL(&S.lock);
    while (n-- && S.count) {
        S.tail = (uint8_t)((S.tail + 1) % XZU_SLOTS);
        S.count--;
    }
    taskEXIT_CRITICAL(&S.lock);
}

static bool build_body(uint8_t n, char *body, size_t cap, size_t *out_len) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) { cJSON_Delete(root); cJSON_Delete(arr); return false; }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id", S.device_id);
    uint8_t idx = S.tail;
    for (uint8_t i = 0; i < n; i++) {
        const xzu_event_t *e = &S.ring[idx];
        idx = (uint8_t)((idx + 1) % XZU_SLOTS);
        if (!e->len || !e->direction[0]) {
            // 防御:空槽绝不外发(出现即 ring 记账异常,计入 dropped)
            taskENTER_CRITICAL(&S.lock); S.dropped_full++; taskEXIT_CRITICAL(&S.lock);
            continue;
        }
        cJSON *ev = cJSON_CreateObject();
        if (!ev) continue;
        cJSON_AddStringToObject(ev, "boot_id", S.boot_id);
        cJSON_AddNumberToObject(ev, "seq", e->seq);
        cJSON_AddStringToObject(ev, "session_id", e->session);
        cJSON_AddStringToObject(ev, "direction", e->direction);
        cJSON_AddStringToObject(ev, "text", e->text);
        cJSON_AddNumberToObject(ev, "monotonic_ms", e->mono_ms);
        cJSON_AddItemToArray(arr, ev);
    }
    cJSON_AddItemToObject(root, "events", arr);
    char *printed = cJSON_PrintBuffered(root, cap - 1, false);
    cJSON_Delete(root);
    if (!printed) return false;
    size_t len = strlen(printed);
    if (len >= cap) { cJSON_free(printed); return false; }
    memcpy(body, printed, len + 1);
    *out_len = len;
    cJSON_free(printed);
    return true;
}

static char s_ack_buf[96];    // 服务端 ACK 正文(仅 accepted/rejected 计数,无文本)
static int  s_ack_len;

static esp_err_t http_evt(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && !evt->user_data &&
        s_ack_len < (int)sizeof s_ack_buf - 1) {
        int cp = evt->data_len;
        if (cp > (int)sizeof s_ack_buf - 1 - s_ack_len) cp = (int)sizeof s_ack_buf - 1 - s_ack_len;
        memcpy(s_ack_buf + s_ack_len, evt->data, cp);
        s_ack_len += cp;
        s_ack_buf[s_ack_len] = '\0';
    }
    return ESP_OK;
}

static bool post_batch(char *body, size_t len) {
    esp_http_client_config_t cfg = {
        .url = S.url,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,   // 完整 CA 校验,禁止关闭
        .keep_alive_enable = false,
        .user_agent = "xz-upload/1.0",
        .method = HTTP_METHOD_POST,
        .event_handler = http_evt,
    };
    s_ack_len = 0; s_ack_buf[0] = '\0';
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return false;
    char auth[XZU_TOKEN_MAX + 8];
    snprintf(auth, sizeof auth, "Bearer %s", S.token);
    esp_http_client_set_header(cl, "Content-Type", "application/json");
    esp_http_client_set_header(cl, "Authorization", auth);
    esp_http_client_set_post_field(cl, body, len);
    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "XZU ack http=%d body=%s", status, s_ack_buf);
    return err == ESP_OK && status == 200;
}

static void upload_task(void *arg) {
    static char body[XZU_BODY_MAX];   // 单飞任务私有,静态复用
    while (true) {
        if (S.kick) xSemaphoreTake(S.kick, pdMS_TO_TICKS(XZU_TICK_MS));
        if (!S.inited || !S.enabled || !S.url[0] || !S.token[0] || S.count == 0)
            continue;
        if (S.backoff_ms) {   // 失败退避中(带抖动)
            vTaskDelay(pdMS_TO_TICKS(S.backoff_ms));
            S.backoff_ms = 0;
        }
        if (!audio_safe_phase()) {
            // 观测:持续被相位门挡住时,每 ~60s 报一次,绝不静默
            static uint32_t blocked_ticks;
            if (++blocked_ticks >= 60) {
                blocked_ticks = 0;
                xz_view_t v; xiaozhi_snapshot(&v);
                ESP_LOGI(TAG, "XZU wait-idle phase=%d pending=%u",
                         (int)v.phase, S.count);
            }
            continue;          // 音频相位:只入队,不发送(P1-1 硬规则)
        }

        uint32_t ok_before = S.sent_ok;
        while (S.count && audio_safe_phase()) {
            uint8_t n = S.count < XZU_BATCH ? S.count : XZU_BATCH;
            size_t len = 0;
            if (!build_body(n, body, sizeof body, &len)) {
                ESP_LOGW(TAG, "body 构建失败(count=%u)", S.count);
                break;
            }
            if (post_batch(body, len)) {
                pop_events(n);
                S.sent_ok += n;
                S.backoff_ms = 0;
            } else {
                S.send_fail++;
                S.backoff_ms = S.backoff_ms < XZU_BACKOFF_MIN_MS
                    ? XZU_BACKOFF_MIN_MS
                    : S.backoff_ms * 2;
                if (S.backoff_ms > XZU_BACKOFF_MAX_MS) S.backoff_ms = XZU_BACKOFF_MAX_MS;
                // ±25% 抖动
                uint32_t jitter = S.backoff_ms / 4;
                S.backoff_ms = S.backoff_ms - jitter + (esp_random() % (2 * jitter + 1));
                ESP_LOGW(TAG, "POST 失败,退避 %ums(累计失败 %u)",
                         S.backoff_ms, S.send_fail);
                break;
            }
        }
        if (S.sent_ok != ok_before)
            ESP_LOGI(TAG, "XZU sent=%u pending=%u dropped=%u trunc=%u hwm=%u fail=%u",
                     S.sent_ok, S.count, S.dropped_full, S.truncated,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), S.send_fail);
    }
}

bool xz_uploader_init(const char *device_id) {
    if (S.inited) return true;
    memset(&S, 0, sizeof S);
    strlcpy(S.device_id, device_id ? device_id : "?", sizeof S.device_id);
    uint32_t rnd;
    esp_fill_random(&rnd, sizeof rnd);
    snprintf(S.boot_id, sizeof S.boot_id, "%08lx", (unsigned long)rnd);
    load_config();
    S.kick = xSemaphoreCreateBinary();
    if (!S.kick) return false;
    S.inited = true;   // 注意:此处不占任何大块内存 —— 队列/任务等云端连上后才建
    ESP_LOGI(TAG, "上传模块待命 device=%s boot=%s en=%d(队列待云端就绪后分配)",
             S.device_id, S.boot_id, S.enabled);
    return true;
}

// 前置条件全部满足时真正占内存:队列 + 上传任务。幂等。
static void start_now(void) {    if (S.ring || !S.cloud_ready || !S.enabled || !S.url[0] || !S.token[0])
        return;
    S.ring = heap_caps_calloc(XZU_SLOTS, sizeof(xzu_event_t), MALLOC_CAP_DEFAULT);
    if (!S.ring) {
        ESP_LOGW(TAG, "队列分配失败(%u B) —— 保持纯串口 TEXTLOG 模式",
                 (unsigned)(XZU_SLOTS * sizeof(xzu_event_t)));
        return;
    }
    if (xTaskCreate(upload_task, "xzu", XZU_TASK_STACK, NULL, 2, NULL) != pdPASS) {
        heap_caps_free(S.ring);
        S.ring = NULL;
        ESP_LOGW(TAG, "上传任务创建失败 —— 保持纯串口 TEXTLOG 模式");
        return;
    }
    ESP_LOGI(TAG, "上传队列就绪(%u 槽) heap=%u largest=%u", XZU_SLOTS,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// 云端语音链路建立成功后由 xiaozhi.c 调用(P1-2 资源纪律:
// 官方语音 TLS 握手优先享有全部内存,上传模块只能在语音连上后再占用)。
void xz_uploader_on_cloud_ready(void) {
    if (!S.inited || S.cloud_ready) return;
    S.cloud_ready = true;
    start_now();
}
