#!/usr/bin/env python3
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import fcntl
import hashlib
import select
import base64
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
PLATFORM = {"Darwin": "macos", "Linux": "linux"}.get(platform.system())
ANDROID = {3, 10, 19, 21, *range(71, 80), 86, 54, 119, 131, 132}
CHROME = Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")


class Blocked(Exception):
    pass


class AppLaunchBlocked(Blocked):
    pass


PROTOTYPE_BLOCKED = {
    112: "浏览器原生 prompt 位于网页渲染区域之外，CDP 页面截图无法截取",
    119: "Android 原型终端辅助键条没有 Snippets、主题和 Autocomplete 入口",
}


def read_matrix(path):
    matrix = path.read_text(encoding="utf-8").split("## 截图矩阵表\n", 1)[-1]
    rows = []
    details = {}
    for line in matrix.splitlines():
        if not re.match(r"^\| SM-\d+ ", line):
            continue
        cells = re.split(r"\s+\|\s+", line.strip().strip("|").strip())
        if len(cells) != 5:
            raise ValueError(f"{path}: malformed screenshot matrix row: {line}")
        key, title = cells[0].split(" ", 1)
        rows.append((key, title, cells[1]))
        details[key] = {"baseline_viewport": cells[2], "theme": cells[3], "source": cells[4]}
    expected = [f"SM-{n:02d}" for n in range(1, 136)]
    if [row[0] for row in rows] != expected:
        raise ValueError(f"{path}: expected the ordered SM-01 through SM-135 matrix")
    return {key: {"title": title.strip(), "trigger": trigger.strip(), **details[key]} for key, title, trigger in rows}


def png_size(path):
    if path.is_file():
        with path.open("rb") as image:
            header = image.read(24)
        if len(header) == 24 and header[:16] == b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
            return struct.unpack(">II", header[16:24])
    raise Blocked("截图接口未生成有效 PNG")


class Prototype:
    """Own one headless Chrome and exchange line-delimited requests with CDP."""
    def __init__(self, stack, chrome, mode, foreground=False):
        self.mode = mode
        if not shutil.which("node"):
            raise Blocked("原型截图依赖缺失: Node.js (>=18)")
        if not chrome or not Path(chrome).is_file():
            raise Blocked("找不到 Chrome/Chromium，请设置 --chrome")
        self.logs = stack.enter_context(tempfile.TemporaryFile(mode="w+"))
        self.process = subprocess.Popen(
            ["node", str(HERE / "prototype.mjs"), str(chrome), str(ROOT),
             "foreground" if foreground else "headless"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.logs, text=True)
        stack.callback(self.close)
        result = self.receive(20)
        if not result.get("ready"):
            raise Blocked(result.get("error", "Chrome 调试管道启动失败"))

    def receive(self, timeout=20):
        if not select.select([self.process.stdout], [], [], timeout)[0]:
            raise Blocked("Chrome 调试驱动超时")
        line = self.process.stdout.readline()
        if not line:
            self.logs.seek(0)
            raise Blocked("Chrome 调试驱动已退出: " + self.logs.read(2000))
        return json.loads(line)

    def capture(self, n, output):
        mode = "android" if n in ANDROID else {1: "mac", 2: "linux"}.get(n, self.mode)
        self.process.stdin.write(json.dumps(dict(id=n, mode=mode, output=str(output))) + "\n")
        self.process.stdin.flush()
        result = self.receive(30)
        if not result.get("ok"):
            raise Blocked(result.get("error", "Chrome screenshot failed"))
        return {key: value for key, value in result.items() if key != "ok"}

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write('{"quit":true}\n')
                self.process.stdin.flush()
                self.process.wait(timeout=4)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                stop(self.process)


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


def capture_mac(n, binary, data, output, foreground=False):
    from native import mac_recipe

    env = app_env(data)
    commands = []

    def dev(command):
        commands.append(command)
        verb, _, rest = command.partition(" ")
        if verb == "dialog-fill":
            field, _, text = rest.partition(" ")
            rest = field + " " + base64.b64encode(text.encode()).decode()
        result = run([str(ROOT / "scripts/dev-mac.sh"), verb, rest], env)
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
            if foreground:
                dev("activate")
            extra = mac_recipe(n, dev, data)
            time.sleep(0.2)
            result = dev(f"snapshot {output}")
            if result.endswith("+sheet"):
                png_size(Path(f"{output}.sheet.png"))
            if extra == "sheet" and not result.endswith("+sheet"):
                dialogs = dev("dialogs")
                raise Blocked("snapshot 未输出所需弹窗；现有 hook 只覆盖主窗/attachedSheet，dialogs=" + dialogs)
            actual = png_size(output)
            if actual == tuple(pixels * 2 for pixels in SIZE):
                run(["sips", "-z", "900", "1400", str(output)])
        finally:
            stop(process)
    return {"commands": commands}


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
    if (n, target) in {(1, "linux"), (2, "macos")}:
        return "基线指定另一桌面平台"
    if n == 80:
        return "该断点要求窗口宽度不超过 900，APP 截图固定窗口宽度为 1400"
    if target == "macos" and n not in MAC:
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
                extra["sheet_note"] = "原生 hook 单独渲染 sheet；主 PNG 不含该 sheet，未猜测坐标合成。"
        artifacts = []
        for image in (output, sheet):
            if image.is_file():
                width, height = png_size(image)
                artifacts.append({"path": str(image.relative_to(output.parents[1])),
                                  "width": width, "height": height,
                                  "sha256": hashlib.sha256(image.read_bytes()).hexdigest()})
        return {"status": "captured", "path": str(output.relative_to(output.parents[1])),
                "artifacts": artifacts, **extra}
    except Exception as error:
        output.unlink(missing_ok=True)
        sheet.unlink(missing_ok=True)
        reason = str(error).strip().splitlines()
        return {"status": "blocked", "reason": reason[0] if reason else type(error).__name__}


def write_blocked(out, matrix):
    groups = defaultdict(list)
    for scene_id, entry in matrix.items():
        for target in ("proto", "macos", "linux"):
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
    parser.add_argument("--foreground", action="store_true", help="show Chrome and activate the isolated macOS APP")
    parser.add_argument("--baseline", type=Path, default=ROOT / "docs/dev/ui-baseline.md")
    parser.add_argument("--out", type=Path, default=ROOT / ".shots")
    parser.add_argument("--app", type=Path, help="path to the native debug executable or macOS .app bundle")
    parser.add_argument("--chrome", default=str(CHROME) if CHROME.is_file() else shutil.which("chromium") or shutil.which("google-chrome"))
    parser.add_argument("--prototype-mode", choices=("mac", "linux"), default="mac" if PLATFORM == "macos" else PLATFORM or "mac")
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
    default_app = ROOT / ("macos/.build/debug/Harbor" if PLATFORM == "macos" else "build/linux/harbor")
    binary = (args.app or default_app).resolve()
    if binary.suffix == ".app":
        binary = binary / "Contents/MacOS/Harbor"

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with (out / ".capture.lock").open("a") as lock, ExitStack() as stack:
        fcntl.flock(lock, fcntl.LOCK_EX)
        manifest = out / "manifest.json"
        previous = json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else {}
        baseline_sha = hashlib.sha256(args.baseline.read_bytes()).hexdigest()
        scenes = previous.get("scenes", {}) if previous.get("baseline_sha256") == baseline_sha else {}
        for key, entry in matrix.items():
            entry.update({target: result for target, result in scenes.get(key, {}).items()
                          if target in ("proto", "macos", "linux")})
        targets = ["proto", PLATFORM] if args.target == "all" else ["proto" if args.target == "proto" else PLATFORM]
        startup_errors = {}
        if "proto" in targets:
            try:
                prototype = Prototype(stack, args.chrome, args.prototype_mode, args.foreground)
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
            backend = capture_mac if target == "macos" else capture_linux
            try:
                return backend(n, binary, data, output, args.foreground) if target == "macos" else backend(n, binary, data, output)
            except AppLaunchBlocked as error:
                startup_errors[target] = str(error)
                raise

        # Initialize the whole requested batch before work. If interrupted, entries
        # must not continue to advertise an older run's successful screenshot.
        for key, scene in matrix.items():
            if not args.ids or key in args.ids:
                for target in targets:
                    scene[target] = {"status": "blocked", "reason": "run_incomplete: 本次尚未执行完成"}
        def persist():
            pending = manifest.with_suffix(".tmp")
            pending.write_text(json.dumps({"schema_version": 1,
                "baseline": str(args.baseline),
                "baseline_sha256": hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
                "viewport": list(SIZE), "foreground": args.foreground,
                "generated_at": datetime.now(timezone.utc).isoformat(),
                "git_revision": run(["git", "rev-parse", "HEAD"]),
                "scenes": matrix}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            pending.replace(manifest)
            write_blocked(out, matrix)
        persist()
        summary = Counter()
        for key, scene in matrix.items():
            if args.ids and key not in args.ids:
                continue
            for target in targets:
                path = out / target / f"{key}.png"
                path.parent.mkdir(exist_ok=True)
                result = capture_one(int(key[3:]), path, capture)
                result["attempted_at"] = datetime.now(timezone.utc).isoformat()
                scene[target] = result
                summary[target, result["status"]] += 1
                print(f"{key} {target} {result['status']} {result.get('reason', '')}", flush=True)
            persist()
        write_blocked(out, matrix)
        for (target, status), count in summary.items():
            print(f"{target}: {count} {status}")
        return int(any(status == "blocked" for _, status in summary))


if __name__ == "__main__":
    raise SystemExit(main())
