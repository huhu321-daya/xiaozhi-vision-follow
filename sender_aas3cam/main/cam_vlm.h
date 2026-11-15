#ifndef CAM_VLM_H
#define CAM_VLM_H

#include <stdint.h>

// 读取云端校准结果：has -1=尚无结果 0=无人 1=有人；zone -1左 0中 1右；dist 1远 2中 3近；last_ms 最近校准时间
void cam_vlm_get(int *has, int *zone, int *dist, uint32_t *last_ms);

// 启动云端校准任务（内部判断跟随开关，未开启不校准）
void cam_vlm_start(void);

#endif