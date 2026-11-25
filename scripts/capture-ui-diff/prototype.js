async ({ id, mode, advance }) => {
  const tab = () => tabById(S.activeTab);
  const sess = () => tab().sess || tab().panes[tab().focused];
  const section = (name) => A.section(name);
  const local = () => A.openLocal();
  const command = (text) => runCommand(sess(), text);
  const ssh = async () => { A.openHost("h2"); await advance(3100); };
  const connect = async () => { A.openHost("h2"); await advance(700); };
  const desktop = async (host = "h5") => { A.openHost(host); await advance(1450); };
  const ws = () => A.openWorkspace();
  const share = () => { local(); A.openShareModal(tab().id); };
  const shareOn = () => { share(); A.toggleShare(sess().id, tab().id, true); };
  const sftp = () => A.switchTab("sftp");
  const files = async (both = false) => {
    sftp(); A.sftpPick("left", "h2"); await advance(2020);
    if (both) { A.sftpPick("right", "local"); await advance(970); }
  };
  const selectFile = () => A.sftpSelect(`left${sftpFiles("left").findIndex(([name]) => name === "notes.md")}`);
  const menu = (method) => {
    const button = [...document.querySelectorAll("[onclick]")].find((el) =>
      el.getClientRects().length && el.getAttribute("onclick").includes(`A.${method}(`));
    if (!button) throw Error(`Visible ${method} control is missing`);
    const box = button.getBoundingClientRect();
    button.dispatchEvent(new MouseEvent("click", { bubbles: true, clientX: box.left, clientY: box.bottom }));
  };
  const item = (label) => {
    const el = [...document.querySelectorAll("#ctxMenu button")].find((e) => e.textContent.trim() === label);
    if (!el) throw Error(`Menu item is missing: ${label}`);
    el.click();
  };
  const fill = (selector, value) => {
    const input = $(selector);
    input.value = value;
    input.dispatchEvent(new Event("input", { bubbles: true }));
  };
  const notice = (title = "Task completed") => notify({ kind: "ok", title, body: "128 tests passed", tabId: null });
  const notifications = () => { $("#notifPanel").hidden = false; renderNotifPanel(); };
  const undo = (action) => { action(); A.confirmOk(); };
  const find = () => { A.openFind(tab().id); fill('[data-filt="find"]', "Ubuntu"); };
  const filter = (key) => {
    section(({ keys: "keychain", fwd: "forwarding", snip: "snippets" })[key] || key);
    A.filt(key, "no-match-ui-capture");
  };
  const more = () => menu("termMore");
  const fwd = () => section("forwarding");
  const settings = () => section("settings");
  const extremes = () => { settings(); A.setPref("fs", 20); A.setPref("lh", 2); };
  const theme = (name) => A.setPref("theme", name);
  const fileMenu = async (selected = true) => { await files(); if (selected) selectFile(); menu("sftpActions"); };
  const fileAction = async (label) => { await fileMenu(); item(label); };
  const hostMenu = () => menu("newHostMenu");
  const keyMenu = () => { section("keychain"); menu("keyMenu"); };
  const fwdMenu = () => { fwd(); menu("fwdMenu"); };
  const snipMenu = () => { section("snippets"); menu("snipMenu"); };

  const steps = {
    1: () => section("hosts"),
    2: () => section("hosts"),
    3: () => section("hosts"),
    4: () => A.hostViewSet("grid"),
    5: () => A.hostViewSet("list"),
    6: () => fill("#quickInput", "no-match-ui-capture"),
    7: () => ["lab", "local", "tun", "prod"].forEach(A.hostTagSet),
    8: () => ["name", "recent", "type"].forEach(A.hostSortSet),
    9: () => hostDrawer("h2"),
    10: () => hostDrawer("h2"),
    11: () => A.newHostModal(),
    12: () => { hostDrawer("h2"); A.editHostModal("h2"); },
    13: () => {
      A.newHostModal();
      fill("#nhAddr", "capture.invalid");
      A.saveHost(); A.openHost(S.hosts.at(-1).id);
    },
    14: connect,
    15: ssh,
    16: () => { section("sessions"); local(); },
    17: async () => { local(); command("ai-task"); await advance(2300); },
    18: async () => { local(); command("ai-task"); await advance(6100); },
    19: async () => { local(); command("ai-task"); section("hosts"); await advance(6300); },
    20: async () => { local(); command("fail-task"); await advance(4900); },
    21: async () => { local(); command("ai-task"); await advance(450); A.aKey("ctrl"); A.aKey("ctrlc"); },
    22: () => { local(); command("exit"); },
    23: () => { local(); A.watchModal(sess().id, tab().id); },
    24: () => { local(); A.watchModal(sess().id, tab().id); A.testRule(); },
    25: async () => { await ssh(); find(); },
    26: async () => { await ssh(); find(); A.findNext(1); },
    27: ws,
    28: () => { ws(); A.toggleFocus(tab().id); },
    29: () => { ws(); A.toggleBroadcast(tab().id); },
    30: () => { ws(); A.assistTab(tab().id, "snip"); },
    31: () => { ws(); A.assistTab(tab().id, "theme"); },
    32: () => { ws(); A.assistTab(tab().id, "auto"); },
    33: share,
    34: shareOn,
    35: async () => { shareOn(); await advance(3650); },
    36: () => { section("shared"); A.joinByCode(); fill('#modalRoot input[placeholder="482 913"]', "482 913"); },
    37: () => { section("shared"); A.joinPeer(); },
    38: () => section("keychain"),
    39: () => { section("keychain"); A.newKeyModal(); },
    40: () => { fwd(); A.toggleForward("f2", true); },
    41: () => { fwd(); A.newForwardModal(); },
    42: () => { section("snippets"); A.filt("snip", "Docker"); },
    43: () => { section("snippets"); A.editSnippet("s1"); },
    44: () => { section("known"); A.importKnown(); },
    45: () => { section("logs"); A.logSort(); A.logSaved(0); },
    46: () => { section("logs"); A.exportLogs(); },
    47: sftp,
    48: async () => { sftp(); A.sftpPick("left", "h2"); await advance(550); },
    49: async () => { await files(true); A.sftpDbl("left", "projects"); },
    50: async () => { await fileMenu(false); item("Hide Hidden Files"); A.sftpFilterToggle("left"); A.sftpFilter("left", "notes"); },
    51: fileMenu,
    52: async () => { await files(); selectFile(); },
    53: async () => { await files(); A.sftpFilterToggle("left"); A.sftpFilter("left", "no-match-ui-capture"); },
    54: async () => { await files(); pickTransferTarget("left", "notes.md", "right"); },
    55: async () => { await files(true); selectFile(); startTransfer("left", "app.tar.gz"); await advance(480); },
    56: async () => { await files(true); startTransfer("left", "notes.md"); await advance(2200); },
    57: async () => { A.openHost("h5"); await advance(500); },
    58: async () => { await desktop(); tab().rbuf = "whoami"; A.remoteKeyBtn(tab().id, "enter"); },
    59: () => desktop("h6"),
    60: async () => { await desktop(); A.remoteKeyBtn(tab().id, "win"); },
    61: async () => { await desktop(); A.rdpQuality(tab().id, "流畅"); },
    62: settings,
    63: () => { settings(); theme("light"); },
    64: () => { settings(); theme("ubuntu"); local(); },
    65: () => { settings(); theme("solarized"); local(); },
    66: extremes,
    67: palette,
    68: () => { palette(); fill('[data-filt="pal"]', "no-match-ui-capture"); },
    69: () => { notice(); notifications(); },
    70: () => { A.clearNotifs(); notifications(); },
    71: () => ["hosts", "sessions", "keychain", "account"].forEach(section),
    72: () => { local(); section("sessions"); },
    73: () => section("keychain"),
    74: () => section("account"),
    75: () => { section("account"); fwd(); },
    76: ssh,
    77: () => { ws(); A.toggleBroadcast(tab().id); },
    78: () => { A.doLogout(false); },
    79: () => { section("account"); recoveryModal(); },
    80: () => section("hosts"),
    81: () => $("#tabAdd").dispatchEvent(new MouseEvent("click", { clientX: 265, clientY: 112 })),
    82: hostMenu,
    83: keyMenu,
    84: fwdMenu,
    85: snipMenu,
    86: () => { local(); command("exit"); more(); },
    87: () => { hostDrawer("h2"); undo(() => A.deleteHost("h2")); },
    88: () => { section("keychain"); undo(() => A.delKey("k1")); },
    89: () => { fwd(); undo(() => A.delForward("f1")); },
    90: () => { section("snippets"); undo(() => A.delSnippet("s1")); },
    91: () => { section("known"); undo(() => A.delKnown("n1")); },
    92: () => fileAction("Rename…"),
    93: () => fileAction("New Folder…"),
    94: () => fileAction("Edit Permissions…"),
    95: () => fileAction("Delete…"),
    96: () => fileAction("Select All"),
    97: () => filter("keys"),
    98: () => filter("fwd"),
    99: () => filter("snip"),
    100: () => filter("known"),
    101: () => filter("logs"),
    102: () => { A.doLogout(true); section("shared"); },
    103: () => section("account"),
    104: () => { section("account"); A.logoutModal(); },
    105: () => { section("account"); A.doLogout(true); section("hosts"); },
    106: () => { A.doLogout(true); A.login(); section("hosts"); },
    107: () => { section("account"); A.revokeDevice("d2"); },
    108: () => { section("account"); $(".switch input").click(); },
    109: () => { keyMenu(); item("导入私钥…"); fill("#pmVal", "示例 OpenSSH 私钥文本"); },
    110: () => { snipMenu(); item("导入 JSON…"); },
    111: () => { section("known"); A.importKnown(); },
    113: () => { section("keychain"); $("[onclick*='A.copyPub']").click(); },
    114: async () => { await connect(); A.connHideLog(tab().id); },
    115: async () => { local(); command("exit"); A.reconnect(tab().id); await advance(550); },
    116: async () => { local(); A.setWatch(sess().id, false); A.closeModal(); command("ai-task"); await advance(6100); },
    117: async () => { await desktop(); ["流畅", "均衡", "高清"].forEach((q) => A.rdpQuality(tab().id, q)); },
    118: async () => { await desktop(); tab().rbuf = "ls"; A.remoteKeyBtn(tab().id, "enter"); A.remoteKeyBtn(tab().id, "win"); },
    120: () => { hostDrawer("h2"); A.duplicateHost("h2"); },
    121: () => { hostMenu(); item("从 ~/.ssh/config 导入"); },
    122: () => { hostMenu(); item("导出主机 JSON"); },
    123: () => { keyMenu(); item("导出全部公钥"); },
    124: () => { fwdMenu(); item("导出规则 JSON"); },
    125: () => { fwdMenu(); item("全部启动"); },
    126: () => { section("keychain"); $("[onclick*='A.toastInfo']").click(); },
    127: async () => { await desktop(); section("logs"); },
    128: async () => { shareOn(); await advance(3650); },
    129: () => { shareOn(); $("#modalRoot [onclick*='A.copyLink']").click(); },
    130: () => { extremes(); A.resetPrefs(); },
    131: () => { section("account"); settings(); },
    132: () => fileMenu(false),
    133: () => { notice(); A.readAll(); notice("Build completed"); notifications(); },
    134: async () => { local(); command("ai-task"); section("hosts"); await advance(12050); },
    135: async () => { await fileAction("Delete…"); A.confirmOk(); },
  };

  $(`#modeSwitch [data-mode="${mode}"]`).click();
  await steps[id]();
  await advance(id === 134 ? 30 : 320);
  const expected = {
    5: [".table"], 6: [".empty"], 9: ["#modalRoot .drawer"], 10: ["#modalRoot .drawer"],
    11: ["#nhAddr"], 12: ["#nhAddr"], 13: ["#modalRoot .modal"],
    19: [".toast"], 25: ["#findCount"], 26: ["#findCount"],
    27: [".workspace .pane"], 29: [".pane.broadcast"], 36: ["#modalRoot input"],
    55: [".xfer.run"], 56: [".xfer.done"], 60: [".start-menu"], 67: [".pal-box"],
    68: [".pal-empty"], 69: ["#notifPanel"], 70: ["#notifPanel"],
    81: ["#ctxMenu"], 82: ["#ctxMenu"], 83: ["#ctxMenu"], 84: ["#ctxMenu"],
    85: ["#ctxMenu"], 86: ["#ctxMenu"], 92: ["#pmVal"], 93: ["#pmVal"], 94: ["#pmVal"],
    109: ["#pmVal"], 110: ["#pmVal"], 111: ["#pmVal"], 132: ["#ctxMenu"],
    134: [".toast.out"], 135: [".toast"],
  }[id] || [];
  for (const selector of expected) {
    if (!$(selector)?.getClientRects().length) throw Error(`Scene ${id}: ${selector} is not visible`);
  }
}
