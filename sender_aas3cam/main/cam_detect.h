#ifndef CAM_DETECT_H
#define CAM_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool cam_detect_init(void);
void cam_detect_run(const uint8_t *rgb, int w, int h, int8_t *has, int8_t *offset, int8_t *dist);

#ifdef __cplusplus
}
#endif

#endif