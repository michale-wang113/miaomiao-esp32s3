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
    if "[miaomiao-fullscreen-preview]" in text:
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
    ]
    for old, new in reps:
        n = text.count(old)
        if n != 1:
            print(f"[patch] ⚠️ 预览图补丁锚点命中 {n} 次（期望 1），中止: {old[:60]!r}")
            return
        text = text.replace(old, new)
    lcd.write_text(text, encoding="utf-8")
    print("[patch] lcd_display.cc 预览图已改为全屏 cover 模式")


if __name__ == "__main__":
    ok = patch_kconfig()
    patch_cmake()
    write_sdkconfig_defaults()
    patch_display_preview()
    if not (root / "main" / "boards" / BOARD_DIR).exists():
        print(f"[patch] ⚠️ 尚未复制板目录: cp -r source/board/{BOARD_DIR} main/boards/{BOARD_DIR}")
    print("[patch] 完成。下一步: idf.py set-target esp32s3 && idf.py build")
