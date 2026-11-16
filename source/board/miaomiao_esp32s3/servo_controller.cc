// =============================================================================
//  servo_controller.cc  —  SG90 舵机控制器实现
// =============================================================================
#include "servo_controller.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <esp_log.h>

static const char* TAG = "Servo";

// 插值步进周期。20ms = SG90 的一个 PWM 周期，视觉上足够平滑
static const int kStepIntervalMs = 20;
// 多舵机同时启动时的上电错峰（社区实测：可把峰值电流从 2.3A 压到 1.3A）
static const int kStaggerMs = 50;

bool ServoController::Initialize() {
    // 1) 定时器：50Hz / 13bit。13bit 下 20ms 周期 = 16384 计数，
    //    1us ≈ 0.8192 计数，resolution 足够表达 500–2500us
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = SERVO_LEDC_SPEED_MODE;
    timer_cfg.duty_resolution = SERVO_LEDC_RESOLUTION;
    timer_cfg.timer_num       = SERVO_LEDC_TIMER;
    timer_cfg.freq_hz         = SERVO_LEDC_FREQ_HZ;
    timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "定时器配置失败: %s", esp_err_to_name(err));
        return false;
    }

    // 2) 通道：错峰初始化，避免两颗舵机同时吸合
    for (int i = 0; i < SERVO_COUNT; ++i) {
        if (channels_[i].gpio == GPIO_NUM_NC) {
            ESP_LOGW(TAG, "舵机 %d 引脚未配置，跳过", i);
            continue;
        }
        ledc_channel_config_t ch_cfg = {};
        ch_cfg.gpio_num   = channels_[i].gpio;
        ch_cfg.speed_mode = SERVO_LEDC_SPEED_MODE;
        ch_cfg.channel    = channels_[i].ledc_channel;
        ch_cfg.intr_type  = LEDC_INTR_DISABLE;
        ch_cfg.timer_sel  = SERVO_LEDC_TIMER;
        ch_cfg.duty       = 0;
        ch_cfg.hpoint     = 0;
        err = ledc_channel_config(&ch_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "通道配置失败 (ch=%d, gpio=%d): %s",
                     i, channels_[i].gpio, esp_err_to_name(err));
            return false;
        }
        // 先回到中位，避免上电瞬间猛甩
        ApplyPulse(channels_[i], AngleToPulse(SERVO_CENTER_ANGLE));
        vTaskDelay(pdMS_TO_TICKS(kStaggerMs));
    }

    // 3) 插值定时器（一次性，每次动作重新 Start）
    esp_timer_create_args_t t_args = {};
    t_args.callback = &ServoController::TimerCallback;
    t_args.arg      = this;
    t_args.name     = "servo_ramp";
    if (esp_timer_create(&t_args, &timer_) != ESP_OK) {
        ESP_LOGE(TAG, "插值定时器创建失败");
        return false;
    }

    // 4) 工作线程：串行消费动作队列
    xTaskCreate(&ServoController::WorkerTask, "ServoTask", 3072, this, 5, &worker_);

    // 5) 连续转动模式的自动停止定时器（每通道一个，惰性使用）
    for (int i = 0; i < SERVO_COUNT; ++i) {
        esp_timer_create_args_t c_args = {};
        c_args.callback = &ServoController::ContStopCallback;
        c_args.arg      = &channels_[i];
        c_args.name     = "servo_cont_stop";
        if (esp_timer_create(&c_args, &cont_timer_[i]) != ESP_OK) {
            ESP_LOGE(TAG, "连续模式停止定时器创建失败 (ch=%d)", i);
        }
    }

    ready_ = true;
    ESP_LOGI(TAG, "舵机初始化成功 (数量=%d, 频率=%dHz, 分辨率=13bit)",
             SERVO_COUNT, SERVO_LEDC_FREQ_HZ);
    return true;
}

int ServoController::AngleToPulse(int angle) {
    angle = std::clamp(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
    // 线性映射：0° → SERVO_MIN_PULSE_US，180° → SERVO_MAX_PULSE_US
    const float span_angle = static_cast<float>(SERVO_MAX_ANGLE - SERVO_MIN_ANGLE);
    const float span_pulse = static_cast<float>(SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US);
    return SERVO_MIN_PULSE_US +
           static_cast<int>(std::lround((angle - SERVO_MIN_ANGLE) * span_pulse / span_angle));
}

void ServoController::ApplyPulse(Channel& ch, int pulse_us) {
    // 13bit @50Hz: period = 2^13 = 8192 计数 / 20ms → 1us = 0.4096 计数
    // 校验：2^13 = 8192, 20ms/8192 = 2.4414us per tick → 1us = 0.4096 tick
    const uint32_t max_duty = (1u << 13) - 1;   // 8191
    float ticks = static_cast<float>(pulse_us) * 0.4096f;
    if (ticks < 0) ticks = 0;
    if (ticks > max_duty) ticks = max_duty;
    ch.pulse_us = pulse_us;
    esp_err_t err = ledc_set_duty(SERVO_LEDC_SPEED_MODE, ch.ledc_channel,
                                  static_cast<uint32_t>(ticks));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置占空比失败: %s", esp_err_to_name(err));
        return;
    }
    err = ledc_update_duty(SERVO_LEDC_SPEED_MODE, ch.ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "更新占空比失败: %s", esp_err_to_name(err));
    }
}

void ServoController::Enqueue(Step step) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(step);
}

bool ServoController::MoveTo(int index, int angle, int duration_ms) {
    if (!ready_) {
        ESP_LOGW(TAG, "舵机未初始化");
        return false;
    }
    angle = std::clamp(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
    duration_ms = std::clamp(duration_ms, 0, 10000);

    if (index < 0 || index >= SERVO_COUNT) {
        // -1 之外的越界值直接拒绝
        if (index != -1) {
            ESP_LOGW(TAG, "舵机序号无效: %d", index);
            return false;
        }
        for (int i = 0; i < SERVO_COUNT; ++i) Enqueue({i, angle, duration_ms});
        return true;
    }
    Enqueue({index, angle, duration_ms});
    return true;
}

int ServoController::GetAngle(int index) const {
    if (index < 0 || index >= SERVO_COUNT) return SERVO_CENTER_ANGLE;
    return channels_[index].current_angle;
}

bool ServoController::RunAction(const std::string& name, int speed_percent) {
    // speed_percent: 100 = 全速，越小越慢
    speed_percent = std::clamp(speed_percent, 10, 200);
    const int slow = 900 * 100 / speed_percent;   // 基准 900ms

    struct Keyframe { int left; int right; int hold_ms; };
    std::vector<Keyframe> kf;

    if (name == "nod") {                    // 点头：低头→回中，重复两下
        kf = {{90, 90, 0}, {55, 55, slow / 2}, {90, 90, 120},
              {55, 55, slow / 2}, {90, 90, 0}};
    } else if (name == "shake") {           // 摇头：左右交替
        kf = {{90, 90, 0}, {140, 40, slow / 2}, {40, 140, slow / 2},
              {140, 40, slow / 2}, {90, 90, 0}};
    } else if (name == "wave") {            // 招手：单侧抬起摆动
        kf = {{90, 90, 0}, {90, 150, slow / 2}, {90, 100, slow / 3},
              {90, 150, slow / 3}, {90, 100, slow / 3}, {90, 90, 0}};
    } else if (name == "happy") {           // 开心：两侧外张
        kf = {{90, 90, 0}, {150, 30, slow / 2}, {150, 30, 150},
              {90, 90, slow / 2}};
    } else if (name == "angry") {           // 生气：快速内收抖动
        kf = {{90, 90, 0}, {60, 120, slow / 4}, {70, 110, slow / 4},
              {60, 120, slow / 4}, {90, 90, slow / 3}};
    } else if (name == "sleepy") {          // 犯困：缓慢下垂
        kf = {{90, 90, 0}, {30, 30, slow * 2}};
    } else if (name == "forward") {         // 定位模式的"前进"：双臂前摆再回中
        kf = {{90, 90, 0}, {45, 45, slow / 2}, {90, 90, slow / 2}};
    } else if (name == "backward") {        // 定位模式的"后退"：双臂后摆再回中
        kf = {{90, 90, 0}, {135, 135, slow / 2}, {90, 90, slow / 2}};
    } else if (name == "center" || name == "reset") {
        kf = {{90, 90, slow}};
    } else {
        ESP_LOGW(TAG, "(no servo) 未知动作: %s", name.c_str());
        return false;
    }

    for (const auto& f : kf) {
        Enqueue({0, f.left, f.hold_ms});
        Enqueue({1, f.right, f.hold_ms});
        if (f.hold_ms > 0) vTaskDelay(pdMS_TO_TICKS(f.hold_ms));
    }
    return true;
}

// ---------------------------------------------------------------------------
//  连续转动模式（改装舵机当轮子）：脉宽即速度/方向
// ---------------------------------------------------------------------------
bool ServoController::SetContinuous(int index, bool enable) {
    if (index < 0 || index >= SERVO_COUNT || !ready_) return false;
    Channel& ch = channels_[index];
    if (ch.continuous == enable) return true;
    ch.continuous = enable;
    if (!enable) {
        // 切回定位模式：立即回中，恢复角度语义
        Enqueue({index, SERVO_CENTER_ANGLE, 300});
    } else {
        // 进入连续模式：先发停止脉宽，避免误转
        ApplyPulse(ch, SERVO_CONT_STOP_PULSE_US);
    }
    ESP_LOGI(TAG, "舵机 %d 切换为%s模式", index, enable ? "连续转动" : "定位");
    return true;
}

bool ServoController::Drive(int index, int direction, int speed_percent, int duration_ms) {
    if (!ready_) return false;
    if (index < 0 || index >= SERVO_COUNT) return false;
    Channel& ch = channels_[index];
    if (!ch.continuous) {
        ESP_LOGW(TAG, "舵机 %d 不在连续模式，先用 self.servo.mode 切换", index);
        return false;
    }
    speed_percent = std::clamp(speed_percent, 0, 100);
    if (speed_percent == 0) direction = 0;
    duration_ms = std::clamp(duration_ms, 0, 60000);

    // 等角度插值结束（同通道互斥）
    while (ramp_.active && ramp_.index == index) {
        vTaskDelay(pdMS_TO_TICKS(kStepIntervalMs));
    }

    if (direction == 0) {
        ApplyPulse(ch, SERVO_CONT_STOP_PULSE_US);
        return true;
    }
    // 停止脉宽 ± 偏移：>STOP=前进，<STOP=后退
    int offset = SERVO_CONT_SPEED_SPAN_US * speed_percent / 100;
    int pulse  = SERVO_CONT_STOP_PULSE_US + (direction > 0 ? offset : -offset);
    ApplyPulse(ch, pulse);

    if (duration_ms > 0) {
        esp_timer_stop(cont_timer_[index]);          // 未启动时 stop 返回错误，无害
        esp_timer_start_once(cont_timer_[index], duration_ms * 1000);
    }
    return true;
}

void ServoController::ContStopCallback(void* arg) {
    auto* ch = static_cast<Channel*>(arg);
    auto* self = &ServoController::GetInstance();
    self->ApplyPulse(*ch, SERVO_CONT_STOP_PULSE_US);
    ESP_LOGI(TAG, "连续转动时间到，舵机已停止 (gpio=%d)", ch->gpio);
}

// ---------------------------------------------------------------------------
//  插值：每 20ms 走一步，把 duration_ms 摊成 total_steps 步
// ---------------------------------------------------------------------------
void ServoController::TimerCallback(void* arg) {
    auto* self = static_cast<ServoController*>(arg);
    if (!self->ramp_.active) return;

    Ramp& r = self->ramp_;
    r.step_index++;
    if (r.step_index >= r.total_steps) {
        // 收尾：精确落到目标脉宽
        Channel& ch = self->channels_[r.index];
        self->ApplyPulse(ch, r.to_pulse);
        ch.current_angle = ch.target_angle;
        r.active = false;
        esp_timer_stop(self->timer_);
        return;
    }

    // 线性插值（如需更自然可换 ease-in-out：t = t*t*(3-2t)）
    float t = static_cast<float>(r.step_index) / static_cast<float>(r.total_steps);
    int pulse = r.from_pulse +
                static_cast<int>(std::lround((r.to_pulse - r.from_pulse) * t));
    self->ApplyPulse(self->channels_[r.index], pulse);
}

void ServoController::WorkerTask(void* arg) {
    auto* self = static_cast<ServoController*>(arg);
    while (true) {
        Step step;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->queue_.empty()) {
                step.index = -2;
            } else {
                step = self->queue_.front();
                self->queue_.pop_front();
            }
        }
        if (step.index == -2) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        Channel& ch = self->channels_[step.index];
        ch.target_angle = step.angle;
        int to_pulse = AngleToPulse(step.angle);

        if (step.duration_ms < kStepIntervalMs) {
            // 瞬时到位
            self->ApplyPulse(ch, to_pulse);
            ch.current_angle = step.angle;
            continue;
        }

        // 渐进插值：等前一个动作跑完（ramp 单槽，简单可靠）
        while (self->ramp_.active) {
            vTaskDelay(pdMS_TO_TICKS(kStepIntervalMs));
        }
        self->ramp_.active      = true;
        self->ramp_.index       = step.index;
        self->ramp_.from_pulse  = ch.pulse_us;
        self->ramp_.to_pulse    = to_pulse;
        self->ramp_.total_steps = step.duration_ms / kStepIntervalMs;
        self->ramp_.step_index  = 0;
        esp_timer_start_periodic(self->timer_, kStepIntervalMs * 1000);

        // 等到这段插值走完再取下一个动作
        while (self->ramp_.active) {
            vTaskDelay(pdMS_TO_TICKS(kStepIntervalMs));
        }
    }
}
