#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

httpd_handle_t http_server_start(void);
void http_server_stop(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif
