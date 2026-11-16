// =============================================================================
//  miaomiao-esp32s3 / config.h
//  由固件二进制逆向还原（非原厂源码，原厂未公开）
//  依据：app_ota0.bin 中的符号名、日志字符串、外设 API 调用序列
//  版本：xiaozhi v2.2.2 / IDF v5.5.4 / 编译于 2026-07-18 15:19:34
//
//  ⚠️ 标记 [待确认] 的宏必须实测填写，否则屏幕/摄像头/舵机不会工作。
//     标记 [已确认] 的宏来自二进制，可直接使用。
// =============================================================================
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <esp_codec_dev_defaults.h>   // ES8311_CODEC_DEFAULT_ADDR（本头文件需自包含）

// -----------------------------------------------------------------------------
// 音频：ES8311 I2S 编解码器
// [已确认] 启动日志 Es8311AudioCodec: Duplex channels created / Slave mode
// [已确认] I2S0 引脚经 GPIO 矩阵实测（2026-11-16 JTAG）：
//          MCLK=GPIO3, BCLK=GPIO38, LRCK/WS=GPIO21,
//          DOUT(ESP→codec DSDIN, 喇叭)=GPIO45, DIN(codec ASDOUT→ESP, 麦克风)=GPIO46
// [已确认] codec 控制口 = I2C0：SDA=GPIO1, SCL=GPIO2
// -----------------------------------------------------------------------------
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_CODEC_ES8311_ENCODE

#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_3    // [已确认]
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_21   // [已确认] 与输出共用 LRCK
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_38   // [已确认] 与输出共用 BCLK
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_46   // [已确认] codec ASDOUT
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_45   // [已确认] ESP DOUT → codec DSDIN
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_38   // [已确认]
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_21   // [已确认]

// ES8311 控制口 I2C（[已确认] 二进制含 i2c_new_master_bus + InitializeCodecI2c）
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_1    // [已确认]
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_2    // [已确认]
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR

// 功放使能：全部 GPIO 输出资源中未发现独立 PA 脚（仅 GPIO0 一个空闲 GPIO 输出，
// 分配给屏幕 DC），判断功放常开或由 codec 直驱 —— 实测无声再排查
#define AUDIO_PA_ENABLE_PIN     GPIO_NUM_NC   // [待确认] 可能硬件直通

// -----------------------------------------------------------------------------
// 显示：GC9A01 圆屏 ×2（[已确认] esp_lcd_new_panel_gc9a01 / SPI3_HOST /
//       invert_color=true / mirror(true,false)）
// 分辨率 240×240 —— [已确认] 来自 assets 包 index.json 的 display_config
// 与 emoji GIF 的逻辑屏幕尺寸（1.28" 圆屏标准规格）
// 8-pin 排线 = GND VCC SCL SDA RES DC CS BLK
// -----------------------------------------------------------------------------
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240

#define DISPLAY_SPI_HOST        SPI3_HOST     // [已确认]
#define DISPLAY_SCLK_PIN        GPIO_NUM_41   // [已确认] GPIO 矩阵实测 SPI3 CLK
#define DISPLAY_MOSI_PIN        GPIO_NUM_42   // [已确认] GPIO 矩阵实测 SPI3 MOSI
#define DISPLAY_MISO_PIN        GPIO_NUM_NC   // 一般不用，置 NC

// 两屏并联驱动（同总线同 CS 同 DC，同时写入相同内容——表情双眼同步显示）
#define DISPLAY_LEFT_CS_PIN     GPIO_NUM_47   // [已确认] GPIO 矩阵实测 SPI3 CS0
#define DISPLAY_LEFT_DC_PIN     GPIO_NUM_0    // [高置信推断] 全机唯一空闲 GPIO 输出，
                                              //  空闲电平高（DC 常态），两屏共用
#define DISPLAY_LEFT_RST_PIN    GPIO_NUM_NC   // [待确认] 疑似硬件拉高/RC 复位
#define DISPLAY_LEFT_BL_PIN     GPIO_NUM_NC   // [待确认] 疑似硬件常亮

// 右眼与左眼完全并联
#define DISPLAY_RIGHT_CS_PIN    GPIO_NUM_47   // [已确认] 共用 CS
#define DISPLAY_RIGHT_DC_PIN    GPIO_NUM_0    // [已确认] 共用 DC
#define DISPLAY_RIGHT_RST_PIN   GPIO_NUM_NC   // [待确认]
#define DISPLAY_RIGHT_BL_PIN    GPIO_NUM_NC   // [待确认]

// 背光：未发现控制脚，疑似硬件常亮
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_NC   // [待确认]
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// 像素时钟
#define DISPLAY_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

// -----------------------------------------------------------------------------
// 摄像头：DVP + OV3660
// [已确认] 2026-11-16 JTAG GPIO 矩阵实测完整 DVP 路由：
//   XCLK=GPIO15(输出), SCCB=I2C1(SDA=GPIO4, SCL=GPIO5),
//   D0-D7={11,9,8,10,12,18,17,16}, PCLK=GPIO13, HREF=GPIO7, VSYNC=GPIO6
// [待确认] PWDN/RESET：未发现控制脚，疑似硬件直通
// -----------------------------------------------------------------------------
#define CAMERA_PIN_PWDN    GPIO_NUM_NC        // [待确认] 疑似硬件直通
#define CAMERA_PIN_RESET   GPIO_NUM_NC        // [待确认] 疑似硬件直通
#define CAMERA_PIN_XCLK    GPIO_NUM_15        // [已确认]
#define CAMERA_PIN_SIOD    GPIO_NUM_4         // [已确认] I2C1 SDA
#define CAMERA_PIN_SIOC    GPIO_NUM_5         // [已确认] I2C1 SCL

#define CAMERA_PIN_D7      GPIO_NUM_16        // [已确认]
#define CAMERA_PIN_D6      GPIO_NUM_17        // [已确认]
#define CAMERA_PIN_D5      GPIO_NUM_18        // [已确认]
#define CAMERA_PIN_D4      GPIO_NUM_12        // [已确认]
#define CAMERA_PIN_D3      GPIO_NUM_10        // [已确认]
#define CAMERA_PIN_D2      GPIO_NUM_8         // [已确认]
#define CAMERA_PIN_D1      GPIO_NUM_9         // [已确认]
#define CAMERA_PIN_D0      GPIO_NUM_11        // [已确认]
#define CAMERA_PIN_VSYNC   GPIO_NUM_6         // [已确认]
#define CAMERA_PIN_HREF    GPIO_NUM_7         // [已确认]
#define CAMERA_PIN_PCLK    GPIO_NUM_13        // [已确认]

#define CAMERA_XCLK_FREQ_HZ 20000000

// -----------------------------------------------------------------------------
// 舵机：SG90 ×2，LEDC 低速模式，共用 TIMER0
// [已确认] 启动日志 "SG90: 舵机初始化成功 - GPIO:39, 通道:0, 定时器:0" / "GPIO:40, 通道:1"
// [已确认] 2026-11-16 JTAG 读回 vendor 定时器实配：APB 80MHz / CLK_DIV=50000(195.3125) /
//          13bit = 精确 50.00Hz；CH0/CH1 SIG_OUT_EN 常开，DUTY 持续输出
// [已验证] 2026-11-16 JTAG 在线改写 DUTY 成功驱动舵机扫掠（写后 vendor 任务脱力归零，
//          证明写入真实生效）
// [已确认] CN7 / CN8 为三线舵机口（橙=信号 红=V+ 棕=GND），5V 独立升压供电
// -----------------------------------------------------------------------------
#define SERVO_COUNT            2

#define SERVO_LEFT_GPIO        GPIO_NUM_39    // [已确认] CN7 信号线（LEDC 通道0）
#define SERVO_RIGHT_GPIO       GPIO_NUM_40    // [已确认] CN8 信号线（LEDC 通道1）

#define SERVO_LEDC_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_TIMER       LEDC_TIMER_0   // [已确认] 固件日志 + 通道寄存器 TIMER_SEL=0
#define SERVO_LEDC_FREQ_HZ     50             // [已确认] 80e6/(50000/256*8192)=50.00Hz
#define SERVO_LEDC_RESOLUTION  LEDC_TIMER_13_BIT  // [已确认] DUTY_RES=13

// 脉宽标定：SG90 数据手册 500–2500us 对应 0–180°
// 实机如出现两端抖动或撞限位，用 servo_calibrate 微调这两个值
#define SERVO_MIN_PULSE_US     500
#define SERVO_MAX_PULSE_US     2500
#define SERVO_MIN_ANGLE        0
#define SERVO_MAX_ANGLE        180
#define SERVO_CENTER_ANGLE     90

// 连续转动模式（改装版）：拆掉舵机限位挡片后，脉宽语义变为
//   1500us=停止，偏移越大转速越快，<1500us=反转(后退) >1500us=正转(前进)
// 运行时可用 self.servo.mode 工具逐个切换，无需重刷固件
#define SERVO_CONT_STOP_PULSE_US   1500   // 停止/零速脉宽（需实测微调）
#define SERVO_CONT_SPEED_SPAN_US   500    // 全速偏移量（500us≈全速）

// -----------------------------------------------------------------------------
// 按键与电池
// [已确认] 2026-11-16 固件字符串 "Exit MAC address screen by button 43/44"
//          + GPIO_IN1 实测 GPIO43/44 空闲均为高（上拉）→ 双按键 GPIO43/44
// [已确认] 电池采样 ADC 通道 3；ADC1_CH3=GPIO4 已被 I2C1 占用，
//          推断实际为 ADC2_CH3=GPIO14（全机唯一未分配引脚）—— 需实测校准
// -----------------------------------------------------------------------------
#define BOOT_BUTTON_GPIO        GPIO_NUM_43   // [已确认] 按键1（长按显示 MAC）
#define SECOND_BUTTON_GPIO      GPIO_NUM_44   // [已确认] 按键2

#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_3 // [已确认] 通道号
#define BATTERY_ADC_GPIO        GPIO_NUM_14   // [高置信推断] ADC2_CH3，需实测
#define BATTERY_ADC_UNIT        ADC_UNIT_2    // [推断] 与 GPIO14 对应
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
// 分压比：锂电 4.2V 经 1/2 分压进 ADC（常见设计），实测校准
#define BATTERY_VOLTAGE_RATIO   2.0f          // [待确认]
#define BATTERY_FULL_MV         4200
#define BATTERY_EMPTY_MV        3300
#define BATTERY_CRITICAL_PERCENT 5            // [已确认] 低于此值进 deep sleep

#endif // _BOARD_CONFIG_H_
