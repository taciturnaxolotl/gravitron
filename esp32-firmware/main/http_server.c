#include "http_server.h"
#include "websocket.h"
#include "camera.h"
#include "uart_bridge.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/param.h>

static const char *TAG = "http";

// Forward declaration for WebSocket init
void websocket_init(httpd_handle_t server);

// ── Helpers ────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}

// ── GET / ── (root — redirect to /index.html) ──────────────────────

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/index.html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ── GET /index.html ────────────────────────────────────────────────

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t index_html_handler(httpd_req_t *req)
{
    const size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start, len);
    return ESP_OK;
}

// ── GET /stream ── (MJPEG multipart) ───────────────────────────────

#define PART_BOUNDARY "123456789000000000000987654321"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY
#define STREAM_BOUNDARY    "\r\n--" PART_BOUNDARY "\r\n"
#define STREAM_PART        "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t jpg_buf_len = 0;
    uint8_t *jpg_buf = NULL;
    char *part_buf[128];

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    while (1) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            bool jpeg_converted = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
            camera_fb_return(fb);
            if (!jpeg_converted) {
                ESP_LOGE(TAG, "JPEG conversion failed");
                res = ESP_FAIL;
                break;
            }
        } else {
            jpg_buf_len = fb->len;
            jpg_buf = fb->buf;
        }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) break;

        int hdr_len = snprintf((char *)part_buf, sizeof(part_buf),
                               STREAM_PART, (unsigned int)jpg_buf_len);
        res = httpd_resp_send_chunk(req, (char *)part_buf, hdr_len);
        if (res != ESP_OK) break;

        res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
        if (fb->format != PIXFORMAT_JPEG) {
            free(jpg_buf);
        } else {
            camera_fb_return(fb);
        }
        if (res != ESP_OK) break;
    }

    // Free if we exited early with a converted JPEG
    if (fb && fb->format != PIXFORMAT_JPEG && jpg_buf) {
        free(jpg_buf);
    } else if (fb) {
        camera_fb_return(fb);
    }

    return res;
}

// ── GET /capture ── (single JPEG) ──────────────────────────────────

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (fb->format == PIXFORMAT_JPEG) {
        esp_err_t ret = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        camera_fb_return(fb);
        return ret;
    }

    size_t jpg_len;
    uint8_t *jpg_buf;
    bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    camera_fb_return(fb);
    if (!ok) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
    free(jpg_buf);
    return ret;
}

// ── GET /api/info ── (camera + connection info) ────────────────────

static esp_err_t api_info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    // Camera info
    sensor_t *s = camera_sensor_get();
    if (s) {
        cJSON *cam = cJSON_AddObjectToObject(root, "camera");
        cJSON_AddNumberToObject(cam, "framesize", s->status.framesize);
        cJSON_AddNumberToObject(cam, "quality", s->status.quality);
        cJSON_AddNumberToObject(cam, "brightness", s->status.brightness);
        cJSON_AddNumberToObject(cam, "contrast", s->status.contrast);
        cJSON_AddNumberToObject(cam, "saturation", s->status.saturation);
        cJSON_AddNumberToObject(cam, "vflip", s->status.vflip);
        cJSON_AddNumberToObject(cam, "hmirror", s->status.hmirror);
        cJSON_AddNumberToObject(cam, "sensor_pid", s->id.PID);

        // Frame size help
        switch (s->status.framesize) {
            case FRAMESIZE_QVGA:   cJSON_AddStringToObject(cam, "framesize_str", "320x240"); break;
            case FRAMESIZE_VGA:    cJSON_AddStringToObject(cam, "framesize_str", "640x480"); break;
            case FRAMESIZE_SVGA:   cJSON_AddStringToObject(cam, "framesize_str", "800x600"); break;
            case FRAMESIZE_XGA:    cJSON_AddStringToObject(cam, "framesize_str", "1024x768"); break;
            case FRAMESIZE_SXGA:   cJSON_AddStringToObject(cam, "framesize_str", "1280x1024"); break;
            case FRAMESIZE_UXGA:   cJSON_AddStringToObject(cam, "framesize_str", "1600x1200"); break;
            default:               cJSON_AddStringToObject(cam, "framesize_str", "unknown"); break;
        }
    }

    // WiFi info — SSID is generated at runtime from MAC, hardcode prefix
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid_prefix", "ELEGOO-");
    cJSON_AddStringToObject(wifi, "ip", "192.168.4.1");

    // Endpoints reference
    cJSON *ep = cJSON_AddObjectToObject(root, "endpoints");
    cJSON_AddStringToObject(ep, "stream", "/stream");
    cJSON_AddStringToObject(ep, "capture", "/capture");
    cJSON_AddStringToObject(ep, "websocket", "/ws");
    cJSON_AddStringToObject(ep, "info", "/api/info");
    cJSON_AddStringToObject(ep, "control", "/api/control");

    cJSON_AddNumberToObject(root, "ws_clients", websocket_client_count());

    char *str = cJSON_Print(root);
    if (!str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ── POST /api/control ── (camera settings) ─────────────────────────

static esp_err_t api_control_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    sensor_t *s = camera_sensor_get();
    if (!s) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int val;
    if (httpd_query_key_value(buf, "var", buf, sizeof(buf)) != ESP_OK) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing 'var' parameter");
        return send_json(req, err);
    }
    if (httpd_query_key_value(buf, "val", (char *)&val, 4) != ESP_OK) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Missing 'val' parameter");
        return send_json(req, err);
    }

    const char *var = buf;

    if (!strcmp(var, "framesize"))       s->set_framesize(s, (framesize_t)val);
    else if (!strcmp(var, "quality"))    s->set_quality(s, val);
    else if (!strcmp(var, "contrast"))   s->set_contrast(s, val);
    else if (!strcmp(var, "brightness")) s->set_brightness(s, val);
    else if (!strcmp(var, "saturation")) s->set_saturation(s, val);
    else if (!strcmp(var, "hmirror"))    s->set_hmirror(s, val);
    else if (!strcmp(var, "vflip"))      s->set_vflip(s, val);
    else if (!strcmp(var, "gainceiling")) s->set_gainceiling(s, (gainceiling_t)val);
    else {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "Unknown variable");
        return send_json(req, err);
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    return send_json(req, ok);
}

// ── CORS preflight ─────────────────────────────────────────────────

static esp_err_t cors_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ── Start / Stop ───────────────────────────────────────────────────

httpd_handle_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7; // enough for stream + WS + infrequent requests

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    // Static page
    httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t index_uri = {
        .uri     = "/index.html",
        .method  = HTTP_GET,
        .handler = index_html_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    // Camera
    httpd_uri_t stream_uri = {
        .uri     = "/stream",
        .method  = HTTP_GET,
        .handler = stream_handler,
    };
    httpd_register_uri_handler(server, &stream_uri);

    httpd_uri_t capture_uri = {
        .uri     = "/capture",
        .method  = HTTP_GET,
        .handler = capture_handler,
    };
    httpd_register_uri_handler(server, &capture_uri);

    // API
    httpd_uri_t info_uri = {
        .uri     = "/api/info",
        .method  = HTTP_GET,
        .handler = api_info_handler,
    };
    httpd_register_uri_handler(server, &info_uri);

    httpd_uri_t control_uri = {
        .uri     = "/api/control",
        .method  = HTTP_POST,
        .handler = api_control_handler,
    };
    httpd_register_uri_handler(server, &control_uri);

    // CORS
    httpd_uri_t cors_uri = {
        .uri     = "/api/*",
        .method  = HTTP_OPTIONS,
        .handler = cors_handler,
    };
    httpd_register_uri_handler(server, &cors_uri);

    // WebSocket
    websocket_init(server);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return server;
}

void http_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}
