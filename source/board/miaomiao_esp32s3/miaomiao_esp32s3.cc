// =============================================================================
//  miaomiao_esp32s3.cc  —  喵喵机芯盒 ESP32-S3 开源板型主类
//
//  依据固件二进制逆向还原（vendor 源码未公开，本文件为净室重写）：
//   - xiaozhi v2.2.2 / IDF v5.5.4
//   - 引脚全部经 JTAG GPIO 矩阵实测（见 config.h）
//   - 硬件：ESP32-S3-WROOM-1-N16R8 / ES8311 codec / GC9A01 双圆屏(并联) /
//           OV3660 DVP 摄像头 / SG90 舵机×2 / 双按键 / 电池 ADC
//
//  相比原厂固件新增（本目录 mcp_servo_tools.cc + mcp_display_tools.cc）：
//   - self.servo.set / action / drive / mode / get → 语音控舵机
//   - self.display.emotion / emotions              → 语音控表情
//
//  显示/相机初始化写法对照官方板型：
//   - movecall-moji-esp32s3（GC9A01 圆屏 + ES8311）
//   - bread-compact-wifi-s3cam（DVP 摄像头）
// =============================================================================

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>

#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
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
    Display* display_ = nullptr;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    adc_oneshot_unit_handle_t battery_adc_handle_ = nullptr;
    adc_cali_handle_t battery_cali_handle_ = nullptr;
    static constexpr int kBatterySampleCount = 8;
    int battery_mv_samples_[kBatterySampleCount] = {0};  // 滑动平均滤波（对齐原厂 filtered）
    int battery_mv_idx_ = 0;
    int battery_mv_count_ = 0;

    void InitializeCodecI2c() {
        // [已确认] codec 控制口 I2C0：SDA=GPIO1, SCL=GPIO2（JTAG 实测）
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        // [已确认] SPI3_HOST: CLK=GPIO41, MOSI=GPIO42（两屏并联共用）
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MISO_PIN;      // GPIO_NUM_NC
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeGc9a01Display() {
        // [已确认] 两屏并联：同总线同 CS(GPIO47) 同 DC(GPIO48)，RST=GPIO0，同步显示
        esp_lcd_panel_io_handle_t io_handle = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_LEFT_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_LEFT_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_PIXEL_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &io_handle));

        esp_lcd_panel_handle_t panel_handle = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_LEFT_RST_PIN;   // GPIO_NUM_0（vendor 反汇编实锤：GPIO0 作硬件复位线）
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));   // [已确认] 固件 API 序列
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));  // [已确认]
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        // [屏幕诊断] 绕过 LVGL 直接刷白屏 3 秒：白屏=panel/SPI/DC 链路通，问题在渲染层；黑屏=面板初始化问题
        {
            const int kRows = 16;
            uint16_t* line = (uint16_t*)heap_caps_malloc(DISPLAY_WIDTH * kRows * sizeof(uint16_t), MALLOC_CAP_DMA);
            if (line) {
                for (int i = 0; i < DISPLAY_WIDTH * kRows; i++) line[i] = 0xFFFF;
                for (int y = 0; y < DISPLAY_HEIGHT; y += kRows) {
                    esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + kRows, line);
                }
                ESP_LOGI(TAG, "[屏幕测试] 白屏已发送，3 秒后交还 LVGL");
                vTaskDelay(pdMS_TO_TICKS(3000));
                heap_caps_free(line);
            } else {
                ESP_LOGE(TAG, "[屏幕测试] DMA 缓冲分配失败");
            }
        }

        display_ = new SpiLcdDisplay(io_handle, panel_handle,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     0, 0,                 // offset_x / offset_y
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        // [已确认] 双按键 GPIO43/44（固件字符串 + 电平实测）
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            // 与原厂一致：长按显示 MAC 地址界面
            auto display = GetDisplay();
            if (display) {
                display->SetStatus(("MAC: " + MacAddressString()).c_str());
            }
        });
        second_button_.OnClick([this]() {
            Application::GetInstance().ToggleChatState();
        });
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
        : boot_button_(BOOT_BUTTON_GPIO),
          second_button_(SECOND_BUTTON_GPIO) {
        InitializeCodecI2c();
        GetCamera();            // 摄像头修复三件套之二：预创建先于显示初始化
                                //（vendor 日志铁证：cam init 先于显示，PSRAM frame buffer 先占坑）
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        InitializeServo();
        InitializeTools();
        // 背光疑似硬件常亮（DISPLAY_BACKLIGHT_PIN=NC），无需 RestoreBrightness
    }

    virtual std::string GetBoardType() override { return "miaomiao-esp32s3"; }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        // 背光无控制脚，疑似硬件常亮
        return nullptr;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // [已确认] ES8311 duplex：双工模式下输入输出必须同为 24kHz（官方 v2.2.2 断言要求）
        // 双工：BCLK(GPIO38)/WS(GPIO21) 收发共用，DOUT=45(喇叭) DIN=46(麦克风)
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,          // GPIO3
            AUDIO_I2S_SPK_GPIO_BCLK,      // GPIO38
            AUDIO_I2S_SPK_GPIO_LRCK,      // GPIO21
            AUDIO_I2S_SPK_GPIO_DOUT,      // GPIO45
            AUDIO_I2S_MIC_GPIO_DIN,       // GPIO46
            AUDIO_PA_ENABLE_PIN,          // GPIO_NUM_NC（功放疑似硬件直通）
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    camera_config_t CameraConfig() {
        // [已确认] 完整 DVP 引脚图（JTAG GPIO 矩阵实测），传感器 OV3660
        camera_config_t cfg = {};
        cfg.ledc_channel = LEDC_CHANNEL_2;      // 舵机占了 CH0/CH1，相机 XCLK 用 CH2
        cfg.ledc_timer   = LEDC_TIMER_1;        // 舵机占 TIMER0
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
        cfg.pixel_format = PIXFORMAT_RGB565;
        cfg.frame_size   = FRAMESIZE_QVGA;
        cfg.fb_location  = CAMERA_FB_IN_PSRAM;  // N16R8 有 8MB PSRAM
        cfg.grab_mode    = CAMERA_GRAB_LATEST;
        cfg.fb_count     = 2;   // 摄像头修复三件套之三：零初始化漏设会导致
                                // cam_hal frame_cnt=0 → calloc(0)=NULL → "frames malloc failed"
        return cfg;
    }

    virtual Camera* GetCamera() override {
        // 注册后官方 AddCommonTools 自带 self.camera.take_photo（拍照识物）
        static Esp32Camera camera(CameraConfig());
        return &camera;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        // [2026-11-19 修复] 对齐原厂 BatterygetVoltage()：ADC1_CH3 + curve_fitting 校准
        // + 滑动平均滤波。此前 ADC2 与 WiFi 互斥、读取恒失败且未查返回值 → 电量恒 0%。
        if (battery_adc_handle_ == nullptr) {
            adc_oneshot_unit_init_cfg_t init_cfg = {};
            init_cfg.unit_id = ADC_UNIT_1;   // [已确认] 原厂用 ADC1（ADC2 被 WiFi 占用）
            ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &battery_adc_handle_));
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten = BATTERY_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ESP_ERROR_CHECK(adc_oneshot_config_channel(
                battery_adc_handle_, BATTERY_ADC_CHANNEL, &chan_cfg));
            adc_cali_curve_fitting_config_t cali_cfg = {
                .unit_id = ADC_UNIT_1,
                .atten = BATTERY_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &battery_cali_handle_));
        }
        int raw = 0, mv = 0;
        if (adc_oneshot_read(battery_adc_handle_, BATTERY_ADC_CHANNEL, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(battery_cali_handle_, raw, &mv) != ESP_OK) {
            ESP_LOGW(TAG, "Battery ADC read failed, skip update");
            return false;   // 读取失败不上报，避免显示假电量
        }
        battery_mv_samples_[battery_mv_idx_++] = mv;
        if (battery_mv_idx_ >= kBatterySampleCount) battery_mv_idx_ = 0;
        if (battery_mv_count_ < kBatterySampleCount) battery_mv_count_++;
        int64_t sum = 0;
        for (int i = 0; i < battery_mv_count_; i++) sum += battery_mv_samples_[i];
        int stable_mv = static_cast<int>(sum / battery_mv_count_);
        int batt_mv = static_cast<int>(stable_mv * BATTERY_VOLTAGE_RATIO);   // 分压还原电池总电压
        charging = false;
        discharging = true;
        level = 100 * (batt_mv - BATTERY_EMPTY_MV) / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        ESP_LOGI(TAG, "Battery pin=%dmV batt=%dmV level=%d%%", stable_mv, batt_mv, level);
        return true;
    }
};

DECLARE_BOARD(MiaoMiaoESP32S3);
