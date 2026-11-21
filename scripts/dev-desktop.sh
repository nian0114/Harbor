#!/usr/bin/env bash
# Harbor 开发用 VNC/RDP 桌面（无需 root）。
#
#   scripts/dev-desktop.sh start    下载/解包工具并拉起桌面
#   scripts/dev-desktop.sh stop     停掉 Xvnc 与 freerdp-shadow
#   scripts/dev-desktop.sh status   查看运行状态
#
# 端点（都只绑 127.0.0.1）：
#   VNC  127.0.0.1:5901   VncAuth 密码 pw123        (Xvnc :55, 1920x1080)
#   RDP  127.0.0.1:13389   NLA, user=harbor / pw123  (freerdp-shadow-cli 共享 :55)
#
# 工具链：apt-get download 拿 .deb → dpkg -x 解到 ~/.local/harbor-tools；
# 动态库用 harbor-tools + harbor-sysroot + 系统库拼 LD_LIBRARY_PATH。
# 运行状态在 /tmp/harbor-dev-desktop（pid/passwd/sam/cert）。
set -uo pipefail

DIR="${HARBOR_DEV_DESKTOP_DIR:-/tmp/harbor-dev-desktop}"
TOOLS="${HARBOR_TOOLS:-$HOME/.local/harbor-tools}"
DEBCACHE="${HARBOR_DEBCACHE:-$HOME/.cache/harbor-debs}"
SYSROOT="${HARBOR_SYSROOT:-$HOME/.local/harbor-sysroot}"
VNC_PORT=5901
RDP_PORT="${HARBOR_DEV_RDP_PORT:-13389}"   # 3389 被 gnome-remote-desktop 占着
DISP=":55"

# 运行时库搜索路径：tools 的 deb 库 → sysroot（freerdp3 全家）→ 系统库。
libpath() {
    echo "$TOOLS/usr/lib/x86_64-linux-gnu:$SYSROOT/usr/lib/x86_64-linux-gnu:$SYSROOT/runtime-libs"
}
BINDIR="$TOOLS/usr/bin"

SEEDS=(
    tigervnc-standalone-server   # Xtigervnc
    tigervnc-tools               # tigervncpasswd
    tigervnc-common              # Xtigervnc 的字体/资源
    freerdp3-shadow-x11          # freerdp-shadow-cli3
    winpr3-utils                 # winpr-hash3 / winpr-makecert3
    xterm                        # 桌面上的终端（输入验证用）
)

# 需要个能在桌面上看/敲的简单 X 客户端；系统没有就下载 x11-apps。
command -v xclock >/dev/null 2>&1 || command -v xeyes >/dev/null 2>&1 \
    || SEEDS+=(x11-apps)

fetch_tools() {
    local p f ok
    mkdir -p "$DEBCACHE" "$TOOLS"
    for p in "${SEEDS[@]}"; do
        f=$(ls "$DEBCACHE"/${p}_*.deb 2>/dev/null | head -1)
        if [ -z "$f" ]; then
            (cd "$DEBCACHE" && apt-get download "$p") >/dev/null 2>&1 \
                || { echo "apt-get download failed: $p" >&2; exit 1; }
            f=$(ls "$DEBCACHE"/${p}_*.deb | head -1)
        fi
        # 幂等：解过一次就跳过（工具树里放个标记即可，dpkg -x 本来也幂等但慢）
        [ -f "$TOOLS/.done-$p" ] && continue
        dpkg -x "$f" "$TOOLS" && touch "$TOOLS/.done-$p"
    done
    # 缺的运行库按 ldd 检查补（一般 sysroot 已齐）
    for b in Xtigervnc freerdp-shadow-cli3 winpr-hash3; do
        ok=$(LD_LIBRARY_PATH="$(libpath)" ldd "$BINDIR/$b" 2>/dev/null | grep -c "not found" || true)
        if [ "${ok:-0}" -gt 0 ]; then
            echo "提示: $b 有 $ok 个未解析库（多半要补 deb）:" >&2
            LD_LIBRARY_PATH="$(libpath)" ldd "$BINDIR/$b" | grep "not found" >&2
        fi
    done
}

start() {
    fetch_tools
    mkdir -p "$DIR"; chmod 700 "$DIR"

    # VncAuth 密码文件
    if [ ! -f "$DIR/vnc.passwd" ]; then
        echo pw123 | LD_LIBRARY_PATH="$(libpath)" \
            "$BINDIR/tigervncpasswd" -f > "$DIR/vnc.passwd" 2>/dev/null \
            || { echo "tigervncpasswd failed" >&2; exit 1; }
        chmod 600 "$DIR/vnc.passwd"
    fi

    # RDP SAM（winpr-hash 输出 pwdump 行）
    if [ ! -f "$DIR/sam" ]; then
        LD_LIBRARY_PATH="$(libpath)" \
            "$BINDIR/winpr-hash3" -u harbor -p pw123 -f sam > "$DIR/sam" \
            || { echo "winpr-hash failed" >&2; exit 1; }
        chmod 600 "$DIR/sam"
    fi

    # Xvnc
    if [ -f "$DIR/xvnc.pid" ] && kill -0 "$(cat "$DIR/xvnc.pid")" 2>/dev/null; then
        echo "Xvnc already running (pid $(cat "$DIR/xvnc.pid"), 127.0.0.1:$VNC_PORT)"
    else
        LD_LIBRARY_PATH="$(libpath)" "$BINDIR/Xtigervnc" \
            "$DISP" -rfbport "$VNC_PORT" -localhost \
            -geometry 1920x1080 -depth 24 \
            -SecurityTypes VncAuth -rfbauth "$DIR/vnc.passwd" \
            -desktop harbor-dev \
            > "$DIR/xvnc.log" 2>&1 &
        echo $! > "$DIR/xvnc.pid"
        sleep 2
        kill -0 "$(cat "$DIR/xvnc.pid")" 2>/dev/null \
            || { echo "Xvnc failed, log:" >&2; cat "$DIR/xvnc.log" >&2; exit 1; }
        echo "Xvnc started (pid $(cat "$DIR/xvnc.pid"), 127.0.0.1:$VNC_PORT, $DISP)"

        # 找个能看/敲的 X 客户端：xterm（typing 验证，tools 下载）+
        # x11-apps 的 xclock + xeyes（系统或 tools 目录）
        for c in xterm xclock xeyes; do
            local_bin="$BINDIR/$c"
            if [ -x "$local_bin" ]; then
                DISPLAY="$DISP" LD_LIBRARY_PATH="$(libpath)"                     "$local_bin" >/dev/null 2>&1 &
            elif command -v "$c" >/dev/null 2>&1; then
                DISPLAY="$DISP" "$c" -geometry 200x200+100+100 >/dev/null 2>&1 &
            fi
        done
    fi

    # freerdp-shadow-cli（共享 :55）
    if [ -f "$DIR/rdp.pid" ] && kill -0 "$(cat "$DIR/rdp.pid")" 2>/dev/null; then
        echo "freerdp-shadow already running (pid $(cat "$DIR/rdp.pid"), 127.0.0.1:$RDP_PORT)"
    else
        DISPLAY="$DISP" LD_LIBRARY_PATH="$(libpath)" \
            "$BINDIR/freerdp-shadow-cli3" \
            /port:"$RDP_PORT" /bind-address:127.0.0.1 \
            /sec:nla /sam-file:"$DIR/sam" \
            > "$DIR/rdp.log" 2>&1 &
        echo $! > "$DIR/rdp.pid"
        sleep 2
        kill -0 "$(cat "$DIR/rdp.pid")" 2>/dev/null \
            || { echo "freerdp-shadow failed, log:" >&2; cat "$DIR/rdp.log" >&2; exit 1; }
        echo "freerdp-shadow started (pid $(cat "$DIR/rdp.pid"), 127.0.0.1:$RDP_PORT)"
    fi

    echo
    echo "  VNC  127.0.0.1:$VNC_PORT   VncAuth 密码: pw123   ($DISP 1920x1080)"
    echo "  RDP  127.0.0.1:$RDP_PORT   NLA  user=harbor  pass=pw123  (共享 $DISP)"
    echo "  logs       : $DIR/xvnc.log $DIR/rdp.log"
}

stop() {
    for k in xvnc rdp; do
        if [ -f "$DIR/$k.pid" ]; then
            kill "$(cat "$DIR/$k.pid")" 2>/dev/null && echo "$k stopped"
            rm -f "$DIR/$k.pid"
        fi
    done
}

status() {
    for k in xvnc rdp; do
        if [ -f "$DIR/$k.pid" ] && kill -0 "$(cat "$DIR/$k.pid")" 2>/dev/null; then
            echo "$k: running (pid $(cat "$DIR/$k.pid"))"
        else
            echo "$k: stopped"
        fi
    done
    ss -tln 2>/dev/null | grep -E ":($VNC_PORT|$RDP_PORT) " || true
}

case "${1:-}" in
    start)  start ;;
    stop)   stop ;;
    status) status ;;
    *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
