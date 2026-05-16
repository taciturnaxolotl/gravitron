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

static int servo_num_for_name(const char *s)
{
    if (!s) return 1;
    if (!strcmp(s, "tilt")) return 2;
    return 1; // default pan
}

static void handle_command(cJSON *json)
{
    cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    if (!cmd || !cJSON_IsString(cmd)) return;

    const char *cmd_str = cmd->valuestring;
    char out[128];

    out[0] = '\0';

    // ── Move — f/b/l/r ──
    if (!strcmp(cmd_str, "move")) {
        cJSON *dir  = cJSON_GetObjectItem(json, "dir");
        cJSON *spd  = cJSON_GetObjectItem(json, "speed");
        if (!dir || !cJSON_IsString(dir)) return;

        const char *d = dir->valuestring;
             if (!strcmp(d, "forward"))  snprintf(out, sizeof(out), "f");
        else if (!strcmp(d, "backward") || !strcmp(d, "back")) snprintf(out, sizeof(out), "b");
        else if (!strcmp(d, "left"))   snprintf(out, sizeof(out), "l");
        else if (!strcmp(d, "right"))  snprintf(out, sizeof(out), "r");
        else return;

        // If speed was given, set it first
        if (spd && cJSON_IsNumber(spd) && spd->valueint >= 0 && spd->valueint <= 255) {
            char speed_cmd[16];
            snprintf(speed_cmd, sizeof(speed_cmd), "speed %d", spd->valueint);
            uart_bridge_send(speed_cmd, strlen(speed_cmd));
        }
    }
    // ── Motors raw — m dirA spdA dirB spdB ──
    else if (!strcmp(cmd_str, "motors")) {
        cJSON *l = cJSON_GetObjectItem(json, "left");
        cJSON *r = cJSON_GetObjectItem(json, "right");
        int lv = l && cJSON_IsNumber(l) ? l->valueint : 0;
        int rv = r && cJSON_IsNumber(r) ? r->valueint : 0;
        // m <dirA> <spdA> <dirB> <spdB>: dir 1=fwd, 2=back
        int dirA = rv >= 0 ? 1 : 2;
        int dirB = lv >= 0 ? 1 : 2;
        snprintf(out, sizeof(out), "m %d %d %d %d", dirA, abs(rv), dirB, abs(lv));
    }
    // ── Stop — s ──
    else if (!strcmp(cmd_str, "stop")) {
        snprintf(out, sizeof(out), "s");
    }
    // ── Servo — sv <which> <angle> ──
    else if (!strcmp(cmd_str, "servo")) {
        cJSON *sv = cJSON_GetObjectItem(json, "servo");
        cJSON *ang = cJSON_GetObjectItem(json, "angle");
        if (!ang || !cJSON_IsNumber(ang)) return;
        int which = servo_num_for_name(sv && cJSON_IsString(sv) ? sv->valuestring : NULL);
        snprintf(out, sizeof(out), "sv %d %d", which, ang->valueint);
    }
    else if (!strcmp(cmd_str, "servo_step")) {
        // step actions: 1=tilt-up, 2=tilt-down, 3=pan-right, 4=pan-left
        cJSON *act = cJSON_GetObjectItem(json, "action");
        int a = act && cJSON_IsNumber(act) ? act->valueint : 5;
        int which = (a == 1 || a == 2) ? 2 : 1; // tilt or pan
        int step = (a == 1 || a == 3) ? 10 : -10; // direction
        int angle = 90 + step;
        angle = (angle < 0) ? 0 : (angle > 180) ? 180 : angle;
        snprintf(out, sizeof(out), "sv %d %d", which, angle);
    }
    // ── LED — led R G B / ledoff ──
    else if (!strcmp(cmd_str, "led")) {
        cJSON *r = cJSON_GetObjectItem(json, "r");
        cJSON *g = cJSON_GetObjectItem(json, "g");
        cJSON *b = cJSON_GetObjectItem(json, "b");
        int rv = r && cJSON_IsNumber(r) ? r->valueint : 0;
        int gv = g && cJSON_IsNumber(g) ? g->valueint : 0;
        int bv = b && cJSON_IsNumber(b) ? b->valueint : 0;
        if (rv == 0 && gv == 0 && bv == 0) {
            snprintf(out, sizeof(out), "ledoff");
        } else {
            snprintf(out, sizeof(out), "led %d %d %d", rv, gv, bv);
        }
    }
    // ── Speed — speed N ──
    else if (!strcmp(cmd_str, "speed")) {
        cJSON *s = cJSON_GetObjectItem(json, "value");
        int v = s && cJSON_IsNumber(s) ? s->valueint : 200;
        snprintf(out, sizeof(out), "speed %d", v);
    }
    // ── Sensor queries ──
    else if (!strcmp(cmd_str, "distance") || !strcmp(cmd_str, "obstacle")) {
        snprintf(out, sizeof(out), "us");
    }
    else if (!strcmp(cmd_str, "ir")) {
        cJSON *s = cJSON_GetObjectItem(json, "sensor");
        snprintf(out, sizeof(out), "ir");
        (void)s; // Arduino always returns all 3, client can filter
    }
    else if (!strcmp(cmd_str, "battery") || !strcmp(cmd_str, "bat")) {
        snprintf(out, sizeof(out), "bat");
    }
    else if (!strcmp(cmd_str, "ground")) {
        snprintf(out, sizeof(out), "ir"); // closest proxy: all-black = off ground
    }
    else if (!strcmp(cmd_str, "yaw")) {
        snprintf(out, sizeof(out), "yaw");
    }
    else if (!strcmp(cmd_str, "calibrate") || !strcmp(cmd_str, "cal")) {
        snprintf(out, sizeof(out), "cal");
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

static void on_uart_response(const char *txt, int len)
{
    // Arduino now sends JSON lines — wrap in a typed envelope for WS clients
    char buf[528];
    int written = snprintf(buf, sizeof(buf),
                           "{\"type\":\"arduino\",\"data\":%.*s}",
                           len, txt);
    if (written > 0 && written < (int)sizeof(buf)) {
        websocket_broadcast(buf, written);
    }
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
            uart_bridge_send("s", 1);
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
