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
    # [屏幕回退实验] 暂时移除 CAMERA_PSRAM_DMA 注入（嫌疑：该配置改 DMA/缓存行为导致显示乱码）
    # if "CAMERA_PSRAM_DMA" not in lines:
    #     lines.append("CONFIG_CAMERA_PSRAM_DMA=y")
    defaults.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("[patch] sdkconfig.defaults 已写入默认板型")


def patch_lcd_display_dc_track() -> None:
    """[Debug] 在 SpiLcdDisplay 构造各阶段插入 GPIO0(DC) 输出使能/电平跟踪日志。"""
    lcd = root / "main" / "display" / "lcd_display.cc"
    if not lcd.exists():
        print("[patch] 未找到 main/display/lcd_display.cc，跳过 DC-TRACK")
        return
    text = lcd.read_text(encoding="utf-8")
    if "DC-TRACK" in text:
        print("[patch] lcd_display.cc 已含 DC-TRACK，跳过")
        return
    LOG = ('    ESP_LOGI(TAG, "[DC-TRACK] {name} OE=%d LVL=%d", '
           '(*(volatile uint32_t*)0x60004020) & 1, (*(volatile uint32_t*)0x60004004) & 1);\n')
    marks = [
        ("    // Set the display to on", "SpiLcd白屏后"),
        ('    ESP_LOGI(TAG, "Initialize LVGL library");', "SpiLcd disp_on后"),
        ('    ESP_LOGI(TAG, "Initialize LVGL port");', "SpiLcd lv_init后"),
        ('    ESP_LOGI(TAG, "Adding LCD display");', "SpiLcd port_init后"),
        ("    SetupUI();", "SpiLcd add_disp后"),
    ]
    count = 0
    for marker, name in marks:
        idx = text.find(marker)
        if idx < 0:
            print(f"[patch] DC-TRACK 未找到标记: {marker.strip()}")
            continue
        text = text[:idx] + LOG.format(name=name) + text[idx:]
        count += 1
    lcd.write_text(text, encoding="utf-8")
    print(f"[patch] lcd_display.cc 已插入 {count} 处 DC-TRACK 日志")


if __name__ == "__main__":
    ok = patch_kconfig()
    patch_cmake()
    write_sdkconfig_defaults()
    patch_lcd_display_dc_track()
    if not (root / "main" / "boards" / BOARD_DIR).exists():
        print(f"[patch] ⚠️ 尚未复制板目录: cp -r source/board/{BOARD_DIR} main/boards/{BOARD_DIR}")
    print("[patch] 完成。下一步: idf.py set-target esp32s3 && idf.py build")
