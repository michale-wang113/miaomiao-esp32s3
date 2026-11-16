// =============================================================================
//  mcp_display_tools.cc  —  屏幕显示内容的语音控制接口层
//
//  设备双眼为 GC9A01 240×240 圆屏（并联，同步显示），表情资源在 assets 分区。
//  小智框架 Display 基类自带 SetEmotion(emotion)，直接接到 MCP 即可。
//
//  已刷入资源包的表情名（assets_new.bin index.json，2026-11-16 实刷验证）：
//    原厂 8 个:  angry neutral sleepy thinking shocked cool happy crying
//    自制 11 个: laughing kissy loving funny confident silly embarrassed
//                surprised confused winking delicious
//
//  语音示例：
//    "笑一个"            → self.display.emotion(name="laughing")
//    "做个飞吻"          → self.display.emotion(name="kissy")
//    "你现在会哪些表情"   → self.display.emotions()
// =============================================================================

#include <string>
#include <vector>

#include <esp_log.h>

#include "mcp_server.h"
#include "board.h"
#include "display.h"

// 表情名清单与 index.json 的 emoji_collection 保持一致（单一事实来源）
static const std::vector<std::string> kEmotionNames = {
    "angry", "neutral", "sleepy", "thinking", "shocked", "cool",
    "happy", "crying",
    "laughing", "kissy", "loving", "funny", "confident", "silly",
    "embarrassed", "surprised", "confused", "winking", "delicious",
};

void RegisterDisplayTools() {
    // -------------------------------------------------------------------------
    // 1) 指定表情
    // -------------------------------------------------------------------------
    std::string emotion_help;
    for (size_t i = 0; i < kEmotionNames.size(); ++i) {
        emotion_help += kEmotionNames[i];
        emotion_help += (i == 6) ? "、\n" : ((i + 1 == kEmotionNames.size()) ? "" : "、");
    }
    emotion_help += "。常见映射：笑=laughing，生气=angry，害羞=embarrassed，"
                    "发火=shocked，疑惑=confused，眨眼=winking，好吃=delicious。";

    McpServer::GetInstance().AddTool(
        "self.display.emotion",
        "在双眼圆屏上播放指定表情动画。name 可选：\n" + emotion_help,
        PropertyList({
            Property("name", kPropertyTypeString, std::string("neutral")),
        }),
        [](const PropertyList& p) -> ReturnValue {
            std::string name = p["name"].value<std::string>();
            bool found = false;
            for (const auto& e : kEmotionNames) {
                if (e == name) { found = true; break; }
            }
            if (!found) {
                return std::string("未知表情: " + name + "，请先调用 self.display.emotions 查询");
            }
            auto* board = &Board::GetInstance();
            auto* display = board->GetDisplay();
            if (display == nullptr) {
                return std::string("显示未就绪");
            }
            display->SetEmotion(name.c_str());
            return true;
        });

    // -------------------------------------------------------------------------
    // 2) 查询表情清单（LLM 用它回答"你会哪些表情"）
    // -------------------------------------------------------------------------
    McpServer::GetInstance().AddTool(
        "self.display.emotions",
        "获取设备当前支持的全部表情名称列表。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            std::string json = "[";
            for (size_t i = 0; i < kEmotionNames.size(); ++i) {
                json += "\"" + kEmotionNames[i] + "\"";
                if (i + 1 < kEmotionNames.size()) json += ",";
            }
            json += "]";
            return json;
        });

    ESP_LOGI("Display", "已注册 2 个表情 MCP 工具 (共 %d 种表情)",
             (int)kEmotionNames.size());
}
