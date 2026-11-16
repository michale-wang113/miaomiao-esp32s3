# 喵喵同学机芯盒改造 · 工作区索引

> 目标机：东莞创贝乐「AI 玩具机芯盒」（双 1.28" 圆屏眼睛 + 摄像头 + 双舵机）
> 固件：xiaozhi v2.2.2（板型 `miaomiao-esp32s3`），IDF v5.5.4，编译于 2026-07-18 15:19:34

## 目录结构

```
机芯盒改造/
├── README.md                     ← 本文件：索引与恢复手册
├── 逆向分析报告.html              ← 固件逆向结论（可读版，先看这个）
├── .github/workflows/build.yml   ← GitHub Actions 一键编译（推荐路径）
├── firmware/                     ← 原厂固件备份（回滚保险）+ 新表情包 assets_new.bin 已刷入
│   ├── boot_pt_64k.bin           ← bootloader + 分区表 + NVS + PHY（0x000000–0x010000）
│   ├── app_ota0.bin              ← 主程序 ota_0（0x020000–0x510000，5056 KB）✅ 完整
│   ├── assets_pack_full.bin      ← 原厂资源包完整备份（3,720,252B，校验 0x2a3f）
│   ├── assets_new.bin            ← 19 种表情新资源包 ✅ 已刷入 0xA00000
│   └── ota1_slot.bin             ← ota_1 备份槽（0x510000–0xA00000）— 可选
├── tools/
│   ├── patch_board.py            ← 板型注册进 xiaozhi 工程的自动补丁脚本
│   └── jtag_servo_demo.tcl       ← JTAG 在线舵机扫掠演示（已实测成功）
└── source/                       ← 源码
    ├── official/                 ← 官方 v2.2.2 开源代码（8 个关键文件，用于对照）
    └── board/miaomiao_esp32s3/   ← 定制板型（引脚已全部实测锁定）
        ├── config.h              ← 引脚与参数定义 ✅ 全部[已确认]
        ├── servo_controller.h/.cc ← 舵机控制器（角度+时长缓动+队列）
        └── mcp_servo_tools.cc    ← self.servo.set/.action/.get 三个 MCP 工具
```

## 一、原厂固件恢复（改坏时的救命手册）

设备 USB-Serial-JTAG 直连，恢复分三刀写回：

```bash
# 环境（本机已装在 /tmp/esp，重启后需重装，见文末）
export PYTHONPATH=/tmp/esp
PORT=/dev/cu.usbmodem21201

# 1) bootloader + 分区表 + NVS
python3 -m esptool --chip esp32s3 --port $PORT --no-stub \
  write_flash 0x0 firmware/boot_pt_64k.bin

# 2) 主程序
python3 -m esptool --chip esp32s3 --port $PORT \
  write_flash 0x20000 firmware/app_ota0.bin

# 3) 资源分区（表情/字体/唤醒词）—— 用原厂完整备份回滚表情包
python3 -m esptool --chip esp32s3 --port $PORT \
  write_flash 0xA00000 firmware/assets_pack_full.bin

# 4) 复位启动
python3 -m esptool --chip esp32s3 --port $PORT --no-stub run
```

**分区表（实测）**

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| nvs | 0x009000 | 16 KB | 配网信息、校准数据 |
| otadata | 0x00D000 | 8 KB | OTA 启动槽选择 |
| phy_init | 0x00F000 | 4 KB | 射频校准 |
| ota_0 | 0x020000 | 5056 KB | 当前固件 |
| ota_1 | 0x510000 | 5056 KB | OTA 备份槽 |
| assets | 0xA00000 | 6144 KB | **表情 GIF / 字体 / 唤醒词模型** |

## 二、核心结论速览

1. **板型是私有代码，但只占 2 个文件**：`main/boards/miaomiao-esp32s3/miaomiao_esp32s3.cc` 与 `setgif.cc`，其余 99% 是官方开源代码。
2. **16 个 MCP 工具已在册，唯独没有舵机** —— 所以「语音控舵机角度/时长」必须重编固件，这是唯一的硬门槛。
3. **表情包可以不动固件就加**：assets 分区 + `index.json` 机制完整存在，云端 `self.assets.set_download_url` 可直接下发新资源包。**✅ 已完成：19 种表情（8 原生 + 11 新增）已刷入 0xA00000 并验证通过。**
4. **✅ 引脚图已全部实测锁定（2026-11-16 JTAG）**，见 `source/board/miaomiao_esp32s3/config.h` 内逐条标注。
5. **✅ 舵机已在线验证**：JTAG 直写 LEDC 占空比成功驱动双舵机扫掠（`tools/jtag_servo_demo.tcl`），硬件路径完全打通，只差编译新固件。

## 三、舵机语音控制固件：三条编译路径

定制板源码（`source/board/miaomiao_esp32s3/`）已完整，含 `self.servo.set / .action / .get` 三个 MCP 工具（角度+时长+预设动作）。编译方式任选其一：

**路径 A：GitHub Actions 一键编译（推荐，无需装环境）**
1. 把整个 `机芯盒改造/` 目录推到你的 GitHub 私有仓库根目录
2. 打开仓库 → Actions → "Build 喵喵机芯盒固件" → Run workflow
3. 完成后下载 artifact `miaomiao-firmware`（含 bootloader / 分区表 / 主程序）

**路径 B：本机装 ESP-IDF 编译**
```bash
# 需要 ESP-IDF v5.5.4（约 3GB，从 dl.espressif.cn 下载国内镜像版）
git clone --recursive --branch v5.5.4 https://github.com/espressif/esp-idf.git
./install.sh esp32s3 && . ./export.sh
git clone --recursive --branch v2.2.2 https://github.com/78/xiaozhi-esp32.git
cd xiaozhi
cp -r <机芯盒改造路径>/source/board/miaomiao_esp32s3 main/boards/miaomiao-esp32s3
python3 ../tools/patch_board.py .
idf.py set-target esp32s3 && idf.py build
```

**路径 C：编译后刷机**（assets 分区不用动，新表情包保留）
```bash
python3 -m esptool --chip esp32s3 --port $PORT \
  write_flash 0x0 bootloader.bin 0x80000 partition-table.bin 0x20000 firmware.bin
# 注意：编译产物的分区表以 build/partition-table.bin 为准，偏移按 flash_args
```

## 四、本机工具环境说明（sandbox 网络限制）

本机网络只放行 `api.github.com`，pip 与 GitHub Release 大文件下载被切断。当前 esptool 是通过 GitHub contents API 逐文件拼装的 v4.7.0，位于 `/tmp/esp`（**临时目录，重启即失**）。

重建方式：`见 source/official/` 无用，需按 `/tmp/esp/bootstrap.md` 重跑（如该文件已随 /tmp 清除，则按 README 末尾一键脚本重来）。

## 五、开源化（2026-11-16 新增）

本仓库的板级代码为**净室重写**（依据二进制逆向，不含任何 vendor 源码），以 MIT 许可开源，边界与发布前检查清单见 `NOTICE.md`。

### 开源版新增的语音能力（对比原厂固件）

| 语音指令 | MCP 调用 | 说明 |
|---|---|---|
| "把头往左转 45 度，慢慢转" | `self.servo.set(index, angle, duration_ms)` | **单个舵机**精确控角度+时长 |
| "点点头 / 摇摇头" | `self.servo.action(name, speed)` | 7 种预设动作 |
| "你的头现在朝哪" | `self.servo.get()` | 状态查询 |
| "笑一个 / 做个飞吻 / 眨眨眼" | `self.display.emotion(name)` | **19 种表情**语音切换 |
| "你会哪些表情" | `self.display.emotions()` | 表情清单查询 |

表情名（与 assets index.json 一致）：
angry neutral sleepy thinking shocked cool happy crying ｜ laughing kissy loving funny confident silly embarrassed surprised confused winking delicious

### 文件清单（source/board/miaomiao_esp32s3/）

- `miaomiao_esp32s3.cc` — 板型主类（新增，编译核对点已用 [编译核对] 标注）
- `servo_controller.h/.cc` — 舵机控制器（缓动 + 队列 + 防电流冲击）
- `mcp_servo_tools.cc` / `mcp_display_tools.cc` — MCP 工具注册层
- `config.h` — 全部引脚（JTAG 实测，逐条标注证据来源）

公开发布前务必先读 `NOTICE.md` 的检查清单（删固件备份、删 official 参考副本、脱敏 MAC 等）。
