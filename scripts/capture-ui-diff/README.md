# 批量截取 UI 基线

`capture.py` 从 `docs/ui-baseline.md` 的截图矩阵读取 `SM-01` 至 `SM-135`，按表格顺序执行。需要 Python 3.9 或更新版本。原型使用 Playwright Chromium，APP 使用当前操作系统的 Harbor DEBUG 构建。

```sh
python3 -m pip install 'playwright>=1.45'
python3 -m playwright install chromium
python3 scripts/capture-ui-diff/capture.py all
```

macOS 默认使用机器上的 Google Chrome 和 `macos/.build/debug/Harbor`。`--chrome` 可指定浏览器二进制，`--app` 可指定 Harbor.app 或 DEBUG 二进制。APP 需从能访问 WindowServer 的会话启动。每个场景使用临时 Vault、`HARBOR_DEMO=1`、文件 DEK 和 `scripts/dev-mac.sh`。当前 macOS 的 `HARBOR_DEMO` 不注入主机，主机相关场景通过已有 `mkobj` 接口建立临时数据。

```sh
python3 scripts/capture-ui-diff/capture.py all --app ~/harbor-build/Harbor.app
python3 scripts/capture-ui-diff/capture.py proto SM-09 SM-67
```

Linux 默认使用 `build/linux/harbor`。先加载 `scripts/env.sh` 所需的构建环境，并安装 Xvfb、xauth、xdotool 和 D-Bus 命令行工具。脚本使用私有 X11 显示与 D-Bus 会话，按场景调用 `--screenshot` 或 `dev-ui` 和 `shot` 动作。

```sh
source scripts/env.sh
python3 scripts/capture-ui-diff/capture.py app
```

主 PNG 为 1400×900，输出到 `.shots/proto/`、`.shots/mac/` 或 `.shots/linux/`。可选 `--out` 指定其他目录。每个 ID 保存一个主状态，涉及多步的行保存其中一个代表帧。原型 `SM-80` 在 1400×900 画布内嵌入 900px 宽视口以触发响应式断点。Android 专属场景使用原型的 Android 模式。macOS 的调试接口会为部分弹窗另外生成 `<ID>.png.sheet.png`。

`.shots/manifest.json` 为每个已执行的目标记录 `captured` 或 `blocked`，`.shots/blocked.md` 按原因列出 ID。重跑某些 ID 会更新对应目标的结果，并保留其余记录。只要本次有阻塞，命令就返回退出码 1。未验证的驱动步骤和本地服务夹具列为阻塞，可从清单定位后续工作。
