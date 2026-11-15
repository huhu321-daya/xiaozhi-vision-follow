#ifndef CAM_CAMERA_H
#define CAM_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool     cam_camera_init(void);
bool     cam_camera_ready(void);
size_t   cam_camera_copy(uint8_t *dst, size_t cap);
uint32_t cam_camera_seq(void);
int64_t  cam_camera_last_us(void);
uint32_t cam_camera_frames(void);

void     cam_camera_follow(int8_t *has, int8_t *offset, int8_t *dist);

#endif