# AhuhuS3 开发板配置
ahuhu 请基于小智AI框架代码，帮我将OTTO-ROBOT的代码移植到我们的 ahuhu 这个开发板上，硬件定义和其他功能严格按照我们板子的readme文件来，
基于小智AI框架的 AhuhuS3 开发板移植代码，支持小车电机控制和语音交互。

# 在项目目录下
idf.py fullclean
idf.py build
idf.py flash monitor

更改重要内容：⚠️ 注意事项
这个配置只对 ESP32-S3 芯片生效
如果您使用其他芯片（如 ESP32-C3），需要修改对应的 sdkconfig.defaults.esp32c3 文件
增加 heap size 会占用更多内部 RAM，但 ESP32-S3 有 512KB SRAM，完全够用✅ 在 sdkconfig.defaults.esp32s3 中添加了 CONFIG_FREERTOS_HEAP_SIZE=0x30000
✅ 在 sdkconfig 中添加了 CONFIG_FREERTOS_HEAP_SIZE=196608
## 显示方案（已实现）

### 待机界面（ST7735S 128x160 彩屏，`SetStandbyFullContent()`）

| 区域 | 内容 | 示例 |
|------|------|------|
| 第一行（顶栏） | 日期 + 星期 + 城市 | **06/08 周一  漯河市** |
| 第二行（居中） | 大时间（含秒） | **19:25:30** |
| 第三行（天气行） | 天气 + 风向 + 风力 + 温度 + 湿度 | **多云 西北风2级 20°C 53%** |
| 第四行（底部） | 滚动新闻标题（大红字体） | **1:XXX新闻  2:XXX新闻...**（今日头条API，30分钟更新） |

> **颜色说明**：温度/湿度/风力使用大黄字体（#FFFF00），滚动新闻使用大红字体（#FF0000），序号间停留 0.5 秒。

### 锁屏界面（5分钟无操作后自动进入）

- **黑底大时间**：纯黑背景 + 白色大字体 HH:MM，居中填满屏幕
- **箴言轮播**：红底（#FF0000）+ 黄字（#FFFF00），9 条《毛选》箴言各显示 15 秒 + 时钟显示 15 秒，循环轮播

> **字库说明**：主字体 `font_puhui_16_4` 覆盖常用汉字，但缺少"漯"(U+6F2F)和"颍"(U+988D)等生僻字。通过 LVGL 字体 fallback 机制，后备字体 `font_luohe_char` 自动补全缺失字形，确保"漯河市"等生僻字正常显示。

## 硬件清单

- **开发板**: ESP32-S3-DevKitC-1（选择 WROOM N16R8 模组）
- **数字麦克风**: INMP441 / ICS43434 麦克风模块
- **功放**: MAX98357A / HT517 音频功放模块
- **腔体喇叭**: 8Ω 2~3W 或 4Ω 2~3W 喇叭
- **导线**: 跳线一盒，或杜邦线若干
- **400 孔面包板**: 2 块
- **显示屏**: ST7735S 128x160 彩色 SPI 显示屏（推荐）
- **TF卡模块**: SPI 接口 TF 卡模块（可选）
- **电机驱动模块**: TC1508 / L298N 电机驱动模块
- **电机**: N20 减速电机 x 2 或其他直流电机
- **4G 模块**: ML307R（可选）

## 硬件连接

### ESP32S3 开发板

#### 麦克风 INMP441 / ICS43434（I2S接口）
| 麦克风引脚 | ESP32 引脚 | 说明 |
|-----------|-----------|------|
| WS | GPIO4 | 数据选择 |
| SCK | GPIO5 | 数据时钟 |
| SD | GPIO6 | 数据输出 |
| VDD | 3V3 | 电源正 3.3V |
| GND | GND | 接地（短接 L/R 左/右声道） |

#### 数字功放 MAX98357A
| 功放引脚 | ESP32 引脚 | 说明 |
|---------|-----------|------|
| DIN | GPIO7 | 数字信号 |
| BCLK | GPIO15 | 位时钟 |
| LRC | GPIO16 | 左/右时钟 |
| Vin | 3V3 / 3.3V | 电源输入（短接 SD 关机频道） |
| GND | GND | 接地（短接 GAIN 增益和频道，BGA封装麦克风不用短接） |

#### 喇叭连接
| 喇叭引脚 | 功放引脚 | 说明 |
|---------|---------|------|
| 正极（红线） | 音频+ | 喇叭正极 |
| 负极 | 音频- | 喇叭负极 |

#### 显示屏 ST7735S（SPI 接口）
| 显示屏引脚 | ESP32 引脚 | 说明 |
|-----------|-----------|------|
| SCL | GPIO42 | SPI 时钟 |
| SDA | GPIO41 | SPI 数据 |
| RES | GPIO1 | 复位 |
| DC | GPIO2 | 数据/命令 |
| CS | GPIO20 | 片选 |
| BLK | GPIO19 | 背光 |
| VCC | 3V3 | 电源正 |
| GND | GND | 接地 |

#### TF卡模块（SPI 接口，可选）
| TF卡引脚 | ESP32 引脚 | 说明 |
|---------|-----------|------|
| CS | GPIO3 | 片选 |
| MOSI | GPIO18 | 主出从入 |
| MISO | GPIO17 | 主入从出 |
| SCK | GPIO8 | 时钟 |
| VCC | 3.3V | 电源正 |
| GND | GND | 接地 |

#### 电机驱动模块（TC1508 / L298N）
| 电机驱动引脚 | ESP32 引脚 | 说明 |
|-------------|-----------|------|
| IN1 | GPIO11 | 左前进控制 |
| IN2 | GPIO12 | 左后退控制 |
| IN3 | GPIO13 | 右前进控制 |
| IN4 | GPIO14 | 右后退控制 |
| ENA | 3V3 或 PWM | 左电机使能 |
| ENB | 3V3 或 PWM | 右电机使能 |
| VCC | 5V~12V | 电机电源 |
| GND | GND | 接地（与 ESP32 共地） |

#### 4G 模块（ML307R，已禁用，引脚给摄像头使用）
| 模块引脚 | ESP32 引脚 | 说明 |
|---------|-----------|------|
| LDO 正极（4.7V） | - | 3个脚最右边那个 |
| BAT（3.7~4.5V） | - | 或者 +5V，短接 EN 使能（电压>4.7V） |
| GND | GND | 接地 |
| TXD | NC（已禁用） | 发送数据 |
| RXD | NC（已禁用） | 接收数据 |

#### 按钮（音量控制）
| 按钮功能 | ESP32 引脚 | 说明 |
|---------|-----------|------|
| 音量减 | GPIO39 | 短触按钮（另一头接地 GND），短按减小音量，长按静音（音量0） |
| 音量加 | GPIO40 | 短触按钮（另一头接地 GND），短按加大音量 |

**请注意四角按钮的同向是连通的。**

## 功能特性

### 基础功能
- WiFi 网络连接 + 一键配网（启动时单击 Boot 按钮，或任意状态下长按 Boot 按钮进入配网模式）
- 语音交互（麦克风输入 + 扬声器输出，支持唤醒词触发，对话结束后 30 秒自动待机）
- ST7735S 彩色 SPI 显示屏（128x160）LVGL 驱动，待机界面显示日期、时间（含秒）、天气实况，底部滚动新闻标题（大红字体，今日头条API，30分钟更新），待机背景支持 PNG/GIF 双格式（WEB 页上传，重启自动保留，GIF 仅在待机时播放，对话/听歌/息屏时自动停止省电）。5分钟无操作进入锁屏模式（黑底大时间），含9条《毛选》箴言红底黄字轮播（每条15秒+时钟15秒循环）。内置后备字库，生僻字（漯/颍）正常显示
- 独立 I2S 音频通道（麦克风 INMP441 / 功放 MAX98357A），支持全双工语音交互
- 音量控制按钮（短按 ±10% 屏幕提示当前音量，长按最大/静音）

### 大灯功能（GPIO48 WS2812 RGB LED）
- **打开大灯** — 默认白光，覆盖状态指示颜色
- **关闭大灯** — 恢复设备状态灯（聆听红、对话绿等）
- **设置颜色** — 通过 RGB 参数自定义任意颜色
- **随机变色** — 7 种预设颜色（红/橙/黄/绿/青/蓝/紫）随机切换
- **跑马灯模式** — 7 种颜色以 200ms 步进循环，持续 30 秒（约 21 轮），自动停止

### 提醒 / 闹钟功能
- **倒计时提醒** — 设置 N 分钟/秒后触发，语音播报提醒内容
- **绝对时间闹钟** — 指定 时:分 触发，支持每天重复
- **屏幕弹窗提示** — 提醒触发时屏幕显示"主人该XXX了"，并显示提醒覆盖层
- **打断提醒** — 说"小智关闭提醒"或单击 Boot 按钮停止当前警惕音
- **取消提醒** — 说"小智取消XXX提醒"删除还未触发的提醒任务（支持智能标题匹配）
- **查看提醒** — 说"小智现在有哪些提醒"查看所有待执行的提醒列表
- **设备重启持久化** — 提醒数据保存在 NVS 闪存中，重启后不丢失

### 天气功能
- **和风天气 API** — 自动获取实时天气（漯河市硬编码坐标 33.57,114.08，含温度、天气描述、风向风力、湿度）
- **每小时自动更新** — 待机界面实时显示天气实况
- **启动延迟** — 开机后 20 秒首次获取天气，避免网络未就绪时报错

### 音乐播放功能
- **在线点歌** — 说出"播放XX歌"，设备通过自建多音源聚合服务器（`yl.huhu123.cloud:8774`）搜索并流式下载播放，聚合酷我/网易/腾讯等多家音源，含 4 路服务器自动切换。播放不受中断，无需存储卡
- **播放控制** — 说"小智停止播放"停止当前音乐
- **歌曲信息显示** — 播放时彩屏显示"正在播放：歌手 - 歌曲名"
- **唤醒自动停止** — 唤醒小智时自动停止音乐，不干扰对话
- **持续播放模式** — 说出"音乐模式/本地模式/SD卡模式"自动进入持续播歌模式，不响应唤醒词（免打扰），按 BOOT 键退出。模式切换时 TTS 音频被阻断，仅播音乐
- **SD 卡模式** — 说出"SD卡模式"进入 SD 卡歌曲播歌；"SD卡目录/进入文件夹"可指定 音乐歌曲/国学/儿歌/相声/戏曲/故事 等文件夹随机播放；"SD卡菜单"用音量+/-选择文件夹、BOOT 键确认
- **网络电台** — 说出"播放电台"或指定电台名（如"北京音乐广播"），支持 17 个内置电台在线收听，"停止播放广播"退出
- **儿童模式** — 说出"儿歌模式"进入持续播放模式，从 16 个儿歌搜索词池中随机选歌
- **月薪猫模式** — 说出"月薪猫"进入全屏猫咪跳舞动画 + 循环月薪猫音乐，BOOT 键退出

### 电机控制（TC1508 / L298N 驱动）
- **基础动作** — 前进（4）、后退（2）、左转（3）、右转（1），可指定速度（0-100）与时长（毫秒）
- **组合动作** — 跳舞，前进→左转→右转→后退→前进…共 8 步组合，可指定速度
- **停止** — 方向码 0 立即停止所有电机
- **空闲自动停止** — 对话结束 / 动作完成后自动停止所有电机

## 引脚配置对照表

| 功能 | 引脚 | 配置宏 |
|------|------|--------|
| 麦克风 WS | GPIO4 | AUDIO_I2S_MIC_GPIO_WS |
| 麦克风 SCK | GPIO5 | AUDIO_I2S_MIC_GPIO_SCK |
| 麦克风 DIN | GPIO6 | AUDIO_I2S_MIC_GPIO_DIN |
| 功放 DIN | GPIO7 | AUDIO_I2S_SPK_GPIO_DOUT |
| 功放 BCLK | GPIO15 | AUDIO_I2S_SPK_GPIO_BCLK |
| 功放 LRC | GPIO16 | AUDIO_I2S_SPK_GPIO_LRCK |
| 显示屏 SCL | GPIO42 | DISPLAY_SPI_SCLK |
| 显示屏 SDA | GPIO41 | DISPLAY_SPI_MOSI |
| 显示屏 RES | GPIO1 | DISPLAY_RST |
| 显示屏 DC | GPIO2 | DISPLAY_DC |
| 显示屏 CS | GPIO20 | DISPLAY_SPI_CS |
| 显示屏 BLK | GPIO19 | DISPLAY_BACKLIGHT |
| TF卡 CS | GPIO3 | SDCARD_SPI_CS |
| TF卡 MOSI | GPIO18 | SDCARD_SPI_MOSI |
| TF卡 MISO | GPIO17 | SDCARD_SPI_MISO |
| TF卡 SCK | GPIO8 | SDCARD_SPI_SCLK |
| 音量减按钮 | GPIO39 | VOLUME_DOWN_BUTTON_GPIO |
| 音量加按钮 | GPIO40 | VOLUME_UP_BUTTON_GPIO |
| Boot 按钮 | GPIO0 | BOOT_BUTTON_GPIO |
| 内置 LED | GPIO48 | BUILTIN_LED_GPIO |
| 电机左前进 | GPIO11 | MOTOR_LF_GPIO |
| 电机左后退 | GPIO12 | MOTOR_LB_GPIO |
| 电机右前进 | GPIO13 | MOTOR_RF_GPIO |
| 电机右后退 | GPIO14 | MOTOR_RB_GPIO |
| 4G模块 TXD | NC（已禁用） | ML307_TX_PIN |
| 4G模块 RXD | NC（已禁用） | ML307_RX_PIN |

## MCP 工具列表

### 电机控制工具（语音："小智，让小车前进"）
| 工具名称 | 功能描述 |
|---------|---------|
| motor.forward | 前进（可指定速度和时长） |
| motor.backward | 后退（可指定速度和时长） |
| motor.turn_left | 左转（可指定速度和时长） |
| motor.turn_right | 右转（可指定速度和时长） |
| motor.dance | 跳舞动作（8 步组合） |

### 大灯控制工具（语音："小智，打开大灯"）
| 工具名称 | 功能描述 |
|---------|---------|
| self.lamp.get_state | 获取大灯当前状态（开/关） |
| self.lamp.set_color | 设置大灯 RGB 颜色（参数 red/green/blue 0-255） |
| self.lamp.random_color | 随机切换到 7 种预设颜色之一（红/橙/黄/绿/青/蓝/紫） |
| self.lamp.marquee | 跑马灯模式：7 种颜色循环切换 30 秒，自动停止 |

### 提醒 / 闹钟工具（语音："小智，5分钟后提醒我开会"）
| 工具名称 | 功能描述 |
|---------|---------|
| timer.set_timer | 设置倒计时提醒（参数 minutes/seconds/title） |
| timer.set_alarm | 设置绝对时间闹钟（参数 hour/minute/title/is_repeating） |
| timer.dismiss_alert | 关闭当前正在响的警惕音/闹钟（只关闭警报声和屏幕提醒，不删除其他未执行的提醒） |
| timer.cancel_reminder | 取消一个还未触发的提醒（按标题匹配，支持智能匹配） |
| timer.get_pending | 获取所有待执行的提醒和闹钟列表 |

### 音乐播放工具（语音："小智，播放七里香"）
| 工具名称 | 功能描述 |
|---------|---------|
| self.music.play_song | 播放指定歌曲（参数 song_name 必填，artist_name 可选），在线搜索并流式播放 |
| self.music.stop | 停止当前音乐播放（持续播放模式下需先唤醒小智才能停止） |
| self.radio.play | 播放网络电台（参数 station_name 可选，默认北京音乐广播） |
| self.music.enter_music_mode | 进入音乐模式：连续播放随机网络歌曲，免打扰，BOOT 键退出 |
| self.music.enter_local_mode | 进入本地音乐模式：连续播放板载本地歌曲，免打扰，BOOT 键退出 |
| self.music.enter_sd_mode | 进入 SD 卡音乐模式：连续播放 SD 卡歌曲，免打扰，BOOT 键退出 |
| self.music.enter_sd_folder | 进入 SD 卡文件夹随机播放（参数 folder：音乐歌曲/国学/儿歌/相声/戏曲/故事等） |
| self.music.enter_sd_menu | 进入 SD 卡目录选择菜单（音量+/-选择文件夹，BOOT 键确认） |
| self.music.enter_children_mode | 进入儿歌模式：连续播放随机儿歌（网络搜索），免打扰，BOOT 键退出 |
| self.music.dance_cat | 进入月薪猫模式：全屏猫咪跳舞动画 + 循环月薪猫音乐，BOOT 键退出 |

### 网络工具
| 工具名称 | 功能描述 |
|---------|---------|
| self.network.get_ip | 获取当前 WiFi IP 地址信息 |

### 直播电视工具（语音："小智，看直播" / "看红牛TV"）
| 工具名称 | 功能描述 |
|---------|---------|
| self.tv.play_live | 播放直播电视（参数 channel 可选，默认 cctv1） |
| self.tv.stop | 停止播放直播电视 |

> **支持的频道**：cctv1~cctv15、cctv17（农业农村）、CCTV5+、河南卫视、红牛TV、斗鱼电影3y、时尚一台/二台、法国新闻一/二/三台、Aleph News、印尼新闻（CNA）、中央国际（CGTN 英文/纪录/俄语/法语/西语/阿语）、1983春晚、1987春晚 等。实际以 [ahuhu_board.cc](ahuhu_board.cc) 中 `self.tv.play_live` 的 channel_map 为准。

### 摄像头工具（语音："小智，看摄像头" / "打开监控"）
| 工具名称 | 功能描述 |
|---------|---------|
| camera.set_preview | 打开/关闭摄像头预览（参数 enable：true=开，false=关） |
| camera.view | 显示局域网内 AASXT 摄像头板的实时画面 |
| camera.stop | 停止显示摄像头画面 |

> **架构说明**：板子通过 MJPEG 流连接 PC 端服务器（`mjpeg_server.py`，端口 8081），PC 端用 ffmpeg 拉流转码为 128x160 MJPEG + PCM 音频，板子解码播放。换直播源只需在 PC 端重启服务器并指定新 URL，无需改板子代码。

## 代码文件功能对照表

| 功能 | 实现文件 | 说明 |
|------|---------|------|
| 硬件引脚定义 | [config.h](config.h) | 麦克风/功放/电机/按钮/显示屏/LED 所有 GPIO 定义 |
| 主控逻辑 + MCP 工具注册 | [ahuhu_board.cc](ahuhu_board.cc) | 大灯、电机、音乐播放、提醒/闹钟、音量按钮等所有 MCP 工具的注册和事件处理 |
| 语音交互（I2S 音频） | [no_audio_codec.h](../../audio/codecs/no_audio_codec.h) / [no_audio_codec.cc](../../audio/codecs/no_audio_codec.cc) | 无硬件 Codec 的 I2S 直出：INMP441 麦克风 + MAX98357A 功放 |
| WiFi 配网 | [wifi_board.h](../common/wifi_board.h) / [wifi_board.cc](../common/wifi_board.cc) + [blufi.cpp](../common/blufi.cpp) | 一键配网（Boot 按键触发） |
| 按钮驱动 | [button.h](../common/button.h) | Boot（GPIO0）、音量加（GPIO40）、音量减（GPIO39）底层驱动 |
| 大灯功能（WS2812） | [circular_strip.h](../../led/circular_strip.h) / [circular_strip.cc](../../led/circular_strip.cc) | GPIO48 WS2812 RGB LED，支持跑马灯/随机变色 |
| ST7735S 彩屏 | [lcd_display.h](../../display/lcd_display.h) / [lcd_display.cc](../../display/lcd_display.cc) | ST7735S 128x160 SPI，LVGL 待机界面（日期/时间/天气） |
| 天气功能 | [weather_service.h](weather_service.h) / [weather_service.cc](weather_service.cc) | 和风天气 API，每小时自动更新（漯河市） |
| 新闻服务 | [news_service.h](news_service.h) / [news_service.cc](news_service.cc) | 今日头条 API，30分钟更新，滚动显示 |
| MJPEG 直播 | [mjpeg_player.h](../../mjpeg_player.h) / [mjpeg_player.cc](../../mjpeg_player.cc) | MJPEG 流解码播放，音视频同步 |
| 电机控制核心 | [application.cc](../../application.cc) / [main.cc](../../main.cc) | LEDC PWM 驱动 TC1508 电机模块，队列调度执行 |
| 音乐播放/电台/儿歌 | [esp32_music.h](../common/esp32_music.h) / [esp32_music.cc](../common/esp32_music.cc) | 多音源聚合服务器在线点歌（含酷我/网易/腾讯等）、网络电台、本地/SD/儿歌模式 |
| 定时器/提醒服务 | [timer_service.h](../../timer_service.h) | 倒计时、闹钟、NVS 持久化 |
| MCP 服务器框架 | [mcp_server.h](../../mcp_server.h) / [mcp_server.cc](../../mcp_server.cc) | MCP 协议层，所有工具的注册和调用入口 |

## 构建与烧录

1. 使用 ESP-IDF 工具链进行构建
2. 选择 `ahuhus3` 构建目标
3. 使用 `idf.py flash` 烧录固件

## 操作说明

### 配网模式
- 启动过程中**单击** Boot 按钮进入 WiFi 配网模式
- 任意状态下**长按** Boot 按钮（约 3 秒）进入 WiFi 配网模式
- 配网完成后设备自动连接并保存 WiFi 配置

### 语音交互
- 单击 Boot 按钮开始/结束语音对话
- 如果正在播放提醒警惕音，单击 Boot 按钮关闭提醒后再单击切换对话状态
- 支持唤醒词触发（需在配置中启用）

### 音量控制
- 短按音量加/减按钮调整音量（每次 ±10%）
- 长按音量加按钮设置最大音量
- 长按音量减按钮静音

### 电机控制语音指令示例
- "小智，让小车前进"
- "小智，向左转"
- "小智，跳个舞"
- "小智，停下"

### 大灯控制语音指令示例
- "小智，打开大灯" — 打开白色大灯
- "小智，关闭大灯" — 关闭大灯，恢复状态灯
- "小智，变颜色" — 随机切换 7 种颜色之一（可多次重复）
- "小智，跑马灯" — 启动 30 秒颜色循环模式

### 提醒 / 闹钟语音指令示例
- "小智，5分钟后提醒我开会"
- "小智，设置一个30秒的提醒" — 倒计时提醒
- "小智，设置早上8点的闹钟"
- "小智，关闭提醒" — 停止当前警惕音（不删除未触发的提醒）
- "小智，取消吃饭提醒" — 取消还未触发的"吃饭"提醒
- "小智，现在有哪些提醒" — 查看所有待执行的提醒列表

### 音乐播放语音指令示例
- "小智，播放七里香" — 播放歌曲
- "小智，播放周杰伦的晴天" — 按歌手/歌名搜索播放
- "小智，播放电台" / "小智，播放北京音乐广播" — 播放网络电台
- "小智，停止播放广播" — 停止电台
- "小智，音乐模式" — 持续播歌免打扰（BOOT 键退出）
- "小智，本地模式" / "小智，SD卡模式" / "小智，儿歌模式" / "小智，月薪猫" — 其它持续播放模式
- "小智，停止音乐" — 停止播放
先退出监控器，激活环境，选择ESP32S3板子再编译，每一次编译时都要先这样操作。
编译前先激活ESP-IDF编译环境：cmd /k d:\S3\v5.5.4\esp-idf\export.bat，确认目标芯片设置为 ESP32S3
1- 确认目标芯片设置为 ESP32S3，命令为：idf.py set-target esp32s3
2- 需要更换板子，需要输入idf.py menuconfig，设置正确的板类型为 AHUHUS3
3- 编译命令：idf.py build
4-烧录及监视端口命令：idf.py -p COM3 flash monitor
## 和风天气 API Token 配置
- 在 `idf.py menuconfig` 中进入 `Xiaozhi Assistant` -> `Weather API Token`
- 将默认 `YOUR_REAL_API_TOKEN` 替换为你的真实和风天气 token
- 也可以把 `CONFIG_WEATHER_API_TOKEN` 写入 `sdkconfig.defaults` 或 `sdkconfig` 文件

## Music Server (4 sources, auto-fallback)

> 音乐在线点歌通过**自建多音源聚合服务器**搜索并流式下载播放，支持 4 路服务器自动切换，源内含酷我/网易/腾讯等多家音源聚合。

| Priority | Name | 说明 |
|----------|------|------|
| 1（默认） | `yl.huhu123.cloud:8774` | 多音源聚合服务器，按关键词返回 30 首候选曲目，支持 `lx:kw`（酷我）/`lx:wy`（网易）/`lx:tx`（腾讯）等源标记 |
| 2~4 | NVS 可配置 | 默认 disabled，支持 LAN 网易云 API（30107/34567）等多路备选，启动时自动探测可达服务器并切换 |

> **服务器配置**：在 `idf.py menuconfig` 中进入 `Xiaozhi Assistant` 配置服务器地址，或通过 NVS 保存。播放失败时自动依次尝试下一路可用服务器。

8-导出固件命令：idf.py build --export
9-导出固件路径：d:\S3\esp-idf-main\build\ahuhus3\ahuhus3.bin
在微信开发者工具中导入 ble_remote_app/ 目录
填写自己的 appid（在 project.config.json 中）
          微信小程序 BLE 遥控器

ble_remote_app/
├── app.js                          # 应用入口
├── app.json                        # 应用配置
├── app.wxss                        # 全局样式
├── project.config.json             # 项目配置
├── sitemap.json                    # 搜索规则
└── pages/
    └── index/
        ├── index.js                # 主页面逻辑
        ├── index.wxml              # 主页面布局
        └── index.wxss              # 主页面样式

     界面功能
区域	功能
🟢 顶部状态	连接状态指示灯 + 设备名
📡 设备列表	扫描附近的 BLE 设备（按名称/服务 UUID 过滤）
🎮 快捷控制	唤醒/停止说话/播放音乐/停止音乐/亮屏/关屏/取消闹钟/重启
🔊 音量控制	滑条 + ＋/－按钮 + 静音
🚗 小车控制	方向键（按住持续发送，松手即停）
📋 通信日志	显示收发数据，方便调试
连接 BLE 协议