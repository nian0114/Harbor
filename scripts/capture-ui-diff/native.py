import json
import re
import time


NO_MATCH = "no-match-ui-capture"


def section(index, *commands, sheet=False):
    return (f"select-sidebar {index}", *commands, *(('sheet',) if sheet else ()))


LOCAL = ("newtab", "wait-local")
WORKSPACE = (*LOCAL, "action split-right", "wait-workspace")
SFTP = ("tab-sel 1", "fixture-files")
MAC = {
    1: ("seed-hosts",),
    4: ("seed-hosts", "mode grid"),
    5: ("seed-hosts", "mode list"),
    6: ("seed-hosts", f"search {NO_MATCH}"),
    7: ("seed-hosts", "click chip-prod"),
    9: ("seed-hosts", "host-sel {host}"),
    11: ("click hosts-new", "sheet"),
    12: ("seed-hosts", "host-sel {host}", "click drawer-edit", "sheet"),
    16: LOCAL,
    17: (*LOCAL, r"type echo Analyzing repository...; sleep 10\n", "wait-output Analyzing repository..."),
    22: (*LOCAL, r"type exit\n", "wait-ended"),
    25: (*LOCAL, "fixture-find", "find-open", "find-type Harbor"),
    26: (*LOCAL, "fixture-find", "find-open", "find-type Harbor", "find-next"),
    27: WORKSPACE,
    28: (*WORKSPACE, "action focus-mode"),
    29: (*WORKSPACE, "action broadcast"),
    30: (*WORKSPACE, "seed-snippet", "assist on", "click assist-tab-snip"),
    31: (*WORKSPACE, "assist on", "click assist-tab-theme"),
    32: (*WORKSPACE, "assist on", "click assist-tab-auto"),
    33: (*LOCAL, "share-dialog", "sheet"),
    36: section(6, "click share-join-open", "sheet"),
    38: section(1, "seed-key"),
    39: section(1, "click kc-gen", sheet=True),
    40: section(2, "seed-forward", "fwd-sw 0 on"),
    41: section(2, "fwd-form", sheet=True),
    42: section(3, "seed-snippet", "dialog-fill snip-search Harbor"),
    43: section(3, "snip-editor", sheet=True),
    44: section(4, "click known-import"),
    46: section(5, "seed-log", "click logs-csv"),
    47: ("tab-sel 1",),
    49: (*SFTP, "local-pane 0 left", "local-pane 1 right"),
    50: (*SFTP, "local-pane 0 left", "sftp-hidden 0 off", "sftp-filter 0 notes"),
    51: (*SFTP, "local-pane 0 left", "sftp-sel 0 0", "sftp-op 0 rename", "sheet"),
    52: (*SFTP, "local-pane 0 left", "sftp-sel 0 0"),
    53: (*SFTP, "local-pane 0 left", f"sftp-filter 0 {NO_MATCH}"),
    56: (*SFTP, "local-pane 0 left", "local-pane 1 right", "sftp-sel 0 0", "sftp-op 0 copy", "wait-transfer"),
    62: section(7),
    63: ("set theme light", *section(7)),
    64: (*LOCAL, "set theme ubuntu"),
    65: (*LOCAL, "set theme solarized"),
    66: ("set font_size 20", "set line_height 2.0", *section(7)),
    67: ("pal-open",),
    68: ("pal-open", f"pal-type {NO_MATCH}"),
    70: ("click bell",),
    92: (*SFTP, "local-pane 0 left", "sftp-sel 0 0", "sftp-op 0 rename", "sheet"),
    93: (*SFTP, "local-pane 0 left", "sftp-op 0 mkdir", "sheet"),
    94: (*SFTP, "local-pane 0 left", "sftp-sel 0 0", "sftp-op 0 chmod", "sheet"),
    95: (*SFTP, "local-pane 0 left", "sftp-sel 0 0", "sftp-op 0 delete", "sheet"),
    96: (*SFTP, "local-pane 0 left", "sftp-op 0 selectall"),
    97: section(1, "seed-key", f"dialog-fill kc-search {NO_MATCH}"),
    98: section(2, "seed-forward", f"dialog-fill fwd-search {NO_MATCH}"),
    99: section(3, "seed-snippet", f"dialog-fill snip-search {NO_MATCH}"),
    100: section(4, f"dialog-fill known-search {NO_MATCH}"),
    101: section(5, "seed-log", f"dialog-fill logs-search {NO_MATCH}"),
    102: section(6),
    112: (*LOCAL, "seed-variable", "snip-send {snippet}", "sheet"),
    120: ("seed-hosts", "host-sel {host}", "click drawer-dup"),
    121: ("import-config",),
    130: ("set font_size 20", "set line_height 2.0", "set theme solarized", "set font_size 14", "set line_height 1.15", "set theme harbor", *section(7)),
}

MAC_BLOCKED = {}
for ids, reason in (
    ((8,), "需要可选中 Hosts 排序菜单选项的驱动步骤"),
    ((45,), "现有 click 钩子只支持 NSControl，不能触发 Logs 的自定义排序表头"),
    ((13, 14, 15, 48, 114, 115), "需要可控的本地 SSH/SFTP 服务及连接状态，当前配方未配置该夹具"),
    ((57, 58, 59, 60, 61, 117, 118, 127), "需要可控的本地 RDP/VNC 和 SSH 隧道夹具"),
    ((34, 35, 37, 103, 104, 105, 106, 107, 108, 128, 129), "需要隔离的已登录同步账号或共享 Console 参与端夹具"),
    ((18, 20, 69, 116, 133, 134), "需要驱动本地会话监控事件及通知状态，现有配方未验证该入口"),
    ((23, 24), "需要可驱动会话监控弹窗及规则测试器的入口"),
    ((55,), "需要可稳定停留在传输运行态的 SFTP 夹具"),
    ((81, 82, 83, 84, 85, 109, 110, 111, 122, 123, 124, 125, 126), "需要捕获原生弹出菜单或选取菜单项的驱动步骤"),
    ((87, 88, 89, 90, 91, 135), "需要驱动删除确认之后的撤销反馈并验证该状态"),
    ((113,), "需要捕获复制成功或失败的反馈状态"),
):
    MAC_BLOCKED.update(dict.fromkeys(ids, reason))


def wait_state(dev, command, predicate):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        state = dev(command)
        if predicate(state):
            return
        time.sleep(0.08)
    raise RuntimeError(f"{command} did not reach the required state")


def mac_recipe(n, dev, data):
    variables = {}
    for command in MAC[n]:
        if command == "sheet":
            return "sheet"
        if command == "seed-hosts":
            # HARBOR_DEMO currently seeds Linux only. Seed equivalent local vault objects via dev.sock.
            for name, group, proto in (("10.10.21.84", "prod", "ssh"), ("10.88.2.11", "prod", "ssh"),
                                       ("10.87.102.179", "lab", "ssh"), ("home-win", "lab", "rdp"),
                                       ("lab-vnc", "lab", "vnc")):
                response = dev(f"mkobj host|name={name}|address=127.0.0.1|proto={proto}|user=demo|group={group}|os=ubuntu")
                if name == "10.88.2.11":
                    variables["host"] = re.search(r"\bid=(\S+)", response)[1]
            # HostsPage does not observe mkobj writes. Re-enter it to reload
            # HostStore before selecting chips/cards or taking the first shot.
            dev("select-sidebar 0")
            dev(f"host-sel {variables['host']}")
            dev("select-sidebar 0")
        elif command in {"seed-snippet", "seed-variable"}:
            body = "tail -f {{path}}" if command == "seed-variable" else "printf 'Harbor demo\\n'"
            response = dev(f"mkobj snippet|title=Harbor Demo|body={body}|scope=global")
            variables["snippet"] = re.search(r"\bid=(\S+)", response)[1]
        elif command == "seed-key":
            dev("kc-gen ed25519 harbor-demo")
        elif command == "seed-forward":
            dev("fwd-add dynamic 127.0.0.1 0 - 0 -")
        elif command == "seed-log":
            dev("newtab")
            wait_state(dev, "state", lambda raw: json.loads(raw)["tabs"][-1].get("running"))
            dev(r"type exit\n")
            wait_state(dev, "state", lambda raw: not json.loads(raw)["tabs"][-1].get("running"))
            dev("select-sidebar 5")
        elif command == "fixture-files":
            for side in ("left", "right"):
                (data / side).mkdir()
            (data / "left/notes.md").write_text("Harbor capture fixture\n", encoding="utf-8")
            (data / "left/.hidden").touch()
        elif command.startswith("local-pane "):
            _, pane, side = command.split()
            dev(f"sftp-pick {pane} local")
            dev(f"sftp-cd {pane} {data / side}")
            wait_state(dev, "sftp", lambda raw: str(data / side) in raw)
        elif command == "wait-transfer":
            wait_state(dev, "sftp", lambda raw: "done" in raw)
        elif command == "wait-local":
            wait_state(dev, "state", lambda raw: json.loads(raw)["tabs"][-1].get("running", False))
        elif command == "wait-ended":
            wait_state(dev, "state", lambda raw: not json.loads(raw)["tabs"][-1].get("running"))
        elif command.startswith("wait-output "):
            wait_state(dev, "gridinfo", lambda raw: command[12:28] in raw)
        elif command == "wait-workspace":
            wait_state(dev, "ws-state", lambda raw: raw.startswith("panes=2 "))
        elif command == "fixture-find":
            dev(r"type echo Harbor Alpha; echo Harbor Bravo\n")
            wait_state(dev, "gridinfo", lambda raw: "Harbor Bravo" in raw)
        elif command == "import-config":
            config = data / "ssh-config"
            config.write_text("Host web-01.prod\n  HostName 127.0.0.1\n  User demo\n", encoding="utf-8")
            dev(f"import-sshcfg {config}")
            wait_state(dev, "hosts", lambda raw: "web-01.prod" in raw)
        else:
            dev(command.format(**variables))
    return None


LINUX = {
    2: (),
    4: ("--host-view=grid",),
    5: ("--host-view=list",),
    6: (f"--filter={NO_MATCH}",),
    16: ("--open-local",),
    17: ("--open-local=printf 'Analyzing repository...\\n'; sleep 10",),
    22: ("--open-local=exit",),
    38: ("--section=keys",),
    40: ("--section=fwd",),
    42: ("--section=snippets",),
    45: ("--section=logs",),
    62: ("--section=appearance",),
    63: ("--section=appearance", "--theme=light"),
    64: ("--open-local", "--theme=ubuntu"),
    65: ("--open-local", "--theme=solarized"),
}

LINUX_ACTIONS = {
    11: ("dev-ui='hostform'",),
    67: ("dev-ui='palette'",),
    69: ("dev-ui='notifpost|Task completed|128 tests passed'", "dev-ui='notif'"),
    70: ("dev-ui='notif'",),
}
