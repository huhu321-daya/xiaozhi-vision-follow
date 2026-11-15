# ESP32-S3-WROOM-1 模组引脚图说明

根据你发的图片，引脚定义如下：

## 左侧排针（从下往上）

| 引脚 | 功能 | 备注 |
|------|------|------|
| **5V0** | 电源输入 | 5V 供电 |
| **GND** | 接地 | |
| **GPIO14** | SPIWP, SUBSPIDQS | 电机右后退 |
| **GPIO13** | FSPIQ, SUBSPID | 电机右前进 |
| **GPIO12** | FSPICLK, SUBSPICLK | 电机左后退 |
| **GPIO11** | FSPID, SUBSPID | 电机左前进 |
| **GPIO10** | FSPICS0, SUBSPICS | 4G 模块 RXD |
| **GPIO9** | FSPICS1, SUBSPICS | 4G 模块 TXD |
| **GPIO46** | LOG, GPIO_NUM_46 | |
| **GPIO48** | RGB_LED | 内置 WS2812 LED |
| **GPIO0** | BOOT | Boot 按键 |
| **GPIO3** | SPIIO7, FSPIHD, SUBSPIDQS | |
| **GPIO5** | SPIIO5, FSPID, SUBSPID | |
| **GPIO6** | SPIIO6, FSPICLK, SUBSPICLK | |
| **GPIO7** | SPIIO7, FSPICS0, SUBSPICS | |
| **GPIO8** | SPIIO8, FSPICS1, SUBSPICS | |
| **GPIO18** | ADC2_7, RTC, GPIO_NUM_18 | |
| **GPIO19** | ADC2_8, U1RTS, CLK_OUT2 | |
| **GPIO17** | ADC2_6, U1CTS, CLK_OUT1 | |
| **GPIO16** | ADC2_5, U1TXD, CLK_OUT0 | |
| **GPIO15** | ADC2_4, U1RXD, CLK_OUT0 | |
| **GPIO10** | ADC2_3, TOUCH6, GPIO_NUM_10 | |
| **GPIO9** | ADC2_2, TOUCH5, GPIO_NUM_9 | |
| **GPIO8** | ADC2_1, TOUCH4, GPIO_NUM_8 | |
| **GPIO7** | ADC2_0, TOUCH3, GPIO_NUM_7 | |
| **GPIO6** | ADC1_6, TOUCH2, GPIO_NUM_6 | |
| **GPIO5** | ADC1_5, TOUCH1, GPIO_NUM_5 | |
| **GPIO4** | ADC1_4, TOUCH0, GPIO_NUM_4 | |
| **GPIO3** | ADC1_3, GPIO_NUM_3 | |
| **GPIO2** | ADC1_2, GPIO_NUM_2 | |
| **GPIO1** | ADC1_1, GPIO_NUM_1 | |
| **GPIO0** | ADC1_0, GPIO_NUM_0 | |
| **EN** | 复位引脚 | 标为 RST |
| **3V3** | 电源输出 | 3.3V |
| **3V3** | 电源输出 | 3.3V |
| **RST** | 复位按键 | 连接到 EN 引脚 |

## 右侧排针（从下往上）

| 引脚 | 功能 | 备注 |
|------|------|------|
| **GND** | 接地 | |
| **USB_D-** | USB-OTG 负 | |
| **USB_D+** | USB-OTG 正 | |
| **GPIO19** | ADC2_8, U1RTS, CLK_OUT2 | |
| **GPIO20** | ADC2_9, U1CTS, CLK_OUT1 | |
| **GPIO41** | RTC, GPIO_NUM_41 | 显示屏 SDA |
| **GPIO42** | RTC, GPIO_NUM_42 | 显示屏 SCL |
| **GPIO43** | U0TXD, CLK_OUT1 | |
| **GPIO44** | U0RXD, CLK_OUT2 | |
| **GND** | 接地 | |
| **GPIO45** | MTDI, GPIO_NUM_45 | |
| **GPIO46** | MTDO, GPIO_NUM_46 | |
| **GPIO39** | MTMS, GPIO_NUM_39 | 音量减 |
| **GPIO40** | MTCK, GPIO_NUM_40 | 音量加 |
| **GPIO1** | GPIO_NUM_1 | |
| **GPIO2** | GPIO_NUM_2 | |
| **GPIO3** | GPIO_NUM_3 | |
| **GPIO4** | GPIO_NUM_4 | |
| **GPIO5** | GPIO_NUM_5 | |
| **GPIO6** | GPIO_NUM_6 | |
| **GPIO7** | GPIO_NUM_7 | |
| **GPIO8** | GPIO_NUM_8 | |
| **GPIO9** | GPIO_NUM_9 | |
| **GPIO10** | GPIO_NUM_10 | |
| **GPIO11** | GPIO_NUM_11 | |
| **GPIO12** | GPIO_NUM_12 | |
| **GPIO13** | GPIO_NUM_13 | |
| **GPIO14** | GPIO_NUM_14 | |
| **GPIO15** | GPIO_NUM_15 | |
| **GPIO16** | GPIO_NUM_16 | |
| **GPIO17** | GPIO_NUM_17 | |
| **GPIO18** | GPIO_NUM_18 | |
| **GPIO19** | GPIO_NUM_19 | |
| **GPIO20** | GPIO_NUM_20 | |
| **GPIO21** | GPIO_NUM_21 | |
| **GPIO22** | GPIO_NUM_22 | |
| **GPIO23** | GPIO_NUM_23 | |
| **GPIO24** | GPIO_NUM_24 | |
| **GPIO25** | GPIO_NUM_25 | |
| **GPIO26** | GPIO_NUM_26 | |
| **GPIO27** | GPIO_NUM_27 | |
| **GPIO28** | GPIO_NUM_28 | |
| **GPIO29** | GPIO_NUM_29 | |
| **GPIO30** | GPIO_NUM_30 | |
| **GPIO31** | GPIO_NUM_31 | |
| **GPIO32** | GPIO_NUM_32 | |
| **GPIO33** | GPIO_NUM_33 | |
| **GPIO34** | GPIO_NUM_34 | |
| **GPIO35** | GPIO_NUM_35 | |
| **GPIO36** | GPIO_NUM_36 | |
| **GPIO37** | GPIO_NUM_37 | |
| **GPIO38** | GPIO_NUM_38 | |
| **GPIO39** | GPIO_NUM_39 | |
| **GPIO40** | GPIO_NUM_40 | |
| **GPIO41** | GPIO_NUM_41 | |
| **GPIO42** | GPIO_NUM_42 | |
| **GPIO43** | GPIO_NUM_43 | |
| **GPIO44** | GPIO_NUM_44 | |
| **GPIO45** | GPIO_NUM_45 | |
| **GPIO46** | GPIO_NUM_46 | |
| **GPIO47** | GPIO_NUM_47 | |
| **GPIO48** | GPIO_NUM_48 | |