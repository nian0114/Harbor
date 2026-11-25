#!/usr/bin/env python3
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SIZE = (1400, 900)
PLATFORM = {"Darwin": "mac", "Linux": "linux"}.get(platform.system())
ANDROID = {3, 10, 19, 21, *range(71, 80), 86, 54, 119, 131, 132}
CHROME = Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")


class Blocked(Exception):
    pass


class AppLaunchBlocked(Blocked):
    pass


PROTOTYPE_BLOCKED = {
    112: "浏览器原生 prompt 位于网页渲染区域之外，Playwright 网页截图无法截取",
    119: "Android 原型终端辅助键条没有 Snippets、主题和 Autocomplete 入口",
}


def read_matrix(path):
    matrix = path.read_text(encoding="utf-8").split("## 截图矩阵表\n", 1)[-1]
    rows = re.findall(r"^\| (SM-\d+) ([^|]+)\|\s*([^|]+)\|", matrix, re.MULTILINE)
    expected = [f"SM-{n:02d}" for n in range(1, 136)]
    if [row[0] for row in rows] != expected:
        raise ValueError(f"{path}: expected the ordered SM-01 through SM-135 matrix")
    return {key: {"title": title.strip(), "trigger": trigger.strip()} for key, title, trigger in rows}


def png_size(path):
    if path.is_file():
        with path.open("rb") as image:
            header = image.read(24)
        if len(header) == 24 and header[:16] == b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
            return struct.unpack(">II", header[16:24])
    raise Blocked("截图接口未生成有效 PNG")


class Prototype:
    def __init__(self, stack, chrome, mode):
        from playwright.sync_api import sync_playwright

        playwright = stack.enter_context(sync_playwright())
        self.browser = playwright.chromium.launch(executable_path=chrome)
        stack.callback(self.browser.close)
        self.mode = mode
        self.recipe = (HERE / "prototype.js").read_text(encoding="utf-8")

    def capture(self, n, output):
        mode = "android" if n in ANDROID else {1: "mac", 2: "linux"}.get(n, self.mode)
        url = (ROOT / "prototype/index.html").as_uri()
        context = self.browser.new_context(viewport={"width": SIZE[0], "height": SIZE[1]},
                                           device_scale_factor=1, timezone_id="Asia/Shanghai")
        try:
            page = context.new_page()
            page.set_default_timeout(5000)
            page.clock.set_fixed_time(datetime(2026, 11, 17, 14, 58, tzinfo=timezone.utc))
            page.goto(url)
            frame = page.main_frame
            if n == 80:
                # The responsive breakpoint needs a 900 px viewport inside the fixed output canvas.
                page.set_content(f'<body style="margin:0;background:#e9edf1;text-align:center">'
                                 f'<iframe src="{url}" width="900" height="900" style="border:0"></iframe>')
                frame = page.locator("iframe").element_handle().content_frame()
                frame.wait_for_load_state()
            frame.wait_for_function("typeof window.A === 'object'")
            frame.evaluate(f"opts => ({self.recipe})({{...opts, advance: ms => new Promise(r => setTimeout(r, ms))}})",
                           {"id": n, "mode": mode})
            page.screenshot(path=output, caret="hide", scale="css")
            return {"mode": mode}
        finally:
            context.close()


def app_env(data):
    return dict(os.environ, HARBOR_DATA_DIR=str(data), HARBOR_DEMO="1", HARBOR_DEV_ACTIONS="1",
                HARBOR_DEK_FILE="1", XDG_CONFIG_HOME=str(data / "config"),
                XDG_DATA_HOME=str(data / "xdg"), XDG_CACHE_HOME=str(data / "cache"), ZDOTDIR=str(data))


def run(args, env=None, timeout=12):
    result = subprocess.run(args, env=env, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise Blocked(f"{Path(args[0]).name}: {result.stderr.strip() or result.stdout.strip() or result.returncode}")
    return result.stdout.strip()


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def wait_for(check, reason, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if result := check():
            return result
        time.sleep(0.05)
    raise Blocked(reason)


def capture_mac(n, binary, data, output):
    from native import mac_recipe

    env = app_env(data)

    def dev(command):
        result = run([str(ROOT / "scripts/dev-mac.sh"), command], env)
        if not result or result.startswith("ERR") or "\nTIMEOUT" in result:
            raise Blocked(f"macOS {command}: {result or 'dev.sock 没有响应'}")
        return result.removeprefix("OK ")

    with tempfile.TemporaryFile() as logs:
        process = subprocess.Popen([binary, "-harbor.language", "zh_CN"], cwd=ROOT, env=env,
                                   stdin=subprocess.DEVNULL, stdout=logs, stderr=subprocess.STDOUT)
        try:
            def ready():
                if process.poll() is not None:
                    raise Blocked(f"Harbor 退出，进程状态为 {process.returncode}")
                return (data / "dev.sock").exists() and dev("state") != "{}"

            try:
                wait_for(ready, "Harbor DEBUG 调试窗口或 dev.sock 未就绪")
            except (Blocked, subprocess.TimeoutExpired) as error:
                logs.seek(0)
                details = logs.read(4096).decode(errors="replace").strip().splitlines()
                raise AppLaunchBlocked(f"macOS APP 启动失败: {error}" + (f" ({details[0]})" if details else "")) from error
            dev("resize 1400 900")
            extra = mac_recipe(n, dev, data)
            time.sleep(0.2)
            result = dev(f"snapshot {output}")
            if result.endswith("+sheet"):
                png_size(Path(f"{output}.sheet.png"))
            if extra == "sheet" and not result.endswith("+sheet"):
                raise Blocked("当前调试入口未打开可截图的 sheet")
            actual = png_size(output)
            if actual == tuple(pixels * 2 for pixels in SIZE):
                run(["sips", "-z", "900", "1400", str(output)])
        finally:
            stop(process)
    return {}


def capture_linux(n, binary, data, output):
    from native import LINUX, LINUX_ACTIONS

    actions = LINUX_ACTIONS.get(n, ())
    for command in ("xvfb-run", "xdotool", "dbus-run-session", "gdbus", "timeout"):
        if not shutil.which(command):
            raise AppLaunchBlocked(f"Linux 截图依赖缺失: {command}")
    env = dict(app_env(data), GDK_BACKEND="x11")
    script = '''
set -eu
output=$1
action_count=$2
shift 2
actions=("${@:1:action_count}")
shift "$action_count"
if [ "$action_count" -eq 0 ]; then
    set -- "$@" "--screenshot=$output" --screenshot-delay=2400
fi
"$@" &
app=$!
trap 'kill "$app" 2>/dev/null || true' EXIT
window=$(xdotool search --sync --onlyvisible --pid "$app" | head -n 1)
xdotool windowsize --sync "$window" 1400 900
if [ "$action_count" -eq 0 ]; then
    wait "$app"
else
    gdbus wait --session --timeout 6 dev.harbor.Harbor
    call_action() {
        gdbus call --session --dest dev.harbor.Harbor --object-path /dev/harbor/Harbor \
            --method org.gtk.Actions.Activate "$1" "[<$2>]" "{}" >/dev/null
    }
    for step in "${actions[@]}"; do
        call_action "${step%%=*}" "${step#*=}"
    done
    sleep 0.5
    call_action shot "'$output'"
fi
'''
    run(["timeout", "18", "dbus-run-session", "--", "xvfb-run", "-a", "-s", "-screen 0 1400x900x24",
         "bash", "-c", script, "capture", str(output), str(len(actions)), *actions, str(binary),
         *LINUX.get(n, ())], env, timeout=22)
    return {}


def app_blocker(n, target):
    from native import MAC, MAC_BLOCKED, LINUX, LINUX_ACTIONS

    if n in ANDROID:
        return "基线要求 Android 原生界面，当前为桌面 APP"
    if (n, target) in {(1, "linux"), (2, "mac")}:
        return "基线指定另一桌面平台"
    if n == 80:
        return "该断点要求窗口宽度不超过 900，APP 截图固定窗口宽度为 1400"
    if target == "mac" and n not in MAC:
        return MAC_BLOCKED.get(n, "现有 macOS 调试入口缺少该状态的已验证驱动步骤")
    if target == "linux" and n not in LINUX and n not in LINUX_ACTIONS:
        return "现有 Linux 调试入口缺少该状态的已验证驱动步骤"
    return None


def capture_one(n, output, capture):
    sheet = Path(f"{output}.sheet.png")
    output.unlink(missing_ok=True)
    sheet.unlink(missing_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="hb-shot-", dir="/tmp") as directory:
            data = Path(directory)
            staged = data / "shot.png"
            extra = capture(n, staged, data)
            actual = png_size(staged)
            if actual != SIZE:
                raise Blocked(f"PNG 实际尺寸 {actual[0]}×{actual[1]}，要求 1400×900")
            shutil.move(staged, output)
            companion = Path(f"{staged}.sheet.png")
            if companion.is_file():
                shutil.move(companion, sheet)
                extra["sheet"] = str(sheet.relative_to(output.parents[1]))
        return {"status": "captured", "path": str(output.relative_to(output.parents[1])), **extra}
    except Exception as error:
        output.unlink(missing_ok=True)
        sheet.unlink(missing_ok=True)
        reason = str(error).strip().splitlines()
        return {"status": "blocked", "reason": reason[0] if reason else type(error).__name__}


def write_blocked(out, matrix):
    groups = defaultdict(list)
    for scene_id, entry in matrix.items():
        for target in ("proto", "mac", "linux"):
            result = entry.get(target, {})
            if result.get("status") == "blocked":
                groups[target, result["reason"]].append(scene_id)
    lines = ["# 截图阻塞清单", ""]
    for (target, reason), ids in sorted(groups.items()):
        lines.extend((f"- **{target} · {len(ids)} 项** {reason}", f"  {', '.join(ids)}"))
    if not groups:
        lines.append("已执行的目标均已生成截图。")
    pending = out / "blocked.tmp"
    pending.write_text("\n".join(lines) + "\n", encoding="utf-8")
    pending.replace(out / "blocked.md")


def main():
    from contextlib import ExitStack

    parser = argparse.ArgumentParser(description="Capture the Harbor UI baseline matrix.")
    parser.add_argument("target", choices=("all", "proto", "app"))
    parser.add_argument("ids", nargs="*", metavar="SM-ID")
    parser.add_argument("--baseline", type=Path, default=ROOT / "docs/ui-baseline.md")
    parser.add_argument("--out", type=Path, default=ROOT / ".shots")
    parser.add_argument("--app", type=Path, help="path to the native debug executable or macOS .app bundle")
    parser.add_argument("--chrome", default=str(CHROME) if CHROME.is_file() else None)
    parser.add_argument("--prototype-mode", choices=("mac", "linux"), default=PLATFORM or "mac")
    args = parser.parse_intermixed_args()
    if args.target != "proto" and PLATFORM is None:
        parser.error("native capture requires macOS or Linux")
    try:
        matrix = read_matrix(args.baseline)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    unknown = set(args.ids) - matrix.keys()
    if unknown:
        parser.error(f"unknown scene IDs: {', '.join(sorted(unknown))}")
    default_app = ROOT / ("macos/.build/debug/Harbor" if PLATFORM == "mac" else "build/linux/harbor")
    binary = (args.app or default_app).resolve()
    if binary.suffix == ".app":
        binary = binary / "Contents/MacOS/Harbor"

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with (out / ".capture.lock").open("a") as lock, ExitStack() as stack:
        fcntl.flock(lock, fcntl.LOCK_EX)
        manifest = out / "manifest.json"
        previous = json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else {}
        scenes = previous.get("scenes", {})
        for key, entry in matrix.items():
            entry.update({target: result for target, result in scenes.get(key, {}).items()
                          if target in ("proto", "mac", "linux")})
        targets = ["proto", PLATFORM] if args.target == "all" else ["proto" if args.target == "proto" else PLATFORM]
        startup_errors = {}
        if "proto" in targets:
            try:
                prototype = Prototype(stack, args.chrome, args.prototype_mode)
            except Exception as error:
                lines = str(error).splitlines()
                for line in lines:
                    if "<process did exit:" in line:
                        lines[0] += " " + line.strip().split("<process did exit: ", 1)[1].rstrip(">")
                        break
                startup_errors["proto"] = lines[0] if lines else type(error).__name__

        def capture(n, output, data):
            if target == "proto":
                if n in PROTOTYPE_BLOCKED:
                    raise Blocked(PROTOTYPE_BLOCKED[n])
                if target in startup_errors:
                    raise Blocked(startup_errors[target])
                return prototype.capture(n, output)
            if reason := app_blocker(n, target):
                raise Blocked(reason)
            if target in startup_errors:
                raise Blocked(startup_errors[target])
            if not os.access(binary, os.X_OK):
                raise Blocked(f"找不到可执行的 {target} APP: {binary}")
            backend = capture_mac if target == "mac" else capture_linux
            try:
                return backend(n, binary, data, output)
            except AppLaunchBlocked as error:
                startup_errors[target] = str(error)
                raise

        summary = Counter()
        for key, scene in matrix.items():
            if args.ids and key not in args.ids:
                continue
            for target in targets:
                path = out / target / f"{key}.png"
                path.parent.mkdir(exist_ok=True)
                result = capture_one(int(key[3:]), path, capture)
                scene[target] = result
                summary[target, result["status"]] += 1
                print(f"{key} {target} {result['status']} {result.get('reason', '')}", flush=True)
            pending = manifest.with_suffix(".tmp")
            pending.write_text(json.dumps({"baseline": str(args.baseline.resolve().relative_to(ROOT))
                                           if args.baseline.resolve().is_relative_to(ROOT) else str(args.baseline.resolve()),
                                           "viewport": list(SIZE), "scenes": matrix},
                                          ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            pending.replace(manifest)
        write_blocked(out, matrix)
        for (target, status), count in summary.items():
            print(f"{target}: {count} {status}")
        return int(any(status == "blocked" for _, status in summary))


if __name__ == "__main__":
    raise SystemExit(main())
