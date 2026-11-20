#!/usr/bin/env bash
# Harbor 开发用用户态 sshd + ssh-agent（无需 root）。
#
#   scripts/dev-sshd.sh start   生成密钥并拉起 sshd(127.0.0.1:2222) 与 agent
#   scripts/dev-sshd.sh stop    停掉 sshd 与 agent
#   scripts/dev-sshd.sh env     打印 export SSH_AUTH_SOCK=...
#
# 用法：
#   scripts/dev-sshd.sh start
#   eval "$(scripts/dev-sshd.sh env)"
#   HARBOR_DEMO=1 ./build/linux/harbor --connect="$USER@127.0.0.1:2222"
#
# 密钥：client（无口令，已加 agent）与 client_enc（口令 pw123，
# 用于私钥口令对话框测试）都已写入 authorized_keys。
# 服务端开了 password/kbi 认证 → AUTO 无凭据时会触发密码对话框，
# 但用户态 sshd 无法校验密码（非 root），回复后必然 AUTH 失败 ——
# 正好用来截图错误卡片。

set -uo pipefail

DIR="${HARBOR_DEV_SSHD_DIR:-/tmp/harbor-dev-sshd}"
PORT="${HARBOR_DEV_SSHD_PORT:-2222}"
SSHD=/usr/sbin/sshd
KEYGEN=/usr/bin/ssh-keygen
AGENT=/usr/bin/ssh-agent
SSHADD=/usr/bin/ssh-add

need() { [ -x "$1" ] || { echo "missing: $1" >&2; exit 1; }; }

start() {
    need "$SSHD"; need "$KEYGEN"; need "$AGENT"; need "$SSHADD"
    mkdir -p "$DIR"; chmod 700 "$DIR"

    [ -f "$DIR/hostkey" ]    || "$KEYGEN" -t ed25519 -N ''      -f "$DIR/hostkey" -q
    [ -f "$DIR/hostkey2" ]   || "$KEYGEN" -t ed25519 -N ''      -f "$DIR/hostkey2" -q
    [ -f "$DIR/client" ]     || "$KEYGEN" -t ed25519 -N ''      -f "$DIR/client" -q
    [ -f "$DIR/client_enc" ] || "$KEYGEN" -t ed25519 -N 'pw123' -f "$DIR/client_enc" -q
    cat "$DIR/client.pub" "$DIR/client_enc.pub" > "$DIR/authorized_keys"

    cat > "$DIR/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $DIR/hostkey
PidFile $DIR/sshd.pid
AuthorizedKeysFile $DIR/authorized_keys
UsePAM no
StrictModes no
PasswordAuthentication yes
KbdInteractiveAuthentication yes
AllowTcpForwarding yes
PermitTTY yes
Subsystem sftp internal-sftp
LogLevel ERROR
EOF

    if [ -f "$DIR/sshd.pid" ] && kill -0 "$(cat "$DIR/sshd.pid")" 2>/dev/null; then
        echo "sshd already running (pid $(cat "$DIR/sshd.pid"), 127.0.0.1:$PORT)"
    else
        "$SSHD" -f "$DIR/sshd_config"   # 无 -D：sshd 自行 daemonize
        echo "sshd started (127.0.0.1:$PORT, log: $DIR/sshd.log 不存在时用 -e 自行排查)"
    fi

    if [ -f "$DIR/agent.pid" ] && kill -0 "$(cat "$DIR/agent.pid")" 2>/dev/null; then
        echo "ssh-agent already running (pid $(cat "$DIR/agent.pid"))"
    else
        out=$("$AGENT" -a "$DIR/agent.sock")
        pid=$(printf '%s\n' "$out" | sed -n 's/^SSH_AGENT_PID=\([0-9][0-9]*\).*/\1/p' | head -1)
        [ -n "$pid" ] && echo "$pid" > "$DIR/agent.pid"
        SSH_AUTH_SOCK="$DIR/agent.sock" "$SSHADD" "$DIR/client" >/dev/null 2>&1
        echo "ssh-agent started (pid $pid, sock $DIR/agent.sock, loaded: client)"
    fi

    echo
    echo "  user       : $(id -un)"
    echo "  client key : $DIR/client      (无口令，已在 agent)"
    echo "  enc key    : $DIR/client_enc  (口令: pw123)"
    echo "  hostkey fp : $("$KEYGEN" -lf "$DIR/hostkey.pub" -E sha256 | awk '{print $2}')"
    echo
    echo "  eval \"\$(scripts/dev-sshd.sh env)\"  # 让 harbor 走 agent 认证"
}

stop() {
    if [ -f "$DIR/sshd.pid" ]; then
        kill "$(cat "$DIR/sshd.pid")" 2>/dev/null && echo "sshd stopped"
        rm -f "$DIR/sshd.pid"
    fi
    if [ -f "$DIR/agent.pid" ]; then
        kill "$(cat "$DIR/agent.pid")" 2>/dev/null && echo "ssh-agent stopped"
        rm -f "$DIR/agent.pid" "$DIR/agent.sock"
    fi
}

env_cmd() { echo "export SSH_AUTH_SOCK=$DIR/agent.sock"; }

case "${1:-}" in
    start) start ;;
    stop)  stop ;;
    env)   env_cmd ;;
    *) echo "usage: $0 start|stop|env" >&2; exit 2 ;;
esac
