import json
from pathlib import Path
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
    39: section(1, "click kc-menu", "wait-view VMenuItem", "ui-click id=vmenu-item-0", "wait-dialog kgen", sheet=True),
    40: section(2, "seed-forward", "fwd-sw 0 on"),
    41: section(2, "fwd-form", sheet=True),
    42: section(3, "seed-snippet", "dialog-fill snip-search Harbor"),
    43: section(3, "snip-editor", sheet=True),
    44: section(4, "click known-import", "wait-open-panel"),
    46: section(5, "seed-log", "click logs-csv", "wait-save-panel"),
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

# These scenes use existing socket commands and real local terminal events.
MAC.update({
    18: ("watch-local", "task-ok", "wait-badge"),
    20: ("watch-local", "task-fail", "wait-badge"),
    23: (*LOCAL, "click ws-watch", "wait-dialog watch", "sheet"),
    24: (*LOCAL, "click ws-watch", "wait-dialog watch", "click watch-tester", "wait-dialog watch-tester", "sheet"),
    69: ("watch-local", "terminal-notification", "click bell", "wait-notification"),
    83: section(1, "click kc-menu", "wait-view VMenuItem"),
    84: section(2, "click fwd-menu", "wait-view VMenuItem"),
    85: section(3, "click snip-menu", "wait-view VMenuItem"),
    87: ("seed-hosts", "host-sel {host}", "click drawer-del", "wait-dialog undo-del", "sheet"),
    88: (*section(1), "seed-key", "delete-key", "wait-dialog kdel", "sheet"),
    89: (*section(2), "seed-forward", "delete-forward", "wait-dialog fwd-del", "sheet"),
    90: (*section(3), "seed-snippet", "delete-snippet", "wait-dialog undo-del", "sheet"),
    91: (*section(4), "seed-known", "click known-del", "wait-dialog known-del", "sheet"),
    95: (*SFTP, "local-pane 0 left", "select-note", "sftp-op 0 delete", "wait-view SftpToast"),
    113: (*section(1), "seed-key", "copy-key", "wait-view SftpToast"),
    116: (*LOCAL, "task-with-watch-off", "verify-no-notification"),
    133: ("watch-local", "terminal-notification", "click bell", "click notif-readall", "terminal-notification", "wait-notification"),
    135: (*SFTP, "local-pane 0 left", "select-note", "sftp-op 0 delete", "wait-view SftpToast"),
})

MAC.update({
    13: ("ssh-fixture", "ssh-connect", "wait-dialog tofu", "sheet"),
    14: ("ssh-fixture-delay", "trust-fixture", "ssh-connect", "wait-connection"),
    15: ("ssh-fixture", "trust-fixture", "ssh-connect", "wait-ssh", "ssh-motd"),
    48: ("ssh-fixture-delay", "trust-fixture", "tab-sel 1", "ssh-sftp", "wait-sftp-connection"),
    55: (*SFTP, "ssh-fixture-throttle", "trust-fixture", "tab-sel 1", "ssh-sftp", "wait-sftp", "transfer-fixture", "wait-running-transfer"),
    59: ("vnc-fixture", "vnc-connect", "wait-vnc"),
    114: ("ssh-fixture-delay", "trust-fixture", "tab-sel 1", "ssh-sftp", "wait-sftp-connection", "click sftp-hidelog-0"),
    115: (*LOCAL, r"type exit\n", "wait-ended", "click ws-reconn", "wait-local"),
})

MAC.update({
    8: ("seed-hosts", "click hosts-sort", "wait-native-menu", "menu-pick 按协议"),
    45: section(5, "seed-log", "click logs-sort", "click logs-saved"),
    82: ("click hosts-import", "wait-native-menu"),
    109: section(1, "click kc-menu", "wait-view VMenuItem", "ui-click id=vmenu-item-1", "wait-open-panel"),
    110: section(3, "click snip-menu", "wait-view VMenuItem", "ui-click id=vmenu-item-1", "wait-open-panel"),
    111: section(4, "click known-import", "wait-open-panel"),
    122: ("click hosts-import", "wait-native-menu", "menu-pick 导出 JSON…", "wait-save-panel"),
    123: section(1, "seed-key", "click kc-menu", "wait-view VMenuItem", "ui-click id=vmenu-item-2", "wait-save-panel"),
    124: section(2, "seed-forward", "click fwd-menu", "wait-view VMenuItem", "ui-click id=vmenu-item-3", "wait-save-panel"),
    125: ("ssh-fixture", "trust-fixture", "select-sidebar 2", "seed-forward-real", "click fwd-menu", "wait-view VMenuItem", "ui-click id=vmenu-item-1", "wait-forward-listening"),
    126: section(1, "ui-click text=Certificate", "wait-view SftpToast"),
    134: ("watch-local", "force-focus off", "setbool watch_focus_dnd 0", "terminal-notification", "wait-system-banner"),
})

MAC.update({
    57: ("rdp-fixture-delayed", "trust-fixture", "rdp-connect", "wait-rdp-connecting"),
    58: ("rdp-fixture", "trust-fixture", "rdp-connect", "wait-rdp"),
    60: ("rdp-fixture", "trust-fixture", "rdp-connect", "wait-rdp", "click rd-more", "wait-native-menu", "menu-pick 发送 Win", "wait-rdp-menu"),
    61: ("rdp-fixture", "trust-fixture", "rdp-connect", "wait-rdp", "click rd-quality", "wait-native-menu", "menu-pick 高清", "wait-rdp-quality"),
    117: ("rdp-fixture", "trust-fixture", "rdp-connect", "wait-rdp", "click rd-quality", "wait-native-menu", "menu-pick 高清", "wait-rdp-quality"),
    118: ("rdp-fixture", "trust-fixture", "rdp-connect", "wait-rdp", "rd-type whoami", "postkey return", "wait-rdp-input"),
    127: ("rdp-fixture", "trust-fixture", "rdp-connect", "wait-rdp"),
})


MAC_BLOCKED = dict.fromkeys(
    (34, 35, 37, 103, 104, 105, 106, 107, 108, 128, 129),
    "本轮按用户决定暂缺本地同步/共享测试服务；需服务后再执行账号与共享场景",
)
MAC_BLOCKED[81] = (
    "现有 macOS TabStrip 的 + 直接调用 newLocalTab，未提供场景要求的新建 Tab 菜单；待用户确认处理方式"
)


def fixture_provenance(n):
    steps = MAC[n]
    fixtures = []
    if any(step.startswith(('ssh-fixture', 'rdp-fixture')) for step in steps):
        fixtures.append({"transport": "SSH/SFTP", "server": "macOS OpenSSH",
                         "scope": "loopback, disposable keys and data"})
    if 'vnc-fixture' in steps:
        fixtures.append({"transport": "RFB 3.8", "server": "local capture fixture",
                         "remote_content": "synthetic framebuffer"})
    if any(step.startswith('rdp-fixture') for step in steps):
        fixtures.append({"transport": "RDP over SSH", "server": "FreeRDP TLS capture fixture",
                         "remote_content": "synthetic framebuffer and input feedback",
                         "limitation": "NLA and remote operating system behavior are not exercised"})
    return fixtures


def wait_state(dev, command, predicate):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        state = dev(command)
        if predicate(state):
            return
        time.sleep(0.08)
    raise RuntimeError(f"{command} did not reach the required state; last={state[:1000]}")


def mac_recipe(n, dev, data):
    variables = {}
    def object_id(raw):
        match = re.search(r"\bid=(\S+)", raw)
        if not match:
            raise RuntimeError('Object command returned no ID: ' + raw)
        return match[1]
    def emit_terminal(text):
        # printf interprets octal escapes in the shell; dev-mac.sh owns wire b64.
        dev("type " + text + r"\n")
    def terminal_notification():
        emit_terminal(r"printf '\033]777;notify;Harbor capture;Task completed\007'")
        wait_state(dev, 'notifs', lambda raw: bool(json.loads(raw)))

    for command in MAC[n]:
        if command == "sheet":
            # Older native sheets are not registered in the HModal registry.
            wait_state(dev, 'ui-windows', lambda raw: any(
                window.get('visible') and window.get('sheet') for window in json.loads(raw)))
            return "sheet"
        if command == "watch-local":
            dev('setbool watch_default_local 1')
            dev('setbool watch_focus_dnd 1')
            dev('force-focus on')
            dev('newtab')
            wait_state(dev, 'state', lambda raw: json.loads(raw)['tabs'][-1].get('running'))
        elif command == 'terminal-notification':
            terminal_notification()
        elif command == 'wait-notification':
            wait_state(dev, 'notifs', lambda raw: bool(json.loads(raw)))
        elif command in ('task-ok', 'task-fail'):
            status = '0' if command == 'task-ok' else '1'
            emit_terminal(r"printf '\033]133;A\007\033]133;B\007\033]133;C\007'; sleep 5.3; printf '\033]133;D;" + status + r"\007'")
        elif command == 'wait-badge':
            wait_state(dev, 'watch-badge', lambda raw: 'none' not in raw)
        elif command == 'task-with-watch-off':
            emit_terminal(r"printf '\033]777;notify;Harbor capture;Task completed\007'")
            time.sleep(.3)
        elif command == 'verify-no-notification':
            if json.loads(dev('notifs')):
                raise RuntimeError('Watch-off scene unexpectedly produced a notification')
        elif command.startswith('wait-dialog '):
            kind = command.split(' ',1)[1]
            wait_state(dev, 'dialogs', lambda raw: any(d['kind']==kind for d in json.loads(raw)))
        elif command.startswith('wait-view '):
            cls = command.split(' ',1)[1]
            wait_state(dev, 'ui-info class='+cls, lambda raw: bool(json.loads(raw)))
        elif command == 'delete-key':
            dev('kc-sel ' + variables['key'])
            dev('ui-click id=kc-del-' + variables['key'])
        elif command == 'copy-key':
            dev('kc-sel ' + variables['key'])
            dev('ui-click id=kc-copy-' + variables['key'])
        elif command == 'delete-forward':
            # Locate the actual generated rule ID using the existing state hook.
            dev('ui-click id=fwd-del-' + variables['forward'])
        elif command == 'delete-snippet':
            dev('select-sidebar 3')
            dev('ui-click id=snip-del-' + variables['snippet'])
        elif command == 'seed-known':
            variables['known'] = object_id(dev('mkobj known|host=capture.invalid|keytype=ssh-ed25519|fingerprint=SHA256:ui-capture'))
            dev('select-sidebar 4')
        elif command == 'select-note':
            # Restrict the list to the single known fixture before selecting it.
            dev('sftp-filter 0 notes.md')
            dev('sftp-sel 0 0')
        elif command == 'wait-system-banner':
            status=dev('notif-auth')
            if 'authorized' not in status and 'provisional' not in status:
                raise RuntimeError('notification-permission-required: '+status)
            time.sleep(1)
            variables['surface']='notification'
        elif command == 'wait-native-menu':
            wait_state(dev,'ui-menu-info',lambda raw:bool(json.loads(raw)))
            variables['surface']='menu'
        elif command.startswith('menu-pick '):
            dev('ui-menu '+command.split(' ',1)[1])
            variables.pop('surface',None)
        elif command in ('wait-open-panel','wait-save-panel'):
            cls='NSOpenPanel' if command=='wait-open-panel' else 'NSSavePanel'
            wait_state(dev, 'ui-windows', lambda raw: any(
                window.get('visible') and window.get('class') == cls for window in json.loads(raw)))
            time.sleep(.8)
            dev('ui-panel-directory ' + str(data))
            time.sleep(.6)
            wait_state(dev, 'ui-panel-path', lambda raw: Path(raw).resolve() == data.resolve())
            time.sleep(.4)
            variables['surface']='screen'
        elif command == 'seed-forward-real':
            variables['forward']=dev('fwd-add dynamic 127.0.0.1 0 - 0 '+variables['ssh_host']).strip()
        elif command == 'wait-forward-listening':
            wait_state(dev,'fwd-mgr',lambda raw:'listen' in raw.lower() or 'running' in raw.lower())
        elif command.startswith('rdp-fixture'):
            from fixtures import RDP,SSH,Gate
            remote=RDP(data);dev.resources.callback(remote.close);remote.start()
            server=SSH(data);dev.resources.callback(server.close);server.start()
            port=server.port
            if command.endswith('delayed'):
                gate=Gate(port,delay=3);dev.resources.callback(gate.close);port=gate.port
            variables['ssh_server']=server;variables['ssh_port']=port
            variables['ssh_host']=object_id(dev(f'mkobj host|name=RDP SSH fixture|proto=ssh|address=127.0.0.1|port={port}#i|user={server.user}|auth=key|identity_file={server.key}'))
            variables['rdp_server']=remote
            variables['rdp_host']=object_id(dev(f"mkobj host|name=Local RDP fixture|proto=rdp|address=127.0.0.1|port={remote.port}#i|user=capture|os=rdp|tunnel=ssh|via={variables['ssh_host']}"))
            dev('setsecret '+variables['rdp_host']+'|password|capture')
            dev('select-sidebar 0')
        elif command == 'rdp-connect':
            dev('host-sel '+variables['rdp_host']);dev('ui-click id=drawer-connect')
        elif command == 'wait-rdp-connecting':
            wait_state(dev,'rd',lambda raw:json.loads(raw).get('useTunnel') and not json.loads(raw).get('connected'))
        elif command == 'wait-rdp':
            deadline=time.monotonic()+18
            while time.monotonic()<deadline:
                state=json.loads(dev('rd'))
                if state.get('connected') and state.get('fbW',0)>0:break
                dialogs=json.loads(dev('dialogs'))
                if any(d['kind']=='rd-cert' for d in dialogs):
                    # Only this locally generated fixture certificate is accepted.
                    dev('dialog-press 信任此证书')
                if state.get('errorShown'):
                    variables['rdp_server'].log.flush()
                    variables['rdp_server'].log.seek(0)
                    serverlog=variables['rdp_server'].log.read()[-3000:]
                    raise RuntimeError('RDP fixture connection failed: '+str(state)+'; server='+serverlog)
                time.sleep(.1)
            else:raise RuntimeError('RDP fixture did not reach a rendered desktop')
        elif command == 'wait-rdp-quality':
            wait_state(dev,'rd',lambda raw:json.loads(raw).get('quality')==2 and json.loads(raw).get('connected'))
        elif command == 'wait-rdp-menu':
            deadline=time.monotonic()+4
            while not variables['rdp_server'].menu and time.monotonic()<deadline:time.sleep(.1)
            if not variables['rdp_server'].menu:raise RuntimeError('RDP server did not receive the Windows key')
            time.sleep(.3)
        elif command == 'wait-rdp-input':
            deadline=time.monotonic()+4
            while variables['rdp_server'].last!='whoami' and time.monotonic()<deadline:time.sleep(.1)
            if variables['rdp_server'].last!='whoami':raise RuntimeError('RDP server did not receive whoami + Enter')
            time.sleep(.3)
        elif command.startswith('ssh-fixture'):
            from fixtures import SSH, Gate
            server=SSH(data)
            dev.resources.callback(server.close)
            server.start()
            port=server.port
            if command != 'ssh-fixture':
                gate=Gate(port,delay=3 if command.endswith('delay') else 0,
                          interval=.03 if command.endswith('throttle') else 0)
                dev.resources.callback(gate.close)
                port=gate.port
            variables['ssh_server']=server
            variables['ssh_port']=port
            variables['ssh_host']=object_id(dev(f'mkobj host|name=Local SSH fixture|proto=ssh|address=127.0.0.1|port={port}#i|user={server.user}|auth=key|identity_file={server.key}|os=ubuntu'))
            dev('select-sidebar 0')
        elif command == 'trust-fixture':
            path=data/'fixture_known_hosts'
            path.write_text(f"[127.0.0.1]:{variables['ssh_port']} {variables['ssh_server'].public}\n")
            dev('select-sidebar 4');dev('known-import '+str(path))
            # Import reports success in a sheet. It belongs to fixture setup,
            # and must not obscure the target SSH/RDP state or its cert prompt.
            dialogs=json.loads(dev('dialogs'))
            if any(d['kind']=='known-import' for d in dialogs):
                dev('dialog-press 关闭')
                wait_state(dev,'dialogs',lambda raw:not any(d['kind']=='known-import' for d in json.loads(raw)))
            dev('select-sidebar 0')
        elif command == 'ssh-connect':
            dev('connect '+variables['ssh_host'])
        elif command == 'wait-connection':
            wait_state(dev,'ui-info id=stage-log',lambda raw:bool(json.loads(raw)))
        elif command == 'wait-ssh':
            wait_state(dev,'gridinfo',lambda raw:bool(re.search(r'nonblank=[1-9]',raw)))
        elif command == 'ssh-motd':
            emit_terminal("printf 'Harbor isolated SSH fixture\\nAuthenticated over loopback OpenSSH\\n'")
        elif command == 'ssh-sftp':
            dev('sftp-pick 0 '+variables['ssh_host'])
        elif command == 'wait-sftp-connection':
            wait_state(dev,'sftp',lambda raw:'state=conn' in raw)
        elif command == 'wait-sftp':
            wait_state(dev,'sftp',lambda raw:'pane0: state=browse' in raw)
        elif command == 'transfer-fixture':
            file=data/'left'/'payload.bin'
            with file.open('wb') as f:f.truncate(16*1024*1024)
            dev('sftp-cd 0 '+str(data/'right'))
            dev('sftp-pick 1 local');dev('sftp-cd 1 '+str(data/'left'))
            dev('sftp-filter 1 payload.bin');dev('sftp-sel 1 0');dev('sftp-op 1 copy')
        elif command == 'wait-running-transfer':
            wait_state(dev,'sftp',lambda raw:':run:' in raw)
        elif command == 'vnc-fixture':
            from fixtures import VNC
            server=VNC();dev.resources.callback(server.close)
            variables['vnc_host']=object_id(dev(f'mkobj host|name=Local VNC fixture|proto=vnc|address=127.0.0.1|port={server.port}#i|os=vnc'))
            dev('select-sidebar 0')
        elif command == 'vnc-connect':
            dev('host-sel '+variables['vnc_host']);dev('click drawer-connect')
        elif command == 'wait-vnc':
            wait_state(dev,'rd',lambda raw:json.loads(raw).get('connected') and json.loads(raw).get('fbW',0)>0)
        elif command == "seed-hosts":
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
            variables['key'] = dev("kc-gen ed25519 harbor-demo").strip()
            dev('select-sidebar 1')
        elif command == "seed-forward":
            variables['forward'] = dev("fwd-add dynamic 127.0.0.1 0 - 0 -").strip()
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
            actual=command.format(**variables)
            if actual.startswith('click '):actual='ui-click id='+actual[6:]
            dev(actual)
    return variables.get('surface')


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
