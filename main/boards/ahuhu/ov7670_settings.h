#pragma once
#include <stdint.h>

typedef struct {
    uint8_t reg;
    uint8_t val;
} ov7670_reginfo_t;

#define REG_DELAY 0xFF

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

static const ov7670_reginfo_t ov7670_init_qvga_rgb565[] = {
    {0x12, 0x80}, /* COM7: reset all registers */
    {REG_DELAY, 100}, /* delay 100ms */

    {0x12, 0x15}, /* COM7: output format RGB, QVGA, output enable */
    {0x11, 0x00}, /* CLKRC: no prescaler, use ext clock directly */
    {0x0C, 0x00}, /* COM3: default */
    {0x3E, 0x00}, /* COM14: PCLK = HCLK */
    {0x40, 0xD0}, /* COM15: full range RGB565, RGB565 output */
    {0x3A, 0x04}, /* TSIB: bypass */
    {0x8C, 0x00}, /* RGB444: disable */
    {0x32, 0x00}, /* HREF: default, no offset */
    {0x17, 0x13}, /* HSTART: horizontal start */
    {0x18, 0x01}, /* HSTOP:  horizontal stop */
    {0x19, 0x02}, /* VSTART: vertical start */
    {0x1A, 0x7A}, /* VSTOP:  vertical stop */
    {0x03, 0x00}, /* VREF: VSYNC active high, match ESP32 DVP default */

    {0x70, 0xBA}, /* SCALING_XSC: horizontal scaling, color bar ENABLED (bit7=1) */
    {0x71, 0xB5}, /* SCALING_YSC: vertical scaling, 8-bar color bar (bit7=1) */
    {0x72, 0x11}, /* SCALING_DCWCTR: down-sample control */
    {0x73, 0x00}, /* SCALING_PCLK_DIV: no division, PCLK=HCLK=20MHz */
    {0xA2, 0x02}, /* SCALING_PCLK_DELAY: scaling delay */

    {0x15, 0x00}, /* COM10: HREF not inverted, PCLK not inverted (match ESP32 DVP) */

    {0x13, 0xE7}, /* COM8: AGC, AEC, AWB enabled */
    {0x00, 0x00}, /* GAIN: default */
    {0x10, 0x00}, /* AECH: default */
    {0x0D, 0x40}, /* COM4: reserved */
    {0x14, 0x18}, /* COM9: AGC ceiling */
    {0xA5, 0x05}, /* BD50MAX: banding filter */
    {0xAB, 0x07}, /* DB60MAX: banding filter */
    {0x24, 0x95}, /* AGC */
    {0x25, 0x33}, /* AGC */
    {0x26, 0xE3}, /* AGC */
    {0x9F, 0x78}, /* AGC */
    {0xA0, 0x68}, /* AGC */
    {0xA1, 0x03}, /* AGC */
    {0xA6, 0xD8}, /* AGC */
    {0xA7, 0xD8}, /* AGC */
    {0xA8, 0xF0}, /* AGC */
    {0xA9, 0x90}, /* AGC */
    {0xAA, 0x94}, /* AGC */

    {0x4F, 0x80}, /* MTX1: color matrix */
    {0x50, 0x80}, /* MTX2 */
    {0x51, 0x00}, /* MTX3 */
    {0x52, 0x22}, /* MTX4 */
    {0x53, 0x5E}, /* MTX5 */
    {0x54, 0x80}, /* MTX6 */

    {0x55, 0x00}, /* BRIGHT: brightness */
    {0x56, 0x40}, /* CONTRAS: contrast */
    {0x58, 0x9E}, /* AWBCtrl */
    {0x59, 0x88}, /* AWBCtrl */
    {0x5A, 0x88}, /* AWBCtrl */
    {0x5B, 0x44}, /* AWBCtrl */
    {0x5C, 0x67}, /* AWBCtrl */
    {0x5D, 0x49}, /* AWBCtrl */
    {0x5E, 0x0E}, /* AWBCtrl */
    {0x6C, 0x0A}, /* AWBCtrl */
    {0x6D, 0x55}, /* AWBCtrl */
    {0x6E, 0x11}, /* AWBCtrl */
    {0x6F, 0x9F}, /* AWBCtrl */
    {0xB0, 0x84}, /* AWBCtrl */

    {0x7A, 0x20}, /* SLOP */
    {0x7B, 0x10}, /* GAM1 */
    {0x7C, 0x1E}, /* GAM2 */
    {0x7D, 0x35}, /* GAM3 */
    {0x7E, 0x5A}, /* GAM4 */
    {0x7F, 0x69}, /* GAM5 */
    {0x80, 0x76}, /* GAM6 */
    {0x81, 0x80}, /* GAM7 */
    {0x82, 0x88}, /* GAM8 */
    {0x83, 0x8F}, /* GAM9 */
    {0x84, 0x96}, /* GAM10 */
    {0x85, 0xA3}, /* GAM11 */
    {0x86, 0xAF}, /* GAM12 */
    {0x87, 0xC4}, /* GAM13 */
    {0x88, 0xD7}, /* GAM14 */
    {0x89, 0xE8}, /* GAM15 */
    {0x6B, 0x00}, /* DBLV: bypass PLL, internal clock = XCLK = 20MHz */
    {0x2A, 0x00}, /* EXHCH */
    {0x2B, 0x00}, /* EXHCL */
    {0x2D, 0x00}, /* ADCCTR */
    {0x2E, 0x00}, /* ADCCTR */
    {0x0E, 0x61}, /* COM5 */
    {0x0F, 0x4B}, /* COM6 */
    {0x69, 0x00}, /* GFIX */

    {0xFF, 0xFF}, /* End marker */
};