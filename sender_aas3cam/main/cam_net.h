#ifndef CAM_NET_H
#define CAM_NET_H

#include <stdbool.h>

// 跟随开关：由接收板通过 FOLLOW_ON/FOLLOW_OFF 控制，决策任务和云端校准任务共用
extern volatile bool cam_follow_active;

bool cam_net_wifi_start(void);
void cam_net_udp_start(void);

#endif