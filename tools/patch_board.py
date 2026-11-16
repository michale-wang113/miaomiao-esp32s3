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
    """若 main/CMakeLists.txt 显式列出板目录则追加；glob 式则无需改动。"""
    cmake = root / "main" / "CMakeLists.txt"
    if not cmake.exists():
        print("[patch] 未找到 main/CMakeLists.txt，跳过")
        return
    text = cmake.read_text(encoding="utf-8")
    if BOARD_DIR in text:
        print("[patch] CMakeLists 已含板目录，跳过")
        return
    # 形如 set(BOARD_TYPE_DIRS xxx yyy) 或 list(APPEND BOARD_DIRS ...) 的显式清单
    m = re.search(r"(set|list\s*\(APPEND)\s*\(?([A-Z_]+(?:BOARD|BOARD_TYPE|BOARD_DIRS)[A-Z_]*)\)?\s+([^)\n]+)", text)
    if m and not re.search(r"GLOB", text[m.start():m.end() + 400]):
        new_text = text.replace(m.group(0), m.group(0).rstrip() + " " + BOARD_DIR, 1)
        cmake.write_text(new_text, encoding="utf-8")
        print(f"[patch] CMakeLists 已追加 {BOARD_DIR}")
        return
    print("[patch] CMakeLists 未显式列出板目录（应为 glob 自动收录），无需改动")


def write_sdkconfig_defaults() -> None:
    """sdkconfig.defaults 指定默认板型 + 串口/JTAG 控制台。"""
    defaults = root / "sdkconfig.defaults"
    lines = []
    if defaults.exists():
        lines = [l for l in defaults.read_text(encoding="utf-8").splitlines()
                 if "BOARD_TYPE" not in l]
    lines.append(f"CONFIG_BOARD_TYPE_{KCONFIG_NAME.replace('BOARD_TYPE_', '')}=y")
    defaults.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("[patch] sdkconfig.defaults 已写入默认板型")


if __name__ == "__main__":
    ok = patch_kconfig()
    patch_cmake()
    write_sdkconfig_defaults()
    if not (root / "main" / "boards" / BOARD_DIR).exists():
        print(f"[patch] ⚠️ 尚未复制板目录: cp -r source/board/{BOARD_DIR} main/boards/{BOARD_DIR}")
    print("[patch] 完成。下一步: idf.py set-target esp32s3 && idf.py build")
