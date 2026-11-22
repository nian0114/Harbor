#!/bin/bash
# dev.sock 行协议客户端（在 Mac 上运行）：
#   scripts/dev-mac.sh snapshot /tmp/s.png
#   scripts/dev-mac.sh type 'echo hi\n' | key 'ctrl+c' | state
SOCK="${HARBOR_DATA_DIR:-$HOME/Library/Application Support/Harbor}/dev.sock"
python3 - "$SOCK" "$@" <<'PY'
import socket, sys
sock, args = sys.argv[1], sys.argv[2:]
import base64
args = list(args)
if args and args[0] in ("type", "ime-mark", "ime-commit"):
    payload = " ".join(args[1:]).replace("\\n", "\n").replace("\\t", "\t")
    args[1:] = [base64.b64encode(payload.encode()).decode()]
line = " ".join(args) + "\n"
import time
s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try:
        s.connect(sock); break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit("no dev.sock")
s.settimeout(10)
s.sendall(line.encode())
out = b""
try:
    while b"\n" not in out:
        d = s.recv(65536)
        if not d: break
        out += d
except socket.timeout:
    out += b"\nTIMEOUT\n"
sys.stdout.write(out.decode())
PY
