#ifndef CAMERA_H
#define CAMERA_H

#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_init(void);
camera_fb_t *camera_fb_get(void);
void camera_fb_return(camera_fb_t *fb);
sensor_t *camera_sensor_get(void);

#ifdef __cplusplus
}
#endif

#endif
