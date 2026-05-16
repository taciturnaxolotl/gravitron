#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WebSocket server on the given HTTP handle.
 * All WebSocket handlers are registered on this server.
 */
void websocket_init(httpd_handle_t server);

/**
 * Broadcast a text frame to all connected WebSocket clients.
 */
void websocket_broadcast(const char *data, int len);

/**
 * Send the camera info object to all connected clients.
 * Returns JSON with frame_size, quality, brightness, contrast, saturation,
 * vflip, hmirror, and detected sensor PID.
 */
void websocket_send_camera_info(void);

/**
 * Returns the number of connected WebSocket clients.
 */
int websocket_client_count(void);

#ifdef __cplusplus
}
#endif

#endif
