# NOTICE — 代码来源与版权边界

本仓库由三部分组成，法律边界各不相同：

## 1. 本仓库原创代码（MIT，见 LICENSE）

- `source/board/miaomiao_esp32s3/` 全部文件：
  - `miaomiao_esp32s3.cc` — 板型主类（净室重写）
  - `servo_controller.h/.cc` — 舵机控制器（角度+时长双参数）
  - `mcp_servo_tools.cc` — 舵机语音控制 MCP 工具
  - `mcp_display_tools.cc` — 表情语音控制 MCP 工具
  - `config.h` — 引脚定义（JTAG 实测得来，不含 vendor 代码）
- `tools/`：repack_assets.py、make_emojis.py、patch_board.py、jtag_servo_demo.tcl
- `.github/workflows/build.yml`

## 2. 官方开源代码的参考副本（版权归 xiaozhi-esp32 作者，遵循其原许可证）

- `source/official/` 下所有文件仅为开发时的对照参考，**不构成发布物**。
  公开发布时建议直接声明依赖 `github.com/78/xiaozhi-esp32` v2.2.2，不打包这些副本。

## 3. 不包含 vendor（东莞创贝乐）固件二进制以外的任何厂商代码

- `firmware/` 下的 bin 文件是用户本人设备的固件备份，仅供该设备恢复使用，
  **不得再分发**。公开发布本仓库时应删除该目录。
- 表情资源 `assets_new.bin` 中的原厂 8 个表情版权归厂商，公开发布时需替换或删除；
  自制的 11 个表情（`make_emojis.py` 生成）可随 MIT 代码发布。

## 开源发布检查清单（推公开仓库前核对）

- [ ] 删除 `firmware/` 目录（固件备份属私有）
- [ ] 删除 `source/official/`（改为 README 里声明依赖官方仓库）
- [ ] `逆向分析报告.html`、`引脚反推工作表.html` 如含 MAC 地址等私有信息先脱敏
- [ ] README 顶部加一句：与原厂无关、非官方固件、刷机自负风险
