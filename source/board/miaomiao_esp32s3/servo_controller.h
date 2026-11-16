// =============================================================================
//  servo_controller.h  —  SG90 舵机控制器（角度 + 时长双参数，带缓动与队列）
//
//  设计要点：
//  1) LEDC 50Hz / 13bit 输出标准舵机 PWM，脉宽范围可在 config.h 标定
//  2) MoveTo(angle, duration_ms) 用 esp_timer 每 20ms 递进一格占空比，
//     duration_ms 越大转得越慢 —— 这就是"慢慢转"的物理实现
//  3) 内部队列 + 任务串行化，避免多舵机同时启动的电流冲击（brownout）
//     两个舵机同时动作时强制错开 50ms 上电（照抄社区实测结论）
//  4) 对外只暴露语义化接口，MCP 层只做参数搬运
// =============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include <driver/ledc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

class ServoController {
public:
    struct Channel {
        gpio_num_t gpio;
        ledc_channel_t ledc_channel;
        int current_angle;    // 当前角度（度）
        int target_angle;     // 目标角度
        int pulse_us;         // 当前脉宽
        bool continuous = false;  // true=连续转动模式（改装舵机当轮子用）
    };

    static ServoController& GetInstance() {
        static ServoController instance;
        return instance;
    }

    // 初始化 LEDC 定时器与通道；失败返回 false 并打印原因
    bool Initialize();

    // 立即到位（duration_ms = 0 时等价于直接设占空比）
    // index: 0=左舵机 1=右舵机；-1 表示两个一起
    // 返回 false 表示未初始化或参数越界
    bool MoveTo(int index, int angle, int duration_ms);

    // 读当前角度；-1 表示读全部，返回 vector 长度 SERVO_COUNT
    int GetAngle(int index) const;

    // 预设动作（与固件里"生气的摸了你的耳朵"等情绪动作对应）
    bool RunAction(const std::string& name, int speed_percent);

    // ---- 连续转动模式（改装舵机） ----
    // 切换单个舵机的定位/连续模式；切回定位模式时自动回中
    bool SetContinuous(int index, bool enable);
    bool IsContinuous(int index) const { return (index >= 0 && index < SERVO_COUNT) && channels_[index].continuous; }

    // 连续转动：direction 1=前进 -1=后退 0=立即停止
    // speed_percent 0-100（0 亦即停止）；duration_ms 后自动停止（=0 则一直转）
    bool Drive(int index, int direction, int speed_percent, int duration_ms);

    bool IsReady() const { return ready_; }

private:
    ServoController() = default;

    struct Step {
        int index;
        int angle;
        int duration_ms;
    };

    void Enqueue(Step step);
    static void TimerCallback(void* arg);
    static void WorkerTask(void* arg);
    void ApplyPulse(Channel& ch, int pulse_us);
    static int AngleToPulse(int angle);

    Channel channels_[SERVO_COUNT] = {
        {SERVO_LEFT_GPIO,  LEDC_CHANNEL_0, SERVO_CENTER_ANGLE, SERVO_CENTER_ANGLE, SERVO_MIN_PULSE_US},
        {SERVO_RIGHT_GPIO, LEDC_CHANNEL_1, SERVO_CENTER_ANGLE, SERVO_CENTER_ANGLE, SERVO_MIN_PULSE_US},
    };

    bool ready_ = false;
    std::deque<Step> queue_;
    std::mutex mutex_;
    TaskHandle_t worker_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;

    // 插值运行时状态
    struct Ramp {
        bool active = false;
        int index = -1;
        int from_pulse = 0;
        int to_pulse = 0;
        int total_steps = 0;
        int step_index = 0;
    };
    Ramp ramp_;

    // 连续转动模式的自动停止定时器（每通道一个）
    esp_timer_handle_t cont_timer_[SERVO_COUNT] = {nullptr, nullptr};
    static void ContStopCallback(void* arg);   // arg = &channels_[i]
};
