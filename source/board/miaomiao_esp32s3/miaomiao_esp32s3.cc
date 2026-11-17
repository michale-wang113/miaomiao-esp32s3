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
#include <esp_cpu.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_adc/adc_oneshot.h>
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

// ============================================================
// [Debug] GPIO0(DC) 输出使能哨兵 — 排查屏幕乱码（DC 被人改成输入）
//   EN0  = GPIO_ENABLE_REG    @0x60004020 (bit0 = GPIO0 输出使能)
//   W1TC = GPIO_ENABLE_W1TC   @0x6000402C (写 1 清除输出使能)
//   MUX  = IO_MUX GPIO0       @0x60009004
// ============================================================
static uint32_t RegRd32(uint32_t addr) { return *(volatile uint32_t*)addr; }

static void LogGpio0State(const char* where) {
    uint32_t en0 = RegRd32(0x60004020);
    uint32_t mux = RegRd32(0x60009004);
    ESP_LOGI("Board", "[哨兵] %-14s EN0=0x%08x bit0=%d MUX0=0x%08x", where, en0, (int)(en0 & 1), mux);
}

static void Gpio0WatchpointTask(void* arg) {
    // 对 GPIO_ENABLE_W1TC 的任何写 = 有人在关 0-31 号脚的输出使能 → panic 留下调用栈
    esp_err_t err = esp_cpu_set_watchpoint(0, (const void*)0x6000402C, 4, ESP_CPU_WATCHPOINT_STORE);
    ESP_LOGI("Board", "[哨兵] CPU%d 观察点安装: %s", (int)xPortGetCoreID(), esp_err_to_name(err));
    vTaskDelete(nullptr);
}

static void Gpio0SentinelTask(void* arg) {
    int last = -1;
    uint32_t last_mux = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (xTaskGetTickCount() < deadline) {
        int bit0 = (int)(RegRd32(0x60004020) & 1);
        uint32_t mux = RegRd32(0x60009004);
        if (last >= 0 && (bit0 != last || mux != last_mux)) {
            ESP_LOGW("Board", "[哨兵] GPIO0 状态翻转! bit0 %d->%d MUX0 0x%08x->0x%08x", last, bit0, last_mux, mux);
        }
        last = bit0;
        last_mux = mux;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI("Board", "[哨兵] 15 秒监视结束");
    vTaskDelete(nullptr);
}
extern void RegisterDisplayTools();

#define TAG "MiaoMiaoBoard"

class MiaoMiaoESP32S3 : public WifiBoard {
private:
    Button boot_button_;
    Button second_button_;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    adc_oneshot_unit_handle_t battery_adc_handle_ = nullptr;

    // —— 语音备忘录（双按键：按一下录音，再按一下回放）——
    std::atomic<bool> memo_recording_{false};
    std::atomic<bool> memo_playing_{false};
    std::atomic<bool> memo_stop_{false};
    int16_t* memo_buffer_ = nullptr;                       // PSRAM
    size_t memo_len_ = 0;                                  // 已录采样数
    static constexpr size_t kMemoSampleRate = 24000;       // 与 codec 输入/输出同率，免重采样
    static constexpr size_t kMemoMaxSeconds = 60;
    static constexpr size_t kMemoMaxSamples = kMemoSampleRate * kMemoMaxSeconds;

    void PlayTone(int freq_hz, int ms, int amp = 6000) {
        auto codec = GetAudioCodec();
        codec->EnableOutput(true);
        vTaskDelay(pdMS_TO_TICKS(30));
        const int kChunk = 240;
        int total = (int)kMemoSampleRate * ms / 1000;
        for (int i = 0; i < total; i += kChunk) {
            std::vector<int16_t> seg;
            seg.reserve(kChunk);
            for (int j = 0; j < kChunk && i + j < total; j++) {
                int n = i + j;
                float env = std::min(1.0f, std::min((float)n / (total * 0.15f),
                                                    (float)(total - n) / (total * 0.15f)));
                seg.push_back((int16_t)(amp * env * sinf(2.0f * (float)M_PI * freq_hz * n / kMemoSampleRate)));
            }
            codec->OutputData(seg);
        }
    }

    void VoiceMemoTask() {
        auto& audio = Application::GetInstance().GetAudioService();
        auto codec = GetAudioCodec();

        // 录音期间暂停唤醒词检测，独占麦克风输入
        audio.EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(120));

        // 提示音：一声短哔 = 开始录音
        PlayTone(880, 150);

        size_t silence_frames = 0;
        std::vector<int16_t> chunk;
        memo_len_ = 0;
        while (memo_len_ < kMemoMaxSamples) {
            chunk.assign(1024, 0);
            if (!audio.ReadAudioData(chunk, kMemoSampleRate, 1024)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            memcpy(memo_buffer_ + memo_len_, chunk.data(),
                   std::min(chunk.size(), kMemoMaxSamples - memo_len_) * sizeof(int16_t));
            memo_len_ += std::min(chunk.size(), kMemoMaxSamples - memo_len_);
            if (memo_stop_.load()) break;
            // 静音自动截断：连续 3 秒无声提前结束录音
            int64_t energy = 0;
            for (size_t i = 0; i < chunk.size(); i += 16) energy += abs(chunk[i]);
            if (energy / (chunk.size() / 16 + 1) < 60) {
                if (++silence_frames >= kMemoSampleRate / 1024 * 3) break;
            } else {
                silence_frames = 0;
            }
        }
        memo_recording_ = false;

        ESP_LOGI(TAG, "[备忘录] 录音结束：%.1f 秒，开始回放", (float)memo_len_ / kMemoSampleRate);
        vTaskDelay(pdMS_TO_TICKS(150));

        // 回放
        memo_playing_ = true;
        codec->EnableOutput(true);
        vTaskDelay(pdMS_TO_TICKS(50));
        const size_t kPlayChunk = 960;
        for (size_t pos = 0; pos < memo_len_; pos += kPlayChunk) {
            size_t n = std::min(kPlayChunk, memo_len_ - pos);
            std::vector<int16_t> out(memo_buffer_ + pos, memo_buffer_ + pos + n);
            codec->OutputData(out);
        }
        // 结束提示音：两声短哔
        PlayTone(1320, 120);
        PlayTone(1320, 120);
        codec->EnableOutput(false);
        memo_playing_ = false;

        // 恢复唤醒词
        audio.EnableWakeWordDetection(true);
        ESP_LOGI(TAG, "[备忘录] 回放完成");
        vTaskDelete(nullptr);
    }

    static void VoiceMemoTaskEntry(void* arg) {
        static_cast<MiaoMiaoESP32S3*>(arg)->VoiceMemoTask();
    }

    void ToggleVoiceMemo() {
        if (memo_recording_.load()) {
            // 第二次按下：停止录音 → 回放
            memo_stop_ = true;
            return;
        }
        if (memo_playing_.load()) {
            return;  // 回放中忽略按键
        }
        // 空闲状态：开始录音
        if (!memo_buffer_) {
            memo_buffer_ = (int16_t*)heap_caps_malloc(
                kMemoMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!memo_buffer_) {
                ESP_LOGE(TAG, "[备忘录] PSRAM 缓冲分配失败（%d 字节）", (int)(kMemoMaxSamples * 2));
                return;
            }
        }
        memo_stop_ = false;
        memo_recording_ = true;
        ESP_LOGI(TAG, "[备忘录] 开始录音（GPIO43/44 再按一次回放，最长 60 秒）");
        if (xTaskCreate(&MiaoMiaoESP32S3::VoiceMemoTaskEntry, "VoiceMemo", 6144, this, 4, nullptr) != pdPASS) {
            memo_recording_ = false;
        }
    }

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
        // [Debug] 双核各装 GPIO_ENABLE_W1TC 写观察点 + 1ms 哨兵任务
        xTaskCreatePinnedToCore(Gpio0WatchpointTask, "wp0", 3072, nullptr, 20, nullptr, 0);
        xTaskCreatePinnedToCore(Gpio0WatchpointTask, "wp1", 3072, nullptr, 20, nullptr, 1);
        xTaskCreate(Gpio0SentinelTask, "gpio0sent", 4096, nullptr, 15, nullptr);

        // [已确认] 两屏并联：同总线同 CS(GPIO47) 同 DC(GPIO0)，同步显示
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
        LogGpio0State("panel_io后");

        esp_lcd_panel_handle_t panel_handle = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_LEFT_RST_PIN;   // GPIO_NUM_NC（疑似硬件复位）
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));   // [已确认] 固件 API 序列
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));  // [已确认]
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
        LogGpio0State("panel_init后");

        // [屏幕对照实验] 绕过 LVGL 直接刷白屏 3 秒：
        // 白屏可见 = panel/SPI/DC 写入链路通 → 黑屏问题在渲染层；
        // 白屏不可见 = 本固件配置下 SPI 写入到不了面板 → 配置回归
        {
            const int kRows = 16;
            uint16_t* line = (uint16_t*)heap_caps_malloc(DISPLAY_WIDTH * kRows * sizeof(uint16_t), MALLOC_CAP_DMA);
            if (line) {
                for (int i = 0; i < DISPLAY_WIDTH * kRows; i++) line[i] = 0xFFFF;
                for (int y = 0; y < DISPLAY_HEIGHT; y += kRows) {
                    esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISPLAY_WIDTH, y + kRows, line);
                }
                ESP_LOGI(TAG, "[屏幕测试] 白屏已发送，3 秒后交还 LVGL");
                LogGpio0State("白屏后");
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
        LogGpio0State("SpiLcdDisplay后");
    }

    void InitializeButtons() {
        // [已确认] 双按键 GPIO43/44（固件字符串 + 电平实测）
        // 新功能：按一下录音，再按一下回放（语音备忘录）
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (app.GetDeviceState() != kDeviceStateIdle) {
                // 对话/播放等非空闲状态：先退出当前状态
                app.ToggleChatState();
                return;
            }
            ToggleVoiceMemo();
        });
        boot_button_.OnLongPress([this]() {
            // 与原厂一致：长按显示 MAC 地址界面
            auto display = GetDisplay();
            if (display) {
                display->SetStatus(("MAC: " + MacAddressString()).c_str());
            }
        });
        second_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle) {
                app.ToggleChatState();
                return;
            }
            ToggleVoiceMemo();
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
        // [屏幕回退实验] 摄像头恢复懒加载（屏幕正常时的原顺序）；fb_count=2 已修复分配问题，
        // 预初始化不再必要。若屏幕恢复 → 嫌疑锁定为摄像头预初始化/PSRAM_DMA。
        ESP_LOGI(TAG, "[内存] 显示初始化前 largest DMA=%u free DMA=%u PSRAM free=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        LogGpio0State("按键后");
        InitializeServo();
        LogGpio0State("舵机后");
        InitializeTools();
        LogGpio0State("工具后");
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
        cfg.fb_count     = 2;                   // [关键修复] 缺省为 0 会导致 frame_cnt=0，calloc(0)=NULL → frames malloc failed
        cfg.fb_location  = CAMERA_FB_IN_PSRAM;  // N16R8 有 8MB PSRAM
        cfg.grab_mode    = CAMERA_GRAB_LATEST;
        return cfg;
    }

    virtual Camera* GetCamera() override {
        // 注册后官方 AddCommonTools 自带 self.camera.take_photo（拍照识物）
        static Esp32Camera camera(CameraConfig());
        return &camera;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        // [已确认] ADC 通道 3；GPIO 推断 GPIO14，分压比待实测校准
        if (battery_adc_handle_ == nullptr) {
            adc_oneshot_unit_init_cfg_t init_cfg = {};
            init_cfg.unit_id = BATTERY_ADC_UNIT;
            ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &battery_adc_handle_));
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten = BATTERY_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ESP_ERROR_CHECK(adc_oneshot_config_channel(
                battery_adc_handle_, BATTERY_ADC_CHANNEL, &chan_cfg));
        }
        int raw = 0;
        adc_oneshot_read(battery_adc_handle_, BATTERY_ADC_CHANNEL, &raw);
        int mv = static_cast<int>(raw * BATTERY_VOLTAGE_RATIO);   // 简化标定；精确值用 adc_cali
        charging = false;
        discharging = false;
        level = 100 * (mv - BATTERY_EMPTY_MV) / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        return true;
    }
};

DECLARE_BOARD(MiaoMiaoESP32S3);
