# 批量截取 UI 基线

统一入口为 `capture.py proto|app|all`。它读取 `docs/dev/ui-baseline.md` 的截图矩阵，校验完整且有序的 `SM-01` 至 `SM-135`。不会修改业务代码，也不进行视觉审计。

需要 Python 3.9+、Node.js 18+ 和 Chrome/Chromium；不需要安装 npm 包或 Playwright。原型通过 `prototype.mjs` 的 Chrome DevTools Protocol 管道执行 `prototype.js` 中的场景配方。APP 使用当前平台的 Harbor DEBUG 构建及现有调试接口。

```sh
python3 scripts/capture-ui-diff/capture.py all
# 前台启动 Chrome，并激活每个隔离的 macOS APP 实例：
python3 scripts/capture-ui-diff/capture.py all --foreground --app ~/harbor-build/Harbor.app
# 分别运行或只重跑指定 ID；保留其余场景结果：
python3 scripts/capture-ui-diff/capture.py proto SM-09 SM-67
python3 scripts/capture-ui-diff/capture.py app SM-11 --app ~/harbor-build/Harbor.app
```

`--chrome` 指定浏览器可执行文件；`--app` 指定 APP 二进制或 `.app`；`--out` 指定输出目录；`--baseline` 指定同格式场景表。默认 macOS APP 为 `macos/.build/debug/Harbor`，Linux 为 `build/linux/harbor`。

## 原型

浏览器视口和 PNG 固定为 1400×900、设备像素比为 1。每个场景重新加载页面，固定日期、时区、随机种子并逐步推进演示定时器；截图前固定动画进度和隐藏 caret，避免不同运行时刻影响任务/传输/Toast 帧。页面和状态均通过原型已有函数、输入与按钮触发。

矩阵中的 Android 专属场景使用原型 Android 模式；桌面主 PNG 仍为 1400×900。`SM-80` 在固定画布中嵌入真实的 900px iframe 以触发 CSS media query。一个场景行包含多个步骤或分支时，配方选取一个显式定义的代表帧，不声称穷举每个分支。原生浏览器 Prompt 无法出现在 CDP 的网页 PNG 中，记录为 blocked，不用 DOM 伪造。

## macOS APP

每个场景独立创建 `/tmp/hb-shot-*`，设置 `HARBOR_DEMO=1`、`HARBOR_DATA_DIR`、`HARBOR_DEV_ACTIONS=1`、`HARBOR_DEK_FILE=1`；结束后停止自己的进程并清理临时目录。XDG 目录和 `ZDOTDIR` 同样隔离。日常 Vault 不参与运行。

窗口通过 `scripts/dev-mac.sh resize 1400 900` 固定。当前 DEBUG APP 须包含 `ui-info` / `ui-click` / `ui-menu` / `snapshot-ui` / `snapshot-screen` 调试命令，详见 `docs/dev/ui-debug.md`。AppKit 的精确 2× backing-scale 截图归一为 1400×900；其余错误尺寸均阻塞。当前 macOS 不处理 `HARBOR_DEMO` 主机注入，配方使用现有 `mkobj` 接口在临时 Vault 中补入演示对象。

主 PNG 使用 `snapshot-ui` 按实际窗口坐标包含子窗口；部分原生 sheet 另存为 `<ID>.png.sheet.png`。原生菜单和系统文件选择器改用 ScreenCaptureKit 在 Harbor 主窗口边界内仅捕获本实例窗口（macOS 14+，需系统录屏权限）。脚本会排除其他应用窗口；系统权限由用户手动批准，脚本不自动修改。通知横幅另需 Harbor 通知权限。

截图运行额外设置 `HARBOR_CAPTURE_PANEL_DIR`，仅 DEBUG 钩子在原生文件选择器展示前将其初始目录定位到本次临时目录。macOS 录屏额外确认即使在 preflight 通过后也可能出现；需在前台处理后重跑受影响的 ID。系统通知截图额外包含与主窗相交的通知窗口，不存在可见横幅时保留 blocked。

APP 必须从可以连接 WindowServer 的会话运行。`--foreground` 会激活每个隔离实例。如果自动化会话无法启动 GUI，可从正常桌面 Terminal 启动专用运行器：

```sh
python3 scripts/capture-ui-diff/desktop.py --app ~/harbor-build/Harbor.app
```

默认 APP 即 `~/harbor-build/Harbor.app`。运行器只接受场景 ID，不接受 shell 命令；在 `.shots/control/` 写入 `request-001.tmp` 后原子重命名为 `request-001.json`，内容为 `{"ids":["SM-60","SM-118"]}`，空数组表示全量 macOS APP。每次请求使用不同编号，结果见 `result-001.json` 和 `.log`。通过 Ctrl-C 或创建 `.shots/control/stop` 停止；默认空闲 45 分钟退出。相同输出目录只允许一个运行器。

### 本地连接夹具

SSH/SFTP 使用系统 `/usr/sbin/sshd`、临时密钥与独立配置，监听随机 loopback 端口；不改 `~/.ssh`。延迟/限速 relay 提供可截取的连接与传输状态。VNC 使用真实 RFB 3.8 协议与合成测试画面。RDP 使用真实 FreeRDP TLS 服务，经实际 SSH 隧道连接；桌面与按键反馈是合成画面，不验证远端操作系统或 NLA。manifest 的 `fixtures` 记录这些来源和限制。

VNC/RDP 画面生成需要 Python Pillow。RDP 夹具复用已有 Harbor macOS SDK，无需新增 Homebrew 包；首次运行先构建：

```sh
python3 scripts/capture-ui-diff/build-fixtures.py
# 可选 --sysroot PATH --link-line PATH，默认分别为：
# ~/.local/harbor-macos/universal、~/harbor-build/link-line.txt
```

每场景结束时关闭夹具并清理临时数据。账号/设备/共享所需的 Harbor 同步服务独立于这些协议夹具；本轮按用户决定将相应 11 项保留为缺少测试服务。缺失入口和系统权限继续记录 blocked，不为截图修改业务代码。

## Linux APP

本轮仅验证 macOS，以下 Linux 路径保留供后续运行，未在本轮校验。先加载 `scripts/env.sh` 的构建环境。需要 Xvfb、xauth、xdotool、D-Bus、gdbus 和 timeout：

```sh
source scripts/env.sh
python3 scripts/capture-ui-diff/capture.py app
```

每场景使用私有 X11 显示和 D-Bus 会话、临时数据目录。通过 xdotool 将窗口设为 1400×900，再调用 `--screenshot` 或 D-Bus `dev-ui` / `shot`。PNG 尺寸必须匹配要求才算 captured。本机无法执行另一系统的 APP，Linux 配方须在 Linux 上运行验证。

## 结果与阻塞

输出为 `.shots/proto/<ID>.png`、`.shots/macos/<ID>.png` 或 `.shots/linux/<ID>.png`。这些产物在 `.gitignore` 中排除。

- `.shots/manifest.json`：135 个 ID 的标题、触发路径、基线视口、主题、来源，以及每个已执行目标的 `captured` / `blocked`、原因、执行时间。成功图片记录实际尺寸与 SHA-256；同时记录基线哈希、macOS 实际驱动命令、截图方式和协议夹具来源。失败项保留 traceback 和可获取的状态诊断。
- `.shots/blocked.md`：按平台及原因分组的完整阻塞 ID 清单。阻塞只说明截图运行条件或驱动限制，不等于已确认 APP 入口缺失。
- 重跑先将本批条目标记为 `run_incomplete`，逐场景原子更新，避免中断后误用上次成功状态；同一输出目录通过文件锁串行执行。基线哈希改变时不沿用旧结果。
- 运行失败会移除该 ID 的旧主图和 sheet，防止将旧图误认为新图；不会改动其他 ID/平台的结果。只要本批有 blocked，退出码为 1。

缺少本地 SSH/RDP/VNC/Relay 服务、场景驱动步骤或平台不适用时记录 blocked，等待决定补 dev hook、补测试服务还是标记入口缺失。
