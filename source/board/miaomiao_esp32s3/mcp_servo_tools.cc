// =============================================================================
//  mcp_servo_tools.cc  —  把舵机接进小智 MCP 体系（就是"语音控舵机"的接口层）
//
//  固件现状（逆向确认）：16 个 MCP 工具已在册，无舵机。
//  本文件补上 5 个工具，注册进 McpServer 后，云端的 LLM 就能自己调用它们。
//
//  调用方式（用户说 → LLM 决策 → 工具调用）：
//    "把头往左转 45 度"            → self.servo.set(index=0, angle=135, duration_ms=500)
//    "慢慢转，用三秒"              → self.servo.set(index=-1, angle=90, duration_ms=3000)
//    "点点头"                      → self.servo.action(name="nod", speed=100)
//    "前进两秒"                    → self.servo.drive(direction=1, duration_ms=2000)  [需改装+切模式]
//    "现在头转到多少度了"          → self.servo.get()
// =============================================================================

#include <cstdio>
#include <string>

#include <esp_log.h>

#include "mcp_server.h"
#include "servo_controller.h"

// 在板型类的 Initialize() 末尾（McpServer 已启动之后）调用一次
void RegisterServoTools() {
    auto& servo = ServoController::GetInstance();

    // -------------------------------------------------------------------------
    // 1) 精确控制：角度 + 时长
    //    这是本次改造的核心工具。duration_ms 让"慢慢转"变成物理上真的慢转。
    // -------------------------------------------------------------------------
    McpServer::GetInstance().AddTool(
        "self.servo.set",
        "控制舵机转到指定角度。"
        "index: 0=左舵机, 1=右舵机, -1=两个一起；"
        "angle: 目标角度 0-180 度（90 为正前方）；"
        "duration_ms: 转动耗时毫秒数，越大转得越慢。"
        "用户说'慢慢转'时用 2000-4000；说'快点'时用 200-400；默认 500。",
        PropertyList({
            Property("index",       kPropertyTypeInteger, 0,   -1, 1),
            Property("angle",       kPropertyTypeInteger, SERVO_CENTER_ANGLE, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE),
            Property("duration_ms", kPropertyTypeInteger, 500, 0,  10000),
        }),
        [&servo](const PropertyList& p) -> ReturnValue {
            int index = p["index"].value<int>();
            int angle = p["angle"].value<int>();
            int ms    = p["duration_ms"].value<int>();
            if (!servo.MoveTo(index, angle, ms)) {
                return std::string("舵机未就绪，请检查供电");
            }
            return true;
        });

    // -------------------------------------------------------------------------
    // 2) 预设动作：语义化指令，比让 LLM 自己算角度更稳
    // -------------------------------------------------------------------------
    McpServer::GetInstance().AddTool(
        "self.servo.action",
        "执行预设舵机动作。name 可选："
        "nod(点头)、shake(摇头)、wave(招手)、happy(开心)、"
        "angry(生气)、sleepy(犯困)、forward(前倾)、backward(后仰)、center(回正)。"
        "speed: 速度百分比，100 为正常，50 为慢速，200 为快速。",
        PropertyList({
            Property("name",  kPropertyTypeString,  std::string("nod")),
            Property("speed", kPropertyTypeInteger, 100, 10, 200),
        }),
        [&servo](const PropertyList& p) -> ReturnValue {
            std::string name = p["name"].value<std::string>();
            int speed = p["speed"].value<int>();
            if (!servo.RunAction(name, speed)) {
                return std::string("未知动作: " + name);
            }
            return true;
        });

    // -------------------------------------------------------------------------
    // 3) 连续转动（前进/后退）：需改装舵机（拆限位挡片）后配合 self.servo.mode
    //    用户说"前进/往前走"→ direction=1；"后退/倒退"→ direction=-1；"停"→ 0
    // -------------------------------------------------------------------------
    McpServer::GetInstance().AddTool(
        "self.servo.drive",
        "让舵机连续转动（前进/后退）。仅当舵机处于连续转动模式时有效"
        "（先用 self.servo.mode 切换，且舵机需物理改装为连续转动）。\n"
        "index: 0=左舵机, 1=右舵机, -1=两个一起；\n"
        "direction: 1=前进, -1=后退, 0=停止；\n"
        "speed: 转速 0-100；\n"
        "duration_ms: 转动时长毫秒，0 表示一直转到收到停止指令。",
        PropertyList({
            Property("index",       kPropertyTypeInteger, 0,   -1, 1),
            Property("direction",   kPropertyTypeInteger, 1,   -1, 1),
            Property("speed",       kPropertyTypeInteger, 60,  0, 100),
            Property("duration_ms", kPropertyTypeInteger, 2000, 0, 60000),
        }),
        [&servo](const PropertyList& p) -> ReturnValue {
            int index = p["index"].value<int>();
            int dir   = p["direction"].value<int>();
            int speed = p["speed"].value<int>();
            int ms    = p["duration_ms"].value<int>();
            if (dir == 0) speed = 0;
            bool ok = true;
            if (index == -1) {
                ok = servo.Drive(0, dir, speed, ms) && servo.Drive(1, dir, speed, ms);
            } else {
                ok = servo.Drive(index, dir, speed, ms);
            }
            if (!ok) return std::string("舵机不在连续转动模式或未就绪");
            return true;
        });

    // -------------------------------------------------------------------------
    // 4) 模式切换：定位模式（角度）↔ 连续转动模式（前进/后退）
    // -------------------------------------------------------------------------
    McpServer::GetInstance().AddTool(
        "self.servo.mode",
        "切换舵机工作模式。continuous=false 为定位模式（转到指定角度，默认）；"
        "continuous=true 为连续转动模式（前进/后退，需舵机已物理改装）。\n"
        "index: 0=左舵机, 1=右舵机, -1=两个一起。",
        PropertyList({
            Property("index",      kPropertyTypeInteger, 0, -1, 1),
            Property("continuous", kPropertyTypeBoolean, false),
        }),
        [&servo](const PropertyList& p) -> ReturnValue {
            int index = p["index"].value<int>();
            bool cont = p["continuous"].value<bool>();
            bool ok = true;
            if (index == -1) {
                ok = servo.SetContinuous(0, cont) && servo.SetContinuous(1, cont);
            } else {
                ok = servo.SetContinuous(index, cont);
            }
            if (!ok) return std::string("切换失败");
            return true;
        });

    // -------------------------------------------------------------------------
    // 5) 状态查询：让 LLM 回答"你的头现在朝哪"
    // -------------------------------------------------------------------------
    McpServer::GetInstance().AddTool(
        "self.servo.get",
        "读取舵机状态（当前角度与工作模式）。",
        PropertyList(),
        [&servo](const PropertyList&) -> ReturnValue {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"left\":%d,\"right\":%d,\"left_mode\":\"%s\",\"right_mode\":\"%s\"}",
                     servo.GetAngle(0), servo.GetAngle(1),
                     servo.IsContinuous(0) ? "continuous" : "positional",
                     servo.IsContinuous(1) ? "continuous" : "positional");
            return std::string(buf);
        });

    ESP_LOGI("Servo", "已注册 5 个舵机 MCP 工具 (set/action/drive/mode/get)");
}
