// =============================================================================
//  miaomiao_esp32s3.cc  —  喵喵机芯盒 ESP32-S3 开源板型主类
//
//  依据固件二进制逆向还原（vendor 源码未公开，本文件为净室重写）：
//   - xiaozhi v2.2.2 / IDF v5.5.4
//   - 引脚全部经 JTAG GPIO 矩阵实测（见 config.h 与 引脚反推工作表.html）
//   - 硬件：ESP32-S3-WROOM-1-N16R8 / ES8311 codec / GC9A01 双圆屏(并联) /
//           OV3660 DVP 摄像头 / SG90 舵机×2 / 双按键 / 电池 ADC
//
//  相比原厂固件新增（本目录 mcp_servo_tools.cc + mcp_display_tools.cc）：
//   - self.servo.set / self.servo.action / self.servo.get   → 语音控舵机角度+时长
//   - self.display.emotion / self.display.emotions          → 语音控表情
//
//  [编译核对] 标记处为对 xiaozhi v2.2.2 API 语义的还原，编译报错时优先核对这些行
//  （不同小版本接口签名可能有差异），改动仅需 1-2 行。
// =============================================================================

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>

#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display.h"
#include "lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets.h"
#include "esp32_camera.h"
#include "mcp_server.h"

#include "servo_controller.h"

extern void RegisterServoTools();
extern void RegisterDisplayTools();

#define TAG "MiaoMiaoBoard"

class MiaoMiaoESP32S3 : public WifiBoard {
private:
    Button boot_button_;
    Button second_button_;
    LcdDisplay* display_ = nullptr;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    adc_oneshot_unit_handle_t battery_adc_handle_ = nullptr;

    void InitializeCodecI2c() {
        // [已确认] codec 控制口 I2C0：SDA=GPIO1, SCL=GPIO2（JTAG 实测）
        i2c_master_bus_config_t i2c_bus_config = {};
        i2c_bus_config.i2c_port = 0;
        i2c_bus_config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_config.clk_flags = I2C_MASTER_CLK_CTRL_DEFAULT_BIT;
        i2c_bus_config.sda_pullup_en = GPIO_PULLUP_ENABLE;
        i2c_bus_config.scl_pullup_en = GPIO_PULLUP_ENABLE;
        i2c_bus_config.master.clk_speed = 100000;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &codec_i2c_bus_));
    }

    void InitializeButtons() {
        // [已确认] 双按键 GPIO43/44（固件字符串 + 电平实测）
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnLongPress([this]() {
            // 与原厂一致：长按显示 MAC 地址界面（GetSystemInfoJson 已含 MAC）
            auto display = GetDisplay();
            if (display) {
                display->SetStatus("MAC: " + MacAddressString());
            }
        });
        second_button_.OnClick([this]() {
            Application::GetInstance().ToggleChatState();
        });
    }

    void InitializeDisplay() {
        // [已确认] SPI3_HOST: CLK=GPIO41, MOSI=GPIO42, CS=GPIO47（两屏并联共用）
        // [高置信推断] DC=GPIO0；RST/BL 疑似硬件直通（见 config.h 注释）
        auto& assets = Assets::GetInstance();
        auto display_config = assets.GetDisplayConfig();  // [编译核对] index.json 的 240x240

        SpiLcdDisplay* lcd_display = new SpiLcdDisplay(
            DISPLAY_SPI_HOST,
            DISPLAY_LEFT_CS_PIN,
            DISPLAY_LEFT_DC_PIN,
            DISPLAY_SCLK_PIN,
            DISPLAY_MOSI_PIN,
            DISPLAY_MISO_PIN,
            DISPLAY_BACKLIGHT_PIN,
            DISPLAY_BACKLIGHT_OUTPUT_INVERT,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X,           // [编译核对] v2.2.2 若无此参数则删去两行
            DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X,
            DISPLAY_MIRROR_Y,
            DISPLAY_SWAP_XY,
            assets.GetDisplayFonts().text_font,        // [编译核对] 字体来自 assets
            assets.GetDisplayFonts().icon_font,
            assets.GetDisplayFonts().emoji_font);
        display_ = lcd_display;
    }

    void InitializeServo() {
        // 独立任务初始化，失败不影响主流程（原厂固件日志行为一致）
        xTaskCreate([](void* arg) {
            auto& servo = ServoController::GetInstance();
            if (servo.Initialize()) {
                // 开机回中，与原厂"舵机初始化成功"后的行为一致
                servo.MoveTo(-1, SERVO_CENTER_ANGLE, 600);
            }
            vTaskDelete(nullptr);
        }, "ServoTask", 4096, nullptr, 3, nullptr);
    }

    void InitializeTools() {
        // McpServer::AddTool 仅登记工具表，实际启动在协议层之后，构造期注册安全
        RegisterServoTools();
        RegisterDisplayTools();
    }

    std::string MacAddressString() {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char buf[18];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return std::string(buf);
    }

public:
    MiaoMiaoESP32S3()
        : boot_button_(BOOT_BUTTON_GPIO, false, 0, false),
          second_button_(SECOND_BUTTON_GPIO, false, 0, false) {   // [编译核对] Button 构造参数
        InitializeCodecI2c();
        InitializeDisplay();
        InitializeButtons();
        InitializeServo();
        InitializeTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual std::string GetBoardType() override { return "miaomiao-esp32s3"; }

    virtual AudioCodec* GetAudioCodec() override {
        // [已确认] ES8311 duplex：16kHz in / 24kHz out（与原厂日志一致）
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN,
            AUDIO_CODEC_ES8311_ADDR);          // [编译核对] 对照 es8311_audio_codec.h 签名
        return &audio_codec;
    }

    camera_config_t CameraConfig() {
        // [已确认] 完整 DVP 引脚图（JTAG GPIO 矩阵实测），传感器 OV3660
        camera_config_t cfg = {};
        cfg.ledc_channel = LEDC_CHANNEL_2;      // [编译核对] 舵机占了 CH0/CH1，相机 XCLK 用 CH2
        cfg.ledc_timer   = LEDC_TIMER_1;        // [编译核对] 舵机占 TIMER0
        cfg.pin_pwdn     = CAMERA_PIN_PWDN;
        cfg.pin_reset    = CAMERA_PIN_RESET;
        cfg.pin_xclk     = CAMERA_PIN_XCLK;
        cfg.pin_sccb_sda = CAMERA_PIN_SIOD;
        cfg.pin_sccb_scl = CAMERA_PIN_SIOC;
        cfg.pin_d7 = CAMERA_PIN_D7;  cfg.pin_d6 = CAMERA_PIN_D6;
        cfg.pin_d5 = CAMERA_PIN_D5;  cfg.pin_d4 = CAMERA_PIN_D4;
        cfg.pin_d3 = CAMERA_PIN_D3;  cfg.pin_d2 = CAMERA_PIN_D2;
        cfg.pin_d1 = CAMERA_PIN_D1;  cfg.pin_d0 = CAMERA_PIN_D0;
        cfg.pin_vsync = CAMERA_PIN_VSYNC;
        cfg.pin_href  = CAMERA_PIN_HREF;
        cfg.pin_pclk  = CAMERA_PIN_PCLK;
        cfg.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
        cfg.pixel_format = PIXFORMAT_RGB565;    // 表情屏场景用小帧率，按需调
        cfg.frame_size   = FRAMESIZE_QVGA;
        cfg.fb_location  = CAMERA_FB_IN_PSRAM;  // N16R8 有 8MB PSRAM
        cfg.grab_mode    = CAMERA_GRAB_LATEST;
        return cfg;
    }

    virtual Camera* GetCamera() override {
        static Esp32Camera camera(CameraConfig());
        return &camera;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        // [已确认] ADC 通道 3；GPIO 推断 GPIO14，分压比待实测校准
        if (battery_adc_handle_ == nullptr) {
            adc_oneshot_unit_init_cfg_t init_cfg = {};
            init_cfg.unit_id = BATTERY_ADC_UNIT;          // [编译核对] GPIO14→ADC_UNIT_2
            ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &battery_adc_handle_));
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten = BATTERY_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ESP_ERROR_CHECK(adc_oneshot_config_channel(
                battery_adc_handle_, BATTERY_ADC_CHANNEL, &chan_cfg));
        }
        int raw = 0;
        adc_oneshot_read(battery_adc_handle_, BATTERY_ADC_CHANNEL, &raw);
        int mv = raw * BATTERY_VOLTAGE_RATIO;   // 简化标定；精确值用 adc_cali
        charging = false;
        discharging = false;
        level = 100 * (mv - BATTERY_EMPTY_MV) / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        return true;
    }
};

DECLARE_BOARD(MiaoMiaoESP32S3);
