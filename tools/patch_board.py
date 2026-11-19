#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_board.py — 把 miaomiao-esp32s3 板级目录注册进 xiaozhi-esp32 工程
用法（在 xiaozhi-esp32 仓库根目录执行）:
    cp -r <本仓库>/source/board/miaomiao_esp32s3 main/boards/miaomiao-esp32s3
    python3 <本仓库>/tools/patch_board.py
脚本自动适配 v2.x 的两种注册机制（Kconfig choice 条目 + CMakeLists 板目录清单）。
"""
import re
import sys
from pathlib import Path

BOARD_DIR = "miaomiao-esp32s3"
KCONFIG_NAME = "BOARD_TYPE_MIAOMIAO_ESP32S3"

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")


def patch_kconfig() -> bool:
    """在 main/Kconfig.projbuild 的 choice BOARD_TYPE 里加一个条目。"""
    kconfig = root / "main" / "Kconfig.projbuild"
    if not kconfig.exists():
        print("[patch] 未找到 main/Kconfig.projbuild，跳过 Kconfig")
        return False
    text = kconfig.read_text(encoding="utf-8")
    if KCONFIG_NAME in text:
        print("[patch] Kconfig 已含条目，跳过")
        return True
    m = re.search(r"(choice\s+BOARD_TYPE\b.*?)(\n\s*endchoice)", text, re.S)
    if not m:
        print("[patch] Kconfig 中未找到 choice BOARD_TYPE，需手工添加")
        return False
    entry = (
        f"    config {KCONFIG_NAME}\n"
        f"        bool \"喵喵机芯盒 ESP32-S3 (定制板)\"\n"
        f"        depends on IDF_TARGET_ESP32S3\n"
    )
    new_text = text[: m.end(1)] + "\n" + entry + text[m.end(1):]
    kconfig.write_text(new_text, encoding="utf-8")
    print(f"[patch] Kconfig 已加 {KCONFIG_NAME}")
    return True


def patch_cmake() -> None:
    """在 main/CMakeLists.txt 的 if(CONFIG_BOARD_TYPE_XXX) 映射链头部插入本板分支，
    使 BOARD_TYPE 变量指向板目录名（源码经 file(GLOB boards/${BOARD_TYPE}/*.cc) 收集）。"""
    cmake = root / "main" / "CMakeLists.txt"
    if not cmake.exists():
        print("[patch] 未找到 main/CMakeLists.txt，跳过")
        return
    text = cmake.read_text(encoding="utf-8")
    if KCONFIG_NAME in text:
        print("[patch] CMakeLists 已含本板映射，跳过")
        return
    m = re.search(r"if\(CONFIG_BOARD_TYPE_[A-Z0-9_]+\)", text)
    if not m:
        print("[patch] ⚠️ CMakeLists 中未找到 if(CONFIG_BOARD_TYPE_...) 映射链，需手工添加:")
        print(f'    if({KCONFIG_NAME})\n        set(BOARD_TYPE "{BOARD_DIR}")')
        return
    # 将链首 if(CONFIG_BOARD_TYPE_XXX) 替换为: 本板 if + set + elseif(原条件)，
    # 保持 if/elseif/.../endif 嵌套结构完整
    branch = (f'if(CONFIG_{KCONFIG_NAME})\n'
              f'    set(BOARD_TYPE "{BOARD_DIR}")\n'
              + m.group(0).replace("if(", "elseif(", 1))
    new_text = text[: m.start()] + branch + text[m.end():]
    cmake.write_text(new_text, encoding="utf-8")
    print(f'[patch] CMakeLists 已插入映射: {KCONFIG_NAME} -> {BOARD_DIR}')


def write_sdkconfig_defaults() -> None:
    """sdkconfig.defaults 指定默认板型 + 串口/JTAG 控制台。"""
    defaults = root / "sdkconfig.defaults"
    lines = []
    if defaults.exists():
        lines = [l for l in defaults.read_text(encoding="utf-8").splitlines()
                 if "BOARD_TYPE" not in l]
    lines.append(f"CONFIG_BOARD_TYPE_{KCONFIG_NAME.replace('BOARD_TYPE_', '')}=y")
    # 摄像头修复三件套之一：S3 上让 camera DMA 直接走 PSRAM，
    # 避免内部 RAM 不足导致 cam_dma_config "frames malloc failed"（vendor 构建同款行为）
    if "CONFIG_CAMERA_PSRAM_DMA" not in lines:
        lines.append("CONFIG_CAMERA_PSRAM_DMA=y")
    # 待机居中大时钟需要的 LVGL 内置大号数字字体
    if "CONFIG_LV_FONT_MONTSERRAT_48" not in lines:
        lines.append("CONFIG_LV_FONT_MONTSERRAT_48=y")
    defaults.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("[patch] sdkconfig.defaults 已写入默认板型 + CAMERA_PSRAM_DMA")


def patch_display_preview() -> None:
    """预览图全屏化：拍照/preview_image 显示时铺满整个圆屏（cover 裁边、居中）。
    上游 v2.2.2 默认只显示 70%×50% 气泡小图（缩放上限 100%、左对齐），
    在 GC9A01 圆屏上表现为"照片缩在屏幕中间一小块"。
    锚点字符串已对 78/xiaozhi-esp32 v2.2.2 的 lcd_display.cc 逐字节校验。"""
    lcd = root / "main" / "display" / "lcd_display.cc"
    if not lcd.exists():
        print("[patch] 未找到 main/display/lcd_display.cc，跳过预览图补丁")
        return
    text = lcd.read_text(encoding="utf-8")
    if ("[miaomiao-fullscreen-preview]" in text and "[miaomiao-bars-transparent]" in text
            and "[miaomiao-hide-bars-on-preview]" in text):
        print("[patch] 预览图补丁已应用，跳过")
        return
    reps = [
        (
            "lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);",
            "lv_obj_set_style_pad_all(img_bubble, 0, 0);  // [miaomiao-fullscreen-preview]",
        ),
        (
            "lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width",
            "lv_coord_t max_width = LV_HOR_RES;  // [miaomiao-fullscreen-preview] 铺满整屏",
        ),
        (
            "lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height",
            "lv_coord_t max_height = LV_VER_RES;  // [miaomiao-fullscreen-preview]",
        ),
        (
            "lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;",
            "lv_coord_t zoom = (zoom_w > zoom_h) ? zoom_w : zoom_h;  // [miaomiao-fullscreen-preview] cover 模式",
        ),
        (
            "    // Ensure zoom doesn't exceed 256 (100%)\n    if (zoom > 256) zoom = 256;\n",
            "    // [miaomiao-fullscreen-preview] 解除 100% 缩放上限，小图允许放大铺满圆屏\n",
        ),
        (
            "    lv_obj_set_width(img_bubble, scaled_width + 16);\n"
            "    lv_obj_set_height(img_bubble, scaled_height + 16);",
            "    lv_obj_set_width(img_bubble, max_width);   // [miaomiao-fullscreen-preview]\n"
            "    lv_obj_set_height(img_bubble, max_height);\n"
            "    lv_obj_remove_flag(img_bubble, LV_OBJ_FLAG_SCROLLABLE);  // 裁掉超出圆屏的部分",
        ),
        (
            "lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);",
            "lv_obj_align(img_bubble, LV_ALIGN_CENTER, 0, 0);  // [miaomiao-fullscreen-preview]",
        ),
        # ---- 非微信样式分支（#else，CONFIG_USE_WECHAT_MESSAGE_STYLE 关闭时编译这套）----
        # 该分支预览用固定成员 preview_image_：创建时写死半屏居中，缩放公式 128*width_/w（仅 50%）
        # ——这才是 miaomiao 构建实际编译的分支（2026-11-19 修正）
        (
            "lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);",
            "lv_obj_set_size(preview_image_, width_, height_);  // [miaomiao-fullscreen-preview] 整屏",
        ),
        (
            "    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {\n"
            "        // zoom factor 0.5\n"
            "        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);\n"
            "    }",
            "    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {\n"
            "        // [miaomiao-fullscreen-preview] cover 模式：短边铺满整屏，超出部分被屏幕边缘裁掉\n"
            "        int scale_w = 256 * width_ / img_dsc->header.w;\n"
            "        int scale_h = 256 * height_ / img_dsc->header.h;\n"
            "        lv_image_set_scale(preview_image_, scale_w > scale_h ? scale_w : scale_h);\n"
            "    }",
        ),
        # ---- 状态栏/底栏透明化（[miaomiao-bars-transparent]）----
        # 现象：预览图全屏后，顶部 top_bar_ 的 50% 半透明背景（LV_OPA_50）盖在照片上，
        # 在"说话中"等状态文字周围形成一块泛白方块（2026-11-19 用户照片实锤）。
        # 普通界面下这些栏与主题背景同色，改全透明后观感完全不变；仅预览时不再挡照片。
        # 锚点唯一性：#if 微信样式分支的 top_bar_ 挂在 container_ 上（lv_obj_create(container_)），
        # 这里用 lv_obj_create(screen) 锁定 #else 分支；SetTheme 处的替换带 8 空格缩进且无行尾注释，唯一。
        (
            "    top_bar_ = lv_obj_create(screen);\n"
            "    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);\n"
            "    lv_obj_set_style_radius(top_bar_, 0, 0);\n"
            "    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background",
            "    top_bar_ = lv_obj_create(screen);\n"
            "    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);\n"
            "    lv_obj_set_style_radius(top_bar_, 0, 0);\n"
            "    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);  // [miaomiao-bars-transparent] 原为 50%，全屏预览时成白块",
        ),
        (
            "        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);",
            "        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);  // [miaomiao-bars-transparent]",
        ),
        (
            "    bottom_bar_ = lv_obj_create(screen);\n"
            "    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(12));\n"
            "    lv_obj_set_style_radius(bottom_bar_, 0, 0);\n"
            "    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);",
            "    bottom_bar_ = lv_obj_create(screen);\n"
            "    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(12));\n"
            "    lv_obj_set_style_radius(bottom_bar_, 0, 0);\n"
            "    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);\n"
            "    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);  // [miaomiao-bars-transparent] 预览时不挡照片",
        ),
        (
            "        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);",
            "        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);  // [miaomiao-bars-transparent]",
        ),
        # ---- 预览期间隐藏三栏（[miaomiao-hide-bars-on-preview]）----
        # 2026-11-19 ELF 反汇编证实透明化已编译但用户仍见"白块"：50% 白叠在浅色照片上
        # 与全透明视觉几乎无差别。改为预览显示期间直接隐藏 top_bar_/status_bar_/bottom_bar_，
        # 照片全屏零遮挡；预览结束（定时器回调 SetPreviewImage(nullptr)）恢复三栏。
        # 锚点唯一性：#if 分支 SetPreviewImage 为 "if (image == nullptr) { return; }" 结构，与此不同。
        (
            "    // Hide emoji_box_\n"
            "    if (gif_controller_) {\n"
            "        gif_controller_->Stop();\n"
            "    }\n"
            "    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);\n"
            "    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);",
            "    // Hide emoji_box_\n"
            "    if (gif_controller_) {\n"
            "        gif_controller_->Stop();\n"
            "    }\n"
            "    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);\n"
            "    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);\n"
            "    // [miaomiao-hide-bars-on-preview] 预览期间隐藏三栏，照片全屏无遮挡\n"
            "    if (top_bar_ != nullptr) {\n"
            "        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "    }\n"
            "    if (status_bar_ != nullptr) {\n"
            "        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "    }\n"
            "    if (bottom_bar_ != nullptr) {\n"
            "        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "    }",
        ),
        (
            "    if (image == nullptr) {\n"
            "        esp_timer_stop(preview_timer_);\n"
            "        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);\n"
            "        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);\n"
            "        preview_image_cached_.reset();",
            "    if (image == nullptr) {\n"
            "        esp_timer_stop(preview_timer_);\n"
            "        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);\n"
            "        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);\n"
            "        // [miaomiao-hide-bars-on-preview] 预览结束恢复三栏\n"
            "        if (top_bar_ != nullptr) {\n"
            "            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "        }\n"
            "        if (status_bar_ != nullptr) {\n"
            "            lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "        }\n"
            "        if (bottom_bar_ != nullptr) {\n"
            "            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "        }\n"
            "        preview_image_cached_.reset();",
        ),
    ]
    for old, new in reps:
        n = text.count(old)
        if n != 1:
            print(f"[patch] ⚠️ 预览图补丁锚点命中 {n} 次（期望 1），中止: {old[:60]!r}")
            return
        text = text.replace(old, new)
    lcd.write_text(text, encoding="utf-8")
    print("[patch] lcd_display.cc 预览图已改为全屏 cover 模式")


def patch_idle_no_emoji() -> None:
    """待机模式对齐 vendor 并美化：黑底白字 + 居中大时钟。
    1) application.cc idle 分支 SetEmotion("neutral")→("none")，待机不播眼睛动画
       （LVGL 9.4 对"文字局部重绘落在 GIF 画布上"的合成有缺陷，待机时钟每 10 秒
       更新就闪白屏；vendor 待机无眼睛所以从不触发）。
    2) lcd_display.cc SetEmotion 增加 "none" 处理：隐藏眼睛/状态栏，背景改纯黑，
       显示居中 montserrat_48 大时钟（sdkconfig 启用 CONFIG_LV_FONT_MONTSERRAT_48）；
       离开待机时隐藏时钟、恢复状态栏并 SetTheme(current_theme_) 还原主题背景。
    3) lvgl_display.h 增加 idle_clock_label_ 成员（两个 TU 共用）。
    4) lvgl_display.cc SetStatus 把 "HH:MM" 形态文本同步到居中大时钟。
    锚点已对 v2.2.2 上游源码验证各命中 1 次。"""
    app = root / "main" / "application.cc"
    lcd = root / "main" / "display" / "lcd_display.cc"
    lcd_h = root / "main" / "display" / "lvgl_display" / "lvgl_display.h"
    lvgl_cc = root / "main" / "display" / "lvgl_display" / "lvgl_display.cc"
    for p in [app, lcd, lcd_h, lvgl_cc]:
        if not p.exists():
            print(f"[patch] 未找到 {p.name}，跳过待机无表情补丁")
            return
    text_app = app.read_text(encoding="utf-8")
    text_lcd = lcd.read_text(encoding="utf-8")
    text_lh = lcd_h.read_text(encoding="utf-8")
    text_lcc = lvgl_cc.read_text(encoding="utf-8")
    if "[miaomiao-idle-clock]" in text_app:
        print("[patch] 待机无表情补丁已应用，跳过")
        return

    reps_app = [
        (
            'display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)',
            'display->SetEmotion("none"); // [miaomiao-idle-clock] 待机不显示眼睛动画（vendor 行为），'
            "避免 GIF 画布局部重绘闪白",
        ),
    ]

    reps_lcd = [
        # 文件头：声明 montserrat_48 大字体
        (
            "#include <cstring>\n",
            "#include <cstring>\n"
            "#include <ctime>\n"
            "LV_FONT_DECLARE(lv_font_montserrat_48);  // [miaomiao-idle-clock] 待机大时钟字体（sdkconfig 启用）\n",
        ),
        # SetupUI：创建居中大时钟（放在 preview_image_ 之前，emoji_box_ 之后）
        (
            "    /* Middle layer: preview_image_ - centered display */\n",
            "    // [miaomiao-idle-clock] 待机大时钟（居中白字，默认隐藏）\n"
            "    idle_clock_label_ = lv_label_create(screen);\n"
            "    lv_obj_set_style_text_font(idle_clock_label_, &lv_font_montserrat_48, 0);\n"
            "    lv_obj_set_style_text_color(idle_clock_label_, lv_color_white(), 0);\n"
            '    lv_label_set_text(idle_clock_label_, "");\n'
            "    lv_obj_align(idle_clock_label_, LV_ALIGN_CENTER, 0, 0);\n"
            "    lv_obj_add_flag(idle_clock_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "\n"
            "    /* Middle layer: preview_image_ - centered display */\n",
        ),
        # SetEmotion："none" 待机模式 + 离开待机恢复
        (
            "    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();",
            "    // [miaomiao-idle-clock] emotion \"none\"：待机模式——黑底白字，隐藏眼睛/状态栏，居中大时钟\n"
            '    if (strcmp(emotion, "none") == 0) {\n'
            "        DisplayLockGuard lock(this);\n"
            "        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);\n"
            "        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "        lv_obj_set_style_bg_color(container_, lv_color_black(), 0);\n"
            "        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);\n"
            "        if (network_label_ != nullptr) lv_obj_set_style_text_color(network_label_, lv_color_white(), 0);\n"
            "        if (mute_label_ != nullptr) lv_obj_set_style_text_color(mute_label_, lv_color_white(), 0);\n"
            "        if (battery_label_ != nullptr) lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);\n"
            "        if (idle_clock_label_ != nullptr) {\n"
            "            // 立即填充当前时间，不用等 10 秒周期刷新\n"
            "            time_t now = time(NULL);\n"
            "            struct tm* tm = localtime(&now);\n"
            "            char time_str[16];\n"
            "            if (tm->tm_year >= 2025 - 1900 && strftime(time_str, sizeof(time_str), \"%H:%M\", tm)) {\n"
            "                lv_label_set_text(idle_clock_label_, time_str);\n"
            "            } else {\n"
            '                lv_label_set_text(idle_clock_label_, "--:--");\n'
            "            }\n"
            "            lv_obj_remove_flag(idle_clock_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "            lv_obj_move_foreground(idle_clock_label_);\n"
            "        }\n"
            "        return;\n"
            "    }\n"
            "\n"
            "    // [miaomiao-idle-clock] 离开待机：隐藏大时钟、恢复状态栏与主题背景\n"
            "    if (idle_clock_label_ != nullptr && !lv_obj_has_flag(idle_clock_label_, LV_OBJ_FLAG_HIDDEN)) {\n"
            "        {\n"
            "            DisplayLockGuard lock(this);\n"
            "            lv_obj_add_flag(idle_clock_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "            lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);\n"
            "        }\n"
            "        SetTheme(current_theme_);\n"
            "    }\n"
            "\n"
            "    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();",
        ),
    ]

    reps_lh = [
        (
            "    lv_obj_t *battery_label_ = nullptr;\n",
            "    lv_obj_t *battery_label_ = nullptr;\n"
            "    lv_obj_t *idle_clock_label_ = nullptr;  // [miaomiao-idle-clock] 待机居中大时钟\n",
        ),
    ]

    reps_lcc = [
        # SetStatus：把 "HH:MM" 同步到待机大时钟
        (
            "    lv_label_set_text(status_label_, status);\n"
            "    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);\n",
            "    lv_label_set_text(status_label_, status);\n"
            "    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);\n"
            "\n"
            "    // [miaomiao-idle-clock] 待机时把时间同步到居中大时钟（仅同步 \"H:MM\"/\"HH:MM\" 形态文本）\n"
            "    if (idle_clock_label_ != nullptr && !lv_obj_has_flag(idle_clock_label_, LV_OBJ_FLAG_HIDDEN)\n"
            "        && strlen(status) >= 4 && strlen(status) <= 5 && strchr(status, ':') != nullptr) {\n"
            "        lv_label_set_text(idle_clock_label_, status);\n"
            "    }\n",
        ),
    ]

    for text, reps, p in [(text_app, reps_app, app), (text_lcd, reps_lcd, lcd),
                          (text_lh, reps_lh, lcd_h), (text_lcc, reps_lcc, lvgl_cc)]:
        for old, new in reps:
            n = text.count(old)
            if n != 1:
                print(f"[patch] ⚠️ 待机补丁锚点命中 {n} 次（期望 1），中止: {old[:60]!r}")
                return
            text = text.replace(old, new)
        p.write_text(text, encoding="utf-8")
    print("[patch] 待机模式补丁已应用（无眼睛 + 黑底白字 + 居中大时钟）")


if __name__ == "__main__":
    ok = patch_kconfig()
    patch_cmake()
    write_sdkconfig_defaults()
    patch_display_preview()
    patch_idle_no_emoji()
    if not (root / "main" / "boards" / BOARD_DIR).exists():
        print(f"[patch] ⚠️ 尚未复制板目录: cp -r source/board/{BOARD_DIR} main/boards/{BOARD_DIR}")
    print("[patch] 完成。下一步: idf.py set-target esp32s3 && idf.py build")
