
#ifndef _AHUHU_CONFIG_H_
#define _AHUHU_CONFIG_H_

// ============================================================
// AhuhuS3 硬件引脚定义
// 支持两种显示方案：
//  - ST7735S (SPI 彩屏):  GPIO42(SCK), GPIO41(MOSI), GPIO1(RES), GPIO2(DC), GPIO20(CS), GPIO19(BL)
//  - SH1106/SSD1306 (I2C): GPIO41(SDA), GPIO42(SCL)
// 通过 DISPLAY_TYPE_ST7735S 宏选择，默认使用 SH1106
// 使用 ST7735S 彩屏时取消下面注释，使用 SH1106 时注释掉
#define DISPLAY_TYPE_ST7735S
// ============================================================
// 麦克风 INMP441 / ICS43434  →  GPIO4~6
// 功放 MAX98357A             →  GPIO7,15,16
// TF卡 SPI                  →  GPIO3,8,17,18
// 音量按钮                   →  GPIO39,40
// Boot 按钮                  →  GPIO0
// 内置 LED (WS2812)          →  GPIO48
// 电机 TC1508 / L298N        →  GPIO11,12,13,14
// 4G ML307R (UART)           →  GPIO9,10
// ============================================================

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16
#else
#define AUDIO_I2S_GPIO_WS GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7
#endif

#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

#ifdef DISPLAY_TYPE_ST7735S

// ST7735S 彩屏 SPI 引脚配置
// SCK/MOSI 复用原 1306 显示屏引脚，其他引脚用右侧剩余引脚
// 用户指定：SCL→GPIO42, SDA→GPIO41, RES→GPIO1, DC→GPIO2, CS→GPIO20, BLK→GPIO19
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_SPI_SCLK        GPIO_NUM_42
#define DISPLAY_SPI_MOSI        GPIO_NUM_41
#define DISPLAY_SPI_CS          GPIO_NUM_20
#define DISPLAY_DC              GPIO_NUM_2
#define DISPLAY_RST             GPIO_NUM_1
#define DISPLAY_BACKLIGHT       GPIO_NUM_19

// ST7735S 显示参数
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          160
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        0
#define DISPLAY_MIRROR_X         1
#define DISPLAY_MIRROR_Y         1
#define DISPLAY_SWAP_XY         0
#define DISPLAY_SPI_MODE        0
#define DISPLAY_SPI_SCLK_HZ     (40 * 1000 * 1000)
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

#else

// SH1106/SSD1306 OLED 显示屏 (I2C)
#ifndef DISPLAY_TYPE_ST7735S
#define DISPLAY_SDA_PIN         GPIO_NUM_41
#define DISPLAY_SCL_PIN         GPIO_NUM_42
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64
#define DISPLAY_MIRROR_X        1
#define DISPLAY_MIRROR_Y        1
#endif
#define SH1106

#endif // DISPLAY_TYPE_ST7735S

// 硬件前进/后退接线反了：交换 LF/LB、RF/RB 引脚，保证 LF/RF=前进、LB/RB=后退
#define MOTOR_LF_GPIO           GPIO_NUM_12
#define MOTOR_LB_GPIO           GPIO_NUM_11
#define MOTOR_RF_GPIO           GPIO_NUM_14
#define MOTOR_RB_GPIO           GPIO_NUM_13

#ifdef DISPLAY_TYPE_ST7735S
// OV7670 摄像头 DVP 接口 (全部禁用，引脚给彩屏和OCTAL PSRAM使用)
// 注意：GPIO15~21 是八线PSRAM数据线，GPIO45~48 是PSRAM高字节地址线，
// GPIO47~48也是电源域，全部置为GPIO_NUM_NC防止摄像头代码误操作导致冲突
#define CAMERA_PIN_D0           GPIO_NUM_NC
#define CAMERA_PIN_D1           GPIO_NUM_NC
#define CAMERA_PIN_D2           GPIO_NUM_NC
#define CAMERA_PIN_D3           GPIO_NUM_NC
#define CAMERA_PIN_D4           GPIO_NUM_NC
#define CAMERA_PIN_D5           GPIO_NUM_NC
#define CAMERA_PIN_D6           GPIO_NUM_NC
#define CAMERA_PIN_D7           GPIO_NUM_NC
#define CAMERA_PIN_XCLK         GPIO_NUM_NC
#define CAMERA_PIN_PCLK         GPIO_NUM_NC
#define CAMERA_PIN_VSYNC        GPIO_NUM_NC
#define CAMERA_PIN_HREF         GPIO_NUM_NC
#define CAMERA_PIN_SIOC         GPIO_NUM_NC
#define CAMERA_PIN_SIOD         GPIO_NUM_NC
#define CAMERA_PIN_PWDN         GPIO_NUM_NC
#define CAMERA_PIN_RESET        GPIO_NUM_NC
#else
// OV7670 摄像头 DVP 接口
// 避开 PSRAM(GPIO35-39)，保留 GPIO43/44 给串口监视器
// I2C SCCB 复用显示屏 GPIO41/42
// 牺牲: GPIO48(LED) + GPIO40(音量+)
#define CAMERA_PIN_D0           GPIO_NUM_1
#define CAMERA_PIN_D1           GPIO_NUM_2
#define CAMERA_PIN_D2           GPIO_NUM_9
#define CAMERA_PIN_D3           GPIO_NUM_10
#define CAMERA_PIN_D4           GPIO_NUM_19
#define CAMERA_PIN_D5           GPIO_NUM_20
#define CAMERA_PIN_D6           GPIO_NUM_21
#define CAMERA_PIN_D7           GPIO_NUM_46
#define CAMERA_PIN_XCLK         GPIO_NUM_48
#define CAMERA_PIN_PCLK         GPIO_NUM_47
#define CAMERA_PIN_VSYNC        GPIO_NUM_45
#define CAMERA_PIN_HREF         GPIO_NUM_40
#define CAMERA_PIN_SIOC         GPIO_NUM_42
#define CAMERA_PIN_SIOD         GPIO_NUM_41
#define CAMERA_PIN_PWDN         GPIO_NUM_NC
#define CAMERA_PIN_RESET        GPIO_NUM_NC
#endif
#define XCLK_FREQ_HZ            20000000

// 4G模块 ML307R (已禁用，引脚给摄像头用)
#define ML307_TX_PIN            GPIO_NUM_NC
#define ML307_RX_PIN            GPIO_NUM_NC

// TF卡 SPI 接口 (左侧引脚 GPIO3/8/17/18)
#define SDCARD_SPI_HOST         SPI3_HOST
#define SDCARD_SPI_CS           GPIO_NUM_3
#define SDCARD_SPI_MOSI         GPIO_NUM_18
#define SDCARD_SPI_MISO         GPIO_NUM_17
#define SDCARD_SPI_SCLK         GPIO_NUM_8
#define SDCARD_MOUNT_POINT      "/sdcard"

#endif