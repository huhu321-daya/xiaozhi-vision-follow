#pragma once

#include "esp_cam_sensor_types.h"
#include "esp_cam_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OV7670_SCCB_ADDR 0x21
#define OV7670_PID       0x76
#define OV7670_SENSOR_NAME "OV7670"

esp_cam_sensor_device_t *ov7670_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif