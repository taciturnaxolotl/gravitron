#include "websocket.h"
#include "uart_bridge.h"
#include "camera.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/param.h>
#include <lwip/sockets.h>

static const char *TAG = "ws";

// ── Client list ────────────────────────────────────────────────────

typedef struct ws_client {
    int fd;
    struct ws_client *next;
} ws_client_t;

static ws_client_t *g_clients = NULL;
static int g_client_count = 0;
static int64_t g_last_heartbeat_us = 0;
static const int64_t HEARTBEAT_TIMEOUT_US = 3000000;

static void ws_client_add(int fd)
{
    ws_client_t *c = calloc(1, sizeof(ws_client_t));
    c->fd = fd;
    c->next = g_clients;
    g_clients = c;
    g_client_count++;
    ESP_LOGI(TAG, "Client connected (fd=%d, total=%d)", fd, g_client_count);
}

static void ws_client_remove(int fd)
{
    ws_client_t **prev = &g_clients;
    while (*prev) {
        if ((*prev)->fd == fd) {
            ws_client_t *tmp = *prev;
            *prev = tmp->next;
            free(tmp);
            g_client_count--;
            ESP_LOGI(TAG, "Client disconnected (fd=%d, total=%d)", fd, g_client_count);
            return;
        }
        prev = &(*prev)->next;
    }
}

static void ws_client_remove_all(void)
{
    while (g_clients) {
        ws_client_t *tmp = g_clients;
        g_clients = g_clients->next;
        close(tmp->fd);
        free(tmp);
    }
    g_client_count = 0;
}

// ── Low-level WebSocket frame send ─────────────────────────────────
// Sends a text frame over the raw socket fd (no masking: server→client)
static void ws_send_text(int fd, const char *data, int len)
{
    if (fd < 0 || !data || len <= 0) return;

    uint8_t header[10];
    int hdr_len;

    header[0] = 0x81; // FIN + text opcode

    if (len <= 125) {
        header[1] = (uint8_t)len;
        hdr_len = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        hdr_len = 4;
    } else {
        header[1] = 127;
        memset(&header[2], 0, 4);
        header[6] = (len >> 24) & 0xFF;
        header[7] = (len >> 16) & 0xFF;
        header[8] = (len >> 8) & 0xFF;
        header[9] = len & 0xFF;
        hdr_len = 10;
    }

    // Use lwip_write to avoid stdio buffering issues
    lwip_write(fd, header, hdr_len);
    lwip_write(fd, data, len);
}

// ── Broadcast ──────────────────────────────────────────────────────

void websocket_broadcast(const char *data, int len)
{
    ws_client_t *c = g_clients;
    while (c) {
        ws_client_t *next = c->next; // in case send fails and removes client
        ws_send_text(c->fd, data, len);
        c = next;
    }
}

int websocket_client_count(void)
{
    return g_client_count;
}

// ── Command translation ────────────────────────────────────────────

static int dir_str_to_num(const char *dir)
{
    if (!strcmp(dir, "forward")) return 3;
    if (!strcmp(dir, "backward") || !strcmp(dir, "back")) return 4;
    if (!strcmp(dir, "left")) return 1;
    if (!strcmp(dir, "right")) return 2;
    return 0;
}

static int mode_str_to_num(const char *mode)
{
    if (!strcmp(mode, "tracking")) return 1;
    if (!strcmp(mode, "obstacle")) return 2;
    if (!strcmp(mode, "follow")) return 3;
    if (!strcmp(mode, "standby")) return 0;
    return -1;
}

static int servo_str_to_num(const char *s)
{
    if (!strcmp(s, "pan")) return 1;
    if (!strcmp(s, "tilt")) return 2;
    if (!strcmp(s, "both")) return 3;
    return 0;
}

static void handle_command(cJSON *json)
{
    cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    if (!cmd || !cJSON_IsString(cmd)) return;

    const char *cmd_str = cmd->valuestring;
    char out[256];

    if (!strcmp(cmd_str, "move")) {
        cJSON *dir  = cJSON_GetObjectItem(json, "dir");
        cJSON *spd  = cJSON_GetObjectItem(json, "speed");
        cJSON *time = cJSON_GetObjectItem(json, "time_ms");

        if (!dir || !cJSON_IsString(dir)) return;
        int d = dir_str_to_num(dir->valuestring);
        if (!d) return;
        int speed = spd && cJSON_IsNumber(spd) ? spd->valueint : 200;

        if (time && cJSON_IsNumber(time)) {
            snprintf(out, sizeof(out),
                     "{\"N\":2,\"D1\":%d,\"D2\":%d,\"T\":%d}", d, speed, time->valueint);
        } else {
            snprintf(out, sizeof(out),
                     "{\"N\":3,\"D1\":%d,\"D2\":%d}", d, speed);
        }
    }
    else if (!strcmp(cmd_str, "motors")) {
        cJSON *l = cJSON_GetObjectItem(json, "left");
        cJSON *r = cJSON_GetObjectItem(json, "right");
        snprintf(out, sizeof(out),
                 "{\"N\":4,\"D1\":%d,\"D2\":%d}",
                 l && cJSON_IsNumber(l) ? l->valueint : 0,
                 r && cJSON_IsNumber(r) ? r->valueint : 0);
    }
    else if (!strcmp(cmd_str, "stop")) {
        snprintf(out, sizeof(out), "{\"N\":100}");
    }
    else if (!strcmp(cmd_str, "servo")) {
        cJSON *sv = cJSON_GetObjectItem(json, "servo");
        cJSON *ang = cJSON_GetObjectItem(json, "angle");
        if (!sv || !ang) return;
        int s = servo_str_to_num(sv->valuestring);
        if (!s) return;
        snprintf(out, sizeof(out),
                 "{\"N\":5,\"D1\":%d,\"D2\":%d}", s, ang->valueint * 10);
    }
    else if (!strcmp(cmd_str, "led")) {
        cJSON *r = cJSON_GetObjectItem(json, "r");
        cJSON *g = cJSON_GetObjectItem(json, "g");
        cJSON *b = cJSON_GetObjectItem(json, "b");
        cJSON *time = cJSON_GetObjectItem(json, "time_ms");
        if (time && cJSON_IsNumber(time)) {
            snprintf(out, sizeof(out),
                     "{\"N\":7,\"D1\":0,\"D2\":%d,\"D3\":%d,\"D4\":%d,\"T\":%d}",
                     r ? r->valueint : 0, g ? g->valueint : 0, b ? b->valueint : 0,
                     time->valueint);
        } else {
            snprintf(out, sizeof(out),
                     "{\"N\":8,\"D1\":0,\"D2\":%d,\"D3\":%d,\"D4\":%d}",
                     r ? r->valueint : 0, g ? g->valueint : 0, b ? b->valueint : 0);
        }
    }
    else if (!strcmp(cmd_str, "led_brightness")) {
        cJSON *dir = cJSON_GetObjectItem(json, "direction");
        int d = dir && cJSON_IsString(dir) && !strcmp(dir->valuestring, "down") ? 2 : 1;
        snprintf(out, sizeof(out), "{\"N\":105,\"D1\":%d}", d);
    }
    else if (!strcmp(cmd_str, "servo_step")) {
        cJSON *action = cJSON_GetObjectItem(json, "action");
        int a = action && cJSON_IsNumber(action) ? action->valueint : 5;
        if (a < 1) a = 1; if (a > 5) a = 5;
        snprintf(out, sizeof(out), "{\"N\":106,\"D1\":%d}", a);
    }
    else if (!strcmp(cmd_str, "mode")) {
        cJSON *m = cJSON_GetObjectItem(json, "mode");
        if (!m || !cJSON_IsString(m)) return;
        int mode_num = mode_str_to_num(m->valuestring);
        if (mode_num < 0) return;
        snprintf(out, sizeof(out), "{\"N\":101,\"D1\":%d}", mode_num);
    }
    else if (!strcmp(cmd_str, "rocker")) {
        cJSON *d = cJSON_GetObjectItem(json, "direction");
        if (!d || !cJSON_IsString(d)) return;
        int rnum = 5;
        if (!strcmp(d->valuestring, "forward")) rnum = 1;
        else if (!strcmp(d->valuestring, "backward") || !strcmp(d->valuestring, "back")) rnum = 2;
        else if (!strcmp(d->valuestring, "left")) rnum = 3;
        else if (!strcmp(d->valuestring, "right")) rnum = 4;
        snprintf(out, sizeof(out), "{\"N\":102,\"D1\":%d}", rnum);
    }
    else if (!strcmp(cmd_str, "distance")) {
        snprintf(out, sizeof(out), "{\"N\":21,\"D1\":2}");
    }
    else if (!strcmp(cmd_str, "obstacle")) {
        snprintf(out, sizeof(out), "{\"N\":21,\"D1\":1}");
    }
    else if (!strcmp(cmd_str, "ir")) {
        cJSON *s = cJSON_GetObjectItem(json, "sensor");
        int snum = s && cJSON_IsNumber(s) ? s->valueint : 0;
        snprintf(out, sizeof(out), "{\"N\":22,\"D1\":%d}", snum);
    }
    else if (!strcmp(cmd_str, "ground")) {
        snprintf(out, sizeof(out), "{\"N\":23}");
    }
    else if (!strcmp(cmd_str, "camera_info")) {
        websocket_send_camera_info();
        return;
    }
    else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd_str);
        return;
    }

    if (out[0]) {
        ESP_LOGI(TAG, "WS → UART: %s", out);
        uart_bridge_send(out, strlen(out));
    }
}

// ── Arduino UART response → WebSocket broadcast ────────────────────

static void on_uart_response(const char *json_str, int len)
{
    ESP_LOGI(TAG, "UART → WS: %.*s", len, json_str);

    char buf[512];
    snprintf(buf, sizeof(buf), "{\"type\":\"arduino\",\"data\":%.*s}", len, json_str);
    websocket_broadcast(buf, strlen(buf));
}

// ── Heartbeat watchdog ─────────────────────────────────────────────

static void heartbeat_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (g_client_count == 0) continue;

        int64_t now = esp_timer_get_time();
        if (now - g_last_heartbeat_us > HEARTBEAT_TIMEOUT_US) {
            ESP_LOGW(TAG, "Heartbeat timeout — stopping and disconnecting all clients");
            uart_bridge_send("{\"N\":100}", 9);
            ws_client_remove_all();
            g_last_heartbeat_us = esp_timer_get_time();
        }
    }
}

// ── WebSocket HTTP handler ─────────────────────────────────────────

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ws_client_add(httpd_req_to_sockfd(req));
        g_last_heartbeat_us = esp_timer_get_time();
        websocket_send_camera_info();
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    uint8_t buf[512] = {0};

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ws_client_remove(httpd_req_to_sockfd(req));
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ws_client_remove(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        g_last_heartbeat_us = esp_timer_get_time();
        return ESP_OK;
    }

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT || ws_pkt.len == 0) {
        return ESP_OK;
    }

    if (ws_pkt.len > sizeof(buf) - 1) {
        ESP_LOGW(TAG, "Frame too large: %d bytes", ws_pkt.len);
        return ESP_OK;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ws_client_remove(httpd_req_to_sockfd(req));
        return ret;
    }
    buf[ws_pkt.len] = '\0';

    int fd = httpd_req_to_sockfd(req);

    // Heartbeat ping → pong back
    if (!strncmp((char *)buf, "heartbeat", 9)) {
        g_last_heartbeat_us = esp_timer_get_time();
        ws_send_text(fd, "heartbeat_ok", 12);
        return ESP_OK;
    }

    cJSON *json = cJSON_ParseWithLength((const char *)buf, ws_pkt.len);
    if (!json) {
        ESP_LOGW(TAG, "Invalid JSON from client: %.*s", ws_pkt.len, buf);
        return ESP_OK;
    }

    handle_command(json);
    cJSON_Delete(json);
    return ESP_OK;
}

// ── Init ───────────────────────────────────────────────────────────

void websocket_init(httpd_handle_t server)
{
    httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws_uri);

    xTaskCreate(heartbeat_task, "ws_hb", 2048, NULL, 5, NULL);

    uart_bridge_set_rx_callback(on_uart_response);

    ESP_LOGI(TAG, "WebSocket ready at /ws");
}

// ── Camera info ────────────────────────────────────────────────────

void websocket_send_camera_info(void)
{
    sensor_t *s = camera_sensor_get();
    if (!s) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "camera_info");

    cJSON *cam = cJSON_AddObjectToObject(root, "camera");
    cJSON_AddNumberToObject(cam, "framesize", s->status.framesize);
    cJSON_AddNumberToObject(cam, "quality", s->status.quality);
    cJSON_AddNumberToObject(cam, "brightness", s->status.brightness);
    cJSON_AddNumberToObject(cam, "contrast", s->status.contrast);
    cJSON_AddNumberToObject(cam, "saturation", s->status.saturation);
    cJSON_AddNumberToObject(cam, "vflip", s->status.vflip);
    cJSON_AddNumberToObject(cam, "hmirror", s->status.hmirror);
    cJSON_AddNumberToObject(cam, "sensor_pid", s->id.PID);

    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        websocket_broadcast(str, strlen(str));
        free(str);
    }
    cJSON_Delete(root);
}
