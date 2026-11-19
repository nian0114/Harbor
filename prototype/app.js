/* ============ Harbor prototype app (macOS · Linux · Android) ============ */
"use strict";

/* ---------- unified SVG icon system (PRD §6.1) ---------- */
const P = {
  server: '<rect x="3" y="4" width="18" height="7" rx="2"/><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M7 7.5h.01M7 16.5h.01"/>',
  key: '<circle cx="8" cy="15.5" r="4"/><path d="M10.9 12.6 20 3.5M15 8.5l3 3M18 5.5l2 2"/>',
  fwd: '<path d="M4 7.5h13.5l-3.2-3.2M20 16.5H6.5l3.2 3.2"/>',
  fingerprint: '<path d="M6.5 11a5.5 5.5 0 0 1 11 0v3.5M9.2 11a2.8 2.8 0 0 1 5.6 0v4a7 7 0 0 1-1 3.6M12 11v5.2a10 10 0 0 1-1.4 4.6M6.5 14.5a13 13 0 0 1-.7 3.7M17.5 17a12 12 0 0 1-.6 2.6M4 8.5A8.5 8.5 0 0 1 12 4a8.4 8.4 0 0 1 6.6 3.2"/>',
  clock: '<circle cx="12" cy="12" r="8.6"/><path d="M12 7.2v4.8l3.1 2"/>',
  broadcast: '<circle cx="12" cy="12" r="2"/><path d="M8.6 8.6a4.8 4.8 0 0 0 0 6.8M15.4 8.6a4.8 4.8 0 0 1 0 6.8M5.8 5.8a8.8 8.8 0 0 0 0 12.4M18.2 5.8a8.8 8.8 0 0 1 0 12.4"/>',
  cloud: '<path d="M7 18a4.5 4.5 0 1 1 .7-8.95A6 6 0 0 1 19.2 10.6 3.8 3.8 0 0 1 18.3 18H7z"/>',
  bell: '<path d="M18 9.5a6 6 0 1 0-12 0c0 5.5-2.3 6.8-2.3 6.8h16.6S18 15 18 9.5M10.4 19.8a1.9 1.9 0 0 0 3.2 0"/>',
  panel: '<rect x="3" y="4" width="18" height="16" rx="2.5"/><path d="M9.5 4v16"/>',
  search: '<circle cx="11" cy="11" r="6.8"/><path d="m20.3 20.3-4.4-4.4"/>',
  grid: '<rect x="4" y="4" width="7" height="7" rx="1.8"/><rect x="13" y="4" width="7" height="7" rx="1.8"/><rect x="4" y="13" width="7" height="7" rx="1.8"/><rect x="13" y="13" width="7" height="7" rx="1.8"/>',
  tag: '<path d="M3.5 10.8V4.5a1 1 0 0 1 1-1h6.3L20.5 13l-7.5 7.5L3.5 10.8z"/><circle cx="8" cy="8" r="1.4"/>',
  list: '<path d="M8.5 6h12M8.5 12h12M8.5 18h12M3.8 6h.01M3.8 12h.01M3.8 18h.01"/>',
  plug: '<path d="M9 3v5M15 3v5M6 8h12v3a6 6 0 0 1-12 0V8zM12 17v4"/>',
  cert: '<rect x="4" y="3" width="16" height="13.5" rx="2"/><path d="M8 7h8M8 10.5h4.5"/><circle cx="15.8" cy="16.8" r="2.8"/><path d="M14.4 19.2 13.8 22l2-1.1 2 1.1-.6-2.8"/>',
  fido: '<rect x="5" y="10" width="14" height="10" rx="2.5"/><path d="M8.2 10V7.2a3.8 3.8 0 0 1 7.6 0V10M12 14v2.6"/>',
  lock: '<rect x="5" y="10" width="14" height="10" rx="2.5"/><path d="M8.2 10V7.2a3.8 3.8 0 0 1 7.6 0V10M12 14v2.6"/>',
  folder: '<path d="M3 7a2 2 0 0 1 2-2h4.2l2 2.5H19a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7z"/>',
  file: '<path d="M6 3h8l4 4v14H6V3zM14 3v4h4M9.2 12.5h5.6M9.2 16h5.6"/>',
  ubuntu: '<circle cx="12" cy="12" r="8.2" stroke-dasharray="11.5 5.7"/><circle cx="12" cy="3.8" r="1.9" fill="currentColor" stroke="none"/><circle cx="4.9" cy="16.1" r="1.9" fill="currentColor" stroke="none"/><circle cx="19.1" cy="16.1" r="1.9" fill="currentColor" stroke="none"/>',
  asterisk: '<path d="M12 4.5v15M5.5 8.25l13 7.5M18.5 8.25l-13 7.5"/>',
  win: '<path d="M4 4h7.4v7.4H4zM12.6 4H20v7.4h-7.4zM4 12.6h7.4V20H4zM12.6 12.6H20V20h-7.4z" fill="currentColor" stroke="none"/>',
  monitor: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M9 20h6M12 16v4"/>',
  terminal: '<rect x="3" y="4" width="18" height="16" rx="2.5"/><path d="m7 9 3 3-3 3M13 15h4"/>',
  cols: '<rect x="3" y="4" width="18" height="16" rx="2.5"/><path d="M12 4v16"/>',
  laptop: '<rect x="4.5" y="5" width="15" height="10" rx="1.8"/><path d="M2.5 19h19l-1.8-3.4H4.3L2.5 19z"/>',
  phone: '<rect x="7" y="3" width="10" height="18" rx="2.6"/><path d="M11 17.8h2"/>',
  check: '<path d="m4.8 12.6 4.8 4.8L19.4 6.6"/>',
  x: '<path d="M6 6l12 12M18 6 6 18"/>',
  info: '<circle cx="12" cy="12" r="8.6"/><path d="M12 11v5M12 7.8h.01"/>',
  user: '<circle cx="12" cy="8" r="3.8"/><path d="M4.5 20.5c1.4-3.8 4.2-5.7 7.5-5.7s6.1 1.9 7.5 5.7"/>',
  eye: '<path d="M2.8 12S6.2 5.8 12 5.8 21.2 12 21.2 12 17.8 18.2 12 18.2 2.8 12 2.8 12z"/><circle cx="12" cy="12" r="2.9"/>',
  focus: '<path d="M4 9V5.5A1.5 1.5 0 0 1 5.5 4H9M15 4h3.5A1.5 1.5 0 0 1 20 5.5V9M20 15v3.5a1.5 1.5 0 0 1-1.5 1.5H15M9 20H5.5A1.5 1.5 0 0 1 4 18.5V15"/>',
  wand: '<path d="M4.5 19.5 15 9M15.5 3.5v4M13.5 5.5h4M19 11.5v3M17.5 13h3"/>',
  palette: '<path d="M12 3a9 9 0 1 0 0 18h1.6a2 2 0 0 0 1.4-3.4c-.8-.8-.2-2.1 1-2.1h1.9A3.6 3.6 0 0 0 21 11.6 9 9 0 0 0 12 3z"/><circle cx="8" cy="10" r="1.1" fill="currentColor" stroke="none"/><circle cx="12" cy="7.3" r="1.1" fill="currentColor" stroke="none"/><circle cx="16" cy="10" r="1.1" fill="currentColor" stroke="none"/>',
  refresh: '<path d="M20.5 12a8.5 8.5 0 1 1-2.5-6M20.5 3.5V8H16"/>',
  bookmark: '<path d="M6.5 3.5h11V20.5l-5.5-3.8-5.5 3.8V3.5z"/>',
  link: '<path d="M10.2 13.8a4.3 4.3 0 0 0 6.1 0l2.9-2.9a4.3 4.3 0 0 0-6.1-6.1l-1.4 1.4M13.8 10.2a4.3 4.3 0 0 0-6.1 0l-2.9 2.9a4.3 4.3 0 0 0 6.1 6.1l1.4-1.4"/>',
  globe: '<circle cx="12" cy="12" r="8.6"/><path d="M3.4 12h17.2M12 3.4a13.4 13.4 0 0 1 0 17.2 13.4 13.4 0 0 1 0-17.2z"/>',
  trash: '<path d="M4.5 7h15M9.5 7V4h5v3M6.5 7l1 13.5h9l1-13.5M10.2 11v6M13.8 11v6"/>',
  keyboard: '<rect x="3" y="7" width="18" height="11" rx="2"/><path d="M6.8 10.8h.01M10.4 10.8h.01M14 10.8h.01M17.4 10.8h.01M7.5 14.4h9"/>',
  back: '<path d="M19 12H5.5M11 6l-6 6 6 6"/>',
  chev: '<path d="m9.5 5.5 6.5 6.5-6.5 6.5"/>',
  more: '<circle cx="12" cy="5.2" r="1.4" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="1.4" fill="currentColor" stroke="none"/><circle cx="12" cy="18.8" r="1.4" fill="currentColor" stroke="none"/>',
  wifi: '<path d="M2.8 9.3a13.5 13.5 0 0 1 18.4 0M5.8 12.6a9.2 9.2 0 0 1 12.4 0M8.9 15.9a4.8 4.8 0 0 1 6.2 0M12 19.2h.01"/>',
  battery: '<rect x="2.5" y="8" width="16.5" height="8" rx="2"/><path d="M21.8 10.8v2.4"/><rect x="5" y="10.4" width="9.5" height="3.2" rx=".8" fill="currentColor" stroke="none"/>',
  shield: '<path d="M12 3l7.8 2.9v5.9c0 4.8-3.3 7.7-7.8 8.9-4.5-1.2-7.8-4.1-7.8-8.9V5.9L12 3z"/><path d="m9 11.8 2.1 2.1 4-4.4"/>',
  plus: '<path d="M12 5.5v13M5.5 12h13"/>',
};
const I = (n, cls = "") => `<svg class="ic ${cls}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${P[n] || ""}</svg>`;
const BRACES = '<span class="mono-ico">{ }</span>';

/* ---------- state ---------- */
let _id = 100;
const nid = (p) => p + (++_id);

const S = {
  mode: "mac",
  aCtrl: false,
  aPrev: "account",
  activeTab: "vault",
  vaultSection: "hosts",
  sidebarHidden: false,
  tabs: [
    { id: "vault", type: "vault", title: "Vault", icon: I("server") },
    { id: "sftp", type: "sftp", title: "SFTP", icon: I("folder") },
  ],
  hosts: [
    { id: "h1", name: "10.10.21.84", user: "x", port: 22, type: "ssh", os: "ubuntu", group: "prod" },
    { id: "h2", name: "10.88.2.11", user: "nian", port: 22, type: "ssh", os: "ubuntu", group: "prod" },
    { id: "h3", name: "10.87.102.179", user: "root", port: 22, type: "ssh", os: "generic", group: "lab" },
    { id: "h4", name: "Local", user: "nian", port: 0, type: "local", os: "local", group: "local", sub: "本机 shell · zsh" },
    { id: "h5", name: "home-win (RDP)", user: "nian", port: 3389, type: "rdp", os: "rdp", addr: "127.0.0.1", via: "h3", group: "lab", sub: "rdp · 经由 10.87.102.179 隧道" },
    { id: "h6", name: "lab-vnc (VNC)", user: "", port: 5901, type: "vnc", os: "vnc", addr: "127.0.0.1", via: "h1", group: "lab", sub: "vnc · 经由 10.10.21.84 隧道" },
  ],
  prefs: { theme: "harbor", fs: 13, lh: 1.5, autocomplete: false },
  filters: { hosts: "", keys: "", fwd: "", snip: "", known: "", logs: "" },
  hostSort: "name",
  hostView: "grid",
  hostTag: "all",
  groups: ["prod", "lab", "local"],
  logDesc: true,
  transfers: [],
  find: null,
  pal: null,
  loggedOut: false,
  keys: [
    { id: "k1", name: "id_ed25519", type: "ED25519", fp: "SHA256:9uQ4x…c21A", bio: true, used: 3 },
    { id: "k2", name: "deploy_rsa", type: "RSA 4096", fp: "SHA256:Kf2m…z7Pw", bio: false, used: 1 },
  ],
  forwards: [
    { id: "f1", kind: "Local", bind: "127.0.0.1:13306", dest: "db.internal:3306", via: "10.88.2.11", on: true, traffic: "12.4 MB" },
    { id: "f2", kind: "Dynamic", bind: "127.0.0.1:1080", dest: "SOCKS5", via: "10.10.21.84", on: false, traffic: "—" },
    { id: "f3", kind: "Remote", bind: "0.0.0.0:8443", dest: "127.0.0.1:443", via: "10.87.102.179", on: true, traffic: "3.1 MB" },
  ],
  snippets: [
    { id: "s1", title: "更新系统", body: "sudo apt update && sudo apt upgrade -y", scope: "全局" },
    { id: "s2", title: "Docker 清理", body: "docker system prune -af --volumes", scope: "分组: prod" },
    { id: "s3", title: "跟踪日志", body: "tail -f {{path}}", scope: "全局" },
  ],
  known: [
    { id: "n1", host: "10.10.21.84", kt: "ssh-ed25519", fp: "SHA256:aa41…9dQf", added: "2026-10-02" },
    { id: "n2", host: "10.88.2.11", kt: "ssh-ed25519", fp: "SHA256:bb17…k2Mw", added: "2026-10-14" },
    { id: "n3", host: "10.87.102.179", kt: "ecdsa-sha2-nistp256", fp: "SHA256:cc92…p0Xs", added: "2026-11-01" },
  ],
  logs: [
    { date: "2026-11-17 22:50", user: "nian", host: "10.88.2.11", proto: "SSH", result: "成功", dur: "2h 14m", saved: true },
    { date: "2026-11-17 21:12", user: "root", host: "10.87.102.179", proto: "SSH", result: "成功", dur: "36m", saved: false },
    { date: "2026-11-17 19:03", user: "nian", host: "home-win", proto: "RDP/Tunnel", result: "成功", dur: "1h 02m", saved: true },
    { date: "2026-11-17 10:44", user: "x", host: "10.10.21.84", proto: "SSH", result: "认证失败", dur: "—", saved: false },
    { date: "2026-11-16 23:31", user: "nian", host: "10.88.2.11", proto: "SFTP", result: "成功", dur: "12m", saved: false },
  ],
  notifications: [],
  devices: [
    { id: "d1", ico: "laptop", name: "Nian-MacBook Pro", sub: "macOS 27.0 · 当前设备 · 刚刚在线", current: true },
    { id: "d2", ico: "phone", name: "Pixel 8", sub: "Android 16 · 2 分钟前在线", current: false },
    { id: "d3", ico: "phone", name: "Xiaomi 15 (HyperOS)", sub: "Android 16 · 昨天 23:41 在线", current: false },
  ],
  sync: { hosts: true, keys: true, snippets: true, forwards: true, known: true, settings: true, history: false },
  sftp: { left: null, right: null, selected: null, pending: null, showLog: true },
};

const MOTD = [
  "Welcome to Ubuntu 24.04.4 LTS (GNU/Linux 7.0.0-43-generic x86_64)",
  "",
  " * Documentation:  https://help.ubuntu.com",
  " * Management:     https://landscape.canonical.com",
  " * Support:        https://ubuntu.com/pro",
  "",
  "Expanded Security Maintenance for Applications is not enabled.",
  "",
  "157 updates can be applied immediately.",
  "9 of these updates are standard security updates.",
  "To see these additional updates run: apt list --upgradable",
  "",
  "New release '26.04.1 LTS' available.",
  "Run 'do-release-upgrade' to upgrade to it.",
  "",
  "Last login: Tue Nov 17 22:50:31 2026 from 10.88.2.1",
];

const FILES = [
  ["..", "", "--", "folder"], [".ssh", "Nov 16, 9:32 PM", "--", "folder"],
  ["projects", "Nov 17, 9:56 PM", "--", "folder"], ["deploy", "Nov 15, 3:13 PM", "--", "folder"],
  ["backups", "Nov 5, 2:10 PM", "--", "folder"], ["ai-workspace", "Oct 25, 12:52 AM", "--", "folder"],
  ["logs", "Nov 17, 10:35 AM", "--", "folder"], ["docker", "Nov 6, 2:36 PM", "--", "folder"],
  [".config", "Nov 2, 10:37 AM", "--", "folder"], ["media", "Nov 4, 4:51 PM", "--", "folder"],
  ["scripts", "Oct 30, 4:20 PM", "--", "folder"], ["tmp", "Nov 15, 12:06 AM", "--", "folder"],
  ["app.tar.gz", "Nov 17, 11:32 AM", "412 MB", "archive"], ["notes.md", "Nov 17, 11:19 AM", "8 KB", "text"],
  ["docker-compose.yml", "Nov 15, 12:12 AM", "2 KB", "yaml"], [".bashrc", "Oct 12, 9:32 PM", "4 KB", "text"],
];

/* ---------- helpers ---------- */
const $ = (sel) => document.querySelector(sel);
const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const hostById = (id) => S.hosts.find((h) => h.id === id);
const tabById = (id) => S.tabs.find((t) => t.id === id);
const now = () => new Date().toTimeString().slice(0, 5);
const isAndroid = () => S.mode === "android";
const osIco = { ubuntu: I("ubuntu"), generic: I("asterisk"), local: '<span class="mono-ico">&gt;_</span>', vnc: I("monitor"), rdp: I("win") };

function switchTab(id) {
  S.activeTab = id;
  const t = tabById(id);
  if (t && t.dot && t.dot !== "share") t.dot = null;
  closeNotifPanel();
  render();
  focusTerminal();
}

function closeTab(id, ev) {
  if (ev) ev.stopPropagation();
  const i = S.tabs.findIndex((t) => t.id === id);
  if (i < 0) return;
  const t = S.tabs[i];
  (t.stageTimers || []).forEach(clearTimeout);
  (t.panes || [t.sess]).forEach((s) => s && s.timers && s.timers.forEach(clearTimeout));
  S.tabs.splice(i, 1);
  if (S.activeTab === id) {
    if (isAndroid()) { S.activeTab = "vault"; S.vaultSection = "sessions"; }
    else S.activeTab = S.tabs[Math.max(0, i - 1)].id;
  }
  render();
}

/* ---------- platform mode switch ---------- */
function setMode(m) {
  S.mode = m;
  S.aCtrl = false;
  closeModal(); closeNotifPanel(); closeMenu();
  document.body.classList.toggle("mode-android", m === "android");
  $("#window").classList.toggle("linux", m === "linux");
  $("#window").hidden = m === "android";
  $("#phone").hidden = m !== "android";
  const root = m === "android" ? $("#phone") : $("#window");
  ["notifPanel", "modalRoot", "toastStack"].forEach((id) => root.appendChild(document.getElementById(id)));
  document.querySelectorAll("#modeSwitch button").forEach((b) => b.classList.toggle("active", b.dataset.mode === m));
  $("#stageHint").innerHTML = m === "android"
    ? 'Harbor Android 原型 · 底部导航切换 主机/会话/凭据/我的 · 点主机卡片连接 · 终端输入 <code>ai-task</code> 后切到别的页体验 heads-up 提醒 · 辅助键条 <code>Ctrl</code>+<code>C</code> 可中断任务'
    : m === "linux"
    ? 'Harbor Linux 原型（GTK4 / libadwaita 风格窗控）· 功能与 macOS 版一致 · 终端输入 <code>ai-task</code> 体验会话完成监控'
    : 'Harbor 交互原型 · 演示提示：在终端输入 <code>ai-task</code> 体验会话完成监控；点击主机卡片建立连接；点击终端工具栏「共享」体验多端 Console。';
  render();
  focusTerminal();
}

/* ---------- notifications (FR-13) ---------- */
function notify({ kind = "ok", title, body, tabId }) {
  const n = { id: nid("n"), kind, title, body, tabId, time: now(), unread: true };
  S.notifications.unshift(n);
  updateBell();
  const focused = S.activeTab === tabId;
  if (!focused && tabId) {
    const t = tabById(tabId);
    if (t) t.dot = kind === "err" ? "err" : "ok";
    showToast(n);
    if (isAndroid()) { const cur = tabById(S.activeTab); if (cur && cur.type === "vault") render(); }
    else renderTabstrip();
  }
  if (!$("#notifPanel").hidden) renderNotifPanel();
}

function updateBell() {
  const c = S.notifications.filter((n) => n.unread).length;
  document.querySelectorAll(".badge-bell").forEach((b) => { b.hidden = c === 0; b.textContent = c; });
}

function showToast(n) {
  const stack = $("#toastStack");
  const d = document.createElement("div");
  d.className = "toast";
  d.innerHTML = `<div class="n-ico ${n.kind}">${n.kind === "ok" ? I("check") : n.kind === "err" ? I("x") : I("info")}</div>
    <div style="flex:1;min-width:0"><div class="n-title">${esc(n.title)}</div><div class="n-body">${esc(n.body)}</div>
    ${n.action ? `<button class="btn sm primary toast-act">${esc(n.action.label)}</button>` : ""}</div>
    <div class="t-app">Harbor · 现在</div>`;
  if (n.action) d.querySelector(".toast-act").onclick = (e) => { e.stopPropagation(); n.action.fn(); d.remove(); };
  d.onclick = () => {
    n.unread = false; updateBell();
    if (n.tabId && tabById(n.tabId)) switchTab(n.tabId);
    d.remove();
  };
  stack.appendChild(d);
  setTimeout(() => { d.classList.add("out"); setTimeout(() => d.remove(), 350); }, n.action ? 9000 : 6000);
}

function renderNotifPanel() {
  const p = $("#notifPanel");
  p.innerHTML = `<div class="notif-head"><span>通知中心</span>
    <span><button class="btn sm ghost" onclick="A.readAll()">全部已读</button>
    <button class="btn sm ghost" onclick="A.clearNotifs()">清空</button></span></div>` +
    (S.notifications.length === 0
      ? `<div style="padding:26px;text-align:center;color:var(--text-3);font-size:12.5px">暂无通知<br><span style="font-size:11.5px">会话完成提醒会出现在这里（FR-13）</span></div>`
      : S.notifications.map((n) => `
      <div class="notif-item ${n.unread ? "unread" : ""}" onclick="A.openNotif('${n.id}')">
        <div class="n-ico ${n.kind}">${n.kind === "ok" ? I("check") : n.kind === "err" ? I("x") : I("info")}</div>
        <div><div class="n-title">${esc(n.title)}</div><div class="n-body">${esc(n.body)}</div><div class="n-time">${n.time}</div></div>
      </div>`).join(""));
}
function closeNotifPanel() { $("#notifPanel").hidden = true; }

/* ---------- sessions / terminal ---------- */
function newSession(tabId, { host, local }) {
  return {
    id: nid("sess"), tabId, host, local,
    lines: [], hist: [], histIdx: -1,
    busy: false, ended: false,
    watch: true,
    watchCfg: { eof: true, osc133: true, ai: true, idle: false },
    shared: false, shareMode: "view", participants: [], timers: [],
    prompt: local ? "nian@Nian-MacBook:~$ " : `${host.user}@${host.name}:~$ `,
  };
}

function sessEl(sess) { return document.querySelector(`[data-sess="${sess.id}"]`); }

function appendLine(sess, text, cls = "") {
  sess.lines.push({ text, cls });
  const wrap = sessEl(sess);
  if (!wrap) return;
  const inputRow = wrap.querySelector(".term-inputline");
  const d = document.createElement("div");
  d.className = "term-line " + cls;
  d.textContent = text;
  wrap.insertBefore(d, inputRow);
  wrap.scrollTop = wrap.scrollHeight;
}

function later(sess, fn, ms) { sess.timers.push(setTimeout(fn, ms)); }

function runCommand(sess, raw) {
  const cmd = raw.trim();
  appendLine(sess, sess.prompt + raw, "white");
  if (!cmd) return;
  sess.hist.push(cmd); sess.histIdx = sess.hist.length;
  const [bin, ...args] = cmd.split(/\s+/);
  const out = (t, c) => appendLine(sess, t, c);
  switch (bin) {
    case "help":
      out("Harbor 演示终端 — 可用命令:", "cyan");
      out("  ls / pwd / whoami / uname -a / uptime / date / echo / clear");
      out("  ai-task     模拟 AI CLI 长任务（约 6s 后完成，触发监控提醒）");
      out("  fail-task   模拟失败任务（退出码 1，红色提醒）");
      out("  exit        结束会话（触发会话结束提醒）");
      break;
    case "ls": out("projects  deploy  logs  app.tar.gz  notes.md  docker-compose.yml"); break;
    case "pwd": out(sess.local ? "/Users/nian" : `/home/${sess.host.user}`); break;
    case "whoami": out(sess.local ? "nian" : sess.host.user); break;
    case "uname": out(sess.local ? "Darwin Nian-MacBook 27.0.0 arm64" : "Linux ubuntu 7.0.0-43-generic #44-Ubuntu SMP x86_64 GNU/Linux"); break;
    case "uptime": out(" 22:58:41 up 14 days,  3:22,  2 users,  load average: 0.42, 0.35, 0.31"); break;
    case "date": out(new Date().toString()); break;
    case "echo": out(args.join(" ")); break;
    case "clear": {
      sess.lines = [];
      const wrap = sessEl(sess);
      if (wrap) wrap.querySelectorAll(".term-line").forEach((e) => e.remove());
      break;
    }
    case "ai-task": aiTask(sess, true); break;
    case "fail-task": aiTask(sess, false); break;
    case "top": out("top - 22:58:41 up 14 days,  load average: 0.42", "dim"); out("Tasks: 213 total,   1 running, 212 sleeping", "dim"); out("%Cpu(s):  3.2 us,  1.1 sy, 95.7 id", "dim"); out("(演示模式：静态快照)", "dim"); break;
    case "exit": endSession(sess); break;
    default: out(`${bin}: command not found`, "red");
  }
}

function aiTask(sess, ok) {
  sess.busy = true;
  setInputVisible(sess, false);
  const steps = ok
    ? [["⠋ Analyzing repository…", 400], ["⠙ Reading 42 files, building context…", 1300], ["⠹ Planning changes (3 files)…", 2300], ["⠸ Applying edits to src/server.c …", 3400], ["⠼ Running tests: 128 passed, 0 failed", 4700], ["✔ Task completed — 3 files changed, tests green. (6.1s)", 6000]]
    : [["⠋ Building target harbor_core…", 400], ["⠙ CC src/vt.c", 1500], ["⠹ CC src/share.c", 2600], ["src/share.c:212: error: implicit declaration of 'relay_send'", 3800], ["✖ Build failed (exit code 1)", 4800]];
  steps.forEach(([t, ms], i) => later(sess, () => {
    appendLine(sess, t, i === steps.length - 1 ? (ok ? "" : "red") : "dim");
    if (i === steps.length - 1) {
      sess.busy = false;
      setInputVisible(sess, true);
      watchTrigger(sess, {
        ok, cmd: ok ? "ai-task" : "fail-task",
        rule: ok ? "AI CLI 完成预设（OSC 133 退出码 0）" : "OSC 133 退出码 1",
        dur: ok ? "6.1s" : "4.8s",
      });
    }
  }, ms));
}

function interruptSession(sess) {
  sess.timers.forEach(clearTimeout);
  sess.timers = [];
  sess.busy = false;
  appendLine(sess, "^C", "dim");
  setInputVisible(sess, true);
  watchTrigger(sess, { ok: false, cmd: "interrupt", rule: "SIGINT · Ctrl-C", dur: "" });
}

function watchTrigger(sess, { ok, cmd, rule, dur }) {
  if (!sess.watch) return;
  const tab = tabById(sess.tabId);
  const where = sess.local ? "Local" : sess.host.name;
  notify({
    kind: ok ? "ok" : "err",
    title: ok ? `会话任务完成 · ${where}` : `任务失败 · ${where}`,
    body: `${cmd} · ${rule}${dur ? " · 耗时 " + dur : ""} — 可以进行下一步了`,
    tabId: sess.tabId,
  });
  if (S.activeTab === sess.tabId) {
    // 聚焦中：不打扰，仅 Tab 角标
    if (tab) {
      tab.dot = ok ? "ok" : "err";
      if (!isAndroid()) renderTabstrip();
      setTimeout(() => { if (tab.dot !== "share") { tab.dot = null; if (!isAndroid()) renderTabstrip(); } }, 4000);
    }
  }
}

function endSession(sess) {
  sess.ended = true;
  appendLine(sess, "logout", "dim");
  appendLine(sess, sess.local ? "[本地进程已退出]" : "[连接已关闭 · SSH channel EOF]", "dim");
  setInputVisible(sess, false);
  const st = document.querySelector(`[data-status="${sess.id}"]`);
  if (st) st.innerHTML = `<span class="warn">● 会话已结束</span><span>监控信号: channel EOF</span>`;
  watchTrigger(sess, { ok: true, cmd: "exit", rule: "会话结束（EOF）", dur: "" });
}

function setInputVisible(sess, vis) {
  const wrap = sessEl(sess);
  if (!wrap) return;
  const row = wrap.querySelector(".term-inputline");
  if (row) row.style.display = vis ? "flex" : "none";
  if (vis) { const inp = row.querySelector("input"); if (inp && S.activeTab === sess.tabId) inp.focus(); wrap.scrollTop = wrap.scrollHeight; }
}

/* ---------- open hosts / tabs ---------- */
function openHost(hostId) {
  const host = hostById(hostId);
  if (!host) return;
  if (host.type === "local") return openLocal();
  if (host.type === "vnc" || host.type === "rdp") return openRemote(host);
  openSSH(host);
}

function openLocal() {
  const host = hostById("h4");
  const tab = { id: nid("t"), type: "term", title: "Local", icon: osIco.local, local: true };
  tab.sess = newSession(tab.id, { host, local: true });
  appendSeed(tab.sess, [["Harbor local shell — zsh (login)", "dim"], ["提示: 输入 help 查看演示命令", "dim"]]);
  S.tabs.push(tab);
  switchTab(tab.id);
}

function appendSeed(sess, lines) { lines.forEach(([t, c]) => sess.lines.push({ text: t, cls: c })); }

function openSSH(host) {
  if (!S.known.some((k) => k.host === host.name)) return tofuModal(host);
  startSSH(host, false);
}

function fakeFP(name) {
  let h = 0;
  for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return "SHA256:" + h.toString(36).padStart(8, "0").slice(0, 8) + "…";
}

function tofuModal(host) {
  const fp = fakeFP(host.name);
  modal(`<div class="modal"><div class="modal-head"><h3>${I("shield")} 首次连接 · 验证主机指纹</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body">
      <div class="hintbox" style="background:#fff7e8;border-color:#f3ddb0;color:#8a6116">该主机不在 known_hosts 中。请通过电话 / 控制台等带外渠道核对指纹后再信任，防止中间人攻击。</div>
      <div class="codebox"><span>${esc(host.name)}:${host.port}</span><span class="tag">ssh-ed25519</span></div>
      <div class="field"><label>主机公钥指纹 (SHA256)</label><div class="codebox"><span>${fp}</span><button class="btn sm" onclick="A.copyText('${fp}',this)">复制</button></div></div>
      <div class="check-row"><div class="cr-label"><span>信任后写入 known_hosts 并经 E2EE 同步</span><span class="cr-sub">之后指纹若变更将红色告警并阻断连接</span></div></div>
    </div>
    <div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.trustHost('${host.id}')">信任并继续</button></div></div>`);
}

function startSSH(host, isNew) {
  const tab = { id: nid("t"), type: "connect", title: host.name, icon: osIco[host.os], host };
  S.tabs.push(tab);
  switchTab(tab.id);
  runStages(tab, host, isNew);
}

function runStages(tab, host, isNew) {
  (tab.stageTimers || []).forEach(clearTimeout);
  const stages = [
    `Resolving ${host.name}…`,
    `Connecting to ${host.name}:${host.port}…`,
    isNew ? `Host key trusted & added to known_hosts (${fakeFP(host.name)})` : "Host key verified (known_hosts ✓)",
    "Authentication that can continue: publickey,keyboard-interactive,password",
    `Authenticating using publickey (id_ed25519${S.keys[0] && S.keys[0].bio ? " · Touch ID ✓" : ""})`,
    "Authentication succeeded (publickey)",
    `Authenticated to "${host.name}":"${host.port}"`,
    "Requesting shell / PTY (xterm-256color)…",
  ];
  tab.connLines = [];
  tab.stageTimers = [];
  stages.forEach((line, i) => tab.stageTimers.push(setTimeout(() => {
    tab.connLines.push(line);
    if (S.activeTab === tab.id) render();
    if (i === stages.length - 1) tab.stageTimers.push(setTimeout(() => {
      tab.type = "term";
      tab.sess = newSession(tab.id, { host, local: false });
      appendSeed(tab.sess, MOTD.map((t) => [t, ""]));
      S.logs.unshift({ date: "2026-11-17 " + now(), user: host.user, host: host.name, proto: "SSH", result: "成功", dur: "进行中", saved: false });
      if (S.activeTab === tab.id) { render(); focusTerminal(); } else if (!isAndroid()) renderTabstrip();
    }, 450));
  }, 320 * (i + 1))));
}

function openRemote(host) {
  const via = hostById(host.via);
  const localPort = host.type === "rdp" ? 53389 : 55901;
  const tab = { id: nid("t"), type: "remote", title: host.name, icon: osIco[host.os], host, via, localPort, tunnelReady: false };
  S.tabs.push(tab);
  switchTab(tab.id);
  setTimeout(() => { tab.tunnelReady = true; S.logs.unshift({ date: "2026-11-17 " + now(), user: host.user || "—", host: host.name, proto: host.type.toUpperCase() + "/Tunnel", result: "成功", dur: "进行中", saved: false }); if (S.activeTab === tab.id) render(); }, 1400);
}

function openWorkspace() {
  const tab = { id: nid("t"), type: "workspace", title: "Workspace", icon: I("cols"), focused: 0, broadcast: false, assist: true };
  const h1 = hostById("h2"), h2 = hostById("h3");
  tab.panes = [newSession(tab.id, { host: h1, local: false }), newSession(tab.id, { host: h2, local: false })];
  tab.panes.forEach((p) => appendSeed(p, MOTD.map((t) => [t, ""])));
  S.tabs.push(tab);
  switchTab(tab.id);
}

/* ---------- share (FR-12) ---------- */
function openShareModal(tabId) {
  const tab = tabById(tabId);
  const sess = tab.sess || tab.panes[tab.focused];
  const m = $("#modalRoot");
  m.hidden = false;
  const renderShare = () => {
    m.innerHTML = `<div class="modal"><div class="modal-head"><h3>共享 Console ${sess.shared ? '<span class="pill on"><span class="dot"></span>共享中</span>' : ""}</h3>
      <button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body">
      <div class="check-row"><div class="cr-label"><b>开启云端共享</b><span class="cr-sub">经 Relay 端到端加密转发，服务端无法解密画面（零知识）</span></div>
        <label class="switch"><input type="checkbox" name="opt" ${sess.shared ? "checked" : ""} onchange="A.toggleShare('${sess.id}','${tabId}',this.checked)"><span class="sl"></span></label></div>
      ${sess.shared ? `
      <div class="field"><label>参与权限</label><select name="opt" onchange="A.shareMode('${sess.id}',this.value)">
        <option value="view" ${sess.shareMode === "view" ? "selected" : ""}>仅观看（只读）</option>
        <option value="collab" ${sess.shareMode === "collab" ? "selected" : ""}>可协作输入</option></select></div>
      <div class="field"><label>共享链接（同账号设备自动可见）</label>
        <div class="codebox"><span>harbor://share/${sess.id.toUpperCase()}#k=9f2e…</span><button class="btn sm" onclick="A.copyLink(this)">复制</button></div></div>
      <div class="field"><label>配对码（30 分钟内有效）</label><div class="codebox"><span style="font-size:17px;letter-spacing:5px">482 913</span><span style="color:var(--text-3);font-size:11px">E2EE 密钥经配对码 PAKE 交换</span></div></div>
      <div class="field"><label>参与者 (${sess.participants.length})</label><div>
        ${sess.participants.map((p) => `<div class="participant"><span>${p.ico}</span><span>${esc(p.name)}</span>
          <span class="pill ${p.perm === "collab" ? "on" : "off"}">${p.perm === "collab" ? "可输入" : "只读"}</span><span class="spacer"></span>
          <button class="btn sm danger" onclick="A.kick('${sess.id}','${p.id}','${tabId}')">移除</button></div>`).join("") || `<div style="color:var(--text-3);font-size:12.5px;padding:6px 0">等待设备加入…（同账号的 ${isAndroid() ? "Nian-MacBook Pro" : "Pixel 8"} 将在几秒后发现此会话）</div>`}
      </div></div>
      <div class="hintbox">所有者关闭 App 或断线时共享自动结束；Tab 上的蓝点常亮表示正在共享。</div>
      ` : `<div class="hintbox">开启后可在 Android / 其它 Mac 上实时观看或接管本会话，多端同屏。画面与输入全程端到端加密。</div>`}
      </div>
      <div class="modal-foot">${sess.shared ? `<button class="btn danger" onclick="A.stopShare('${sess.id}','${tabId}')">停止共享</button>` : ""}
      <button class="btn primary" onclick="A.closeModal()">完成</button></div></div>`;
  };
  m._renderShare = renderShare;
  renderShare();
}

function findSess(sid) {
  for (const t of S.tabs) {
    if (t.sess && t.sess.id === sid) return t.sess;
    if (t.panes) { const p = t.panes.find((x) => x.id === sid); if (p) return p; }
  }
  return null;
}

/* ---------- render: tabstrip ---------- */
function renderTabstrip() {
  if (isAndroid()) return;
  const strip = $("#tabstrip");
  strip.innerHTML = S.tabs.map((t) => {
    const isTerm = t.type === "term" || t.type === "workspace" || t.type === "connect";
    const sess = t.sess || (t.panes && t.panes[0]);
    const sharing = sess && sess.shared || (t.panes && t.panes.some((p) => p.shared));
    const dot = sharing ? "share" : t.dot;
    return `<div class="tab ${isTerm ? "term" : ""} ${S.activeTab === t.id ? "active" : ""}" onclick="A.switchTab('${t.id}')">
      <span class="t-ico">${t.icon || I("server")}</span><span>${esc(t.title)}</span>
      ${dot ? `<span class="t-dot ${dot}"></span>` : ""}
      ${t.id !== "vault" && t.id !== "sftp" ? `<span class="t-close" onclick="A.closeTab('${t.id}',event)">${I("x", "sm")}</span>` : ""}
    </div>`;
  }).join("");
}

/* ---------- render: body ---------- */
function render() {
  if (isAndroid()) return renderAndroid();
  renderTabstrip();
  const body = $("#appBody");
  const tab = tabById(S.activeTab) || S.tabs[0];
  S.activeTab = tab.id;
  if (tab.type === "vault") body.innerHTML = vaultView();
  else if (tab.type === "sftp") body.innerHTML = sftpView();
  else if (tab.type === "connect") body.innerHTML = connectView(tab);
  else if (tab.type === "term") { body.innerHTML = termView(tab); mountSession(tab.sess); }
  else if (tab.type === "workspace") { body.innerHTML = workspaceView(tab); tab.panes.forEach(mountSession); }
  else if (tab.type === "remote") { body.innerHTML = remoteView(tab); scrollDw(); }
  updateBell();
  applyFind(false);
}

function focusTerminal() {
  const tab = tabById(S.activeTab);
  if (!tab) return;
  const sess = tab.sess || (tab.panes && tab.panes[tab.focused]);
  if (!sess) return;
  const wrap = sessEl(sess);
  if (wrap) { const inp = wrap.querySelector(".term-input"); if (inp && !sess.ended && !sess.busy) inp.focus(); }
}

/* ---------- vault ---------- */
const SECTIONS = [
  ["hosts", I("grid"), "Hosts"], ["keychain", I("key"), "Keychain"], ["forwarding", I("fwd"), "Port Forwarding"],
  ["snippets", BRACES, "Snippets"], ["known", I("shield"), "Known Hosts"], ["logs", I("clock"), "Logs"],
  ["shared", I("broadcast"), "共享会话"], ["settings", I("palette"), "外观与终端"], ["account", I("cloud"), "账号与同步"],
];

function vaultView() {
  const sec = S.vaultSection;
  return `${S.sidebarHidden ? "" : `<div class="sidebar">
    ${SECTIONS.map(([id, ico, lbl]) => `<button class="side-item ${sec === id ? "active" : ""}" onclick="A.section('${id}')"><span class="s-ico">${ico}</span><span class="lbl s-lbl">${lbl}</span></button>`).join("")}
    <div class="side-bottom"><div class="side-sep"></div>
      <button class="account-chip" onclick="A.section('account')"><span class="avatar">N</span>
        <span class="a-meta"><div class="a-name">nian@harbor.dev</div><div class="a-sub" style="${S.loggedOut ? "color:var(--text-3)" : ""}">${S.loggedOut ? "未登录 · 点击登录" : "● E2EE 同步已开启"}</div></span></button>
    </div></div>`}
  <div class="content">${vaultSectionView(sec)}</div>`;
}

function vaultSectionView(sec) {
  switch (sec) {
    case "hosts": return hostsView();
    case "sessions": return sessionsView();
    case "keychain": return keychainView();
    case "forwarding": return forwardingView();
    case "snippets": return snippetsView();
    case "known": return knownView();
    case "logs": return logsView();
    case "shared": return sharedView();
    case "settings": return settingsView();
    case "account": return accountView();
  }
}

function filteredHosts() {
  const q = S.filters.hosts.trim().toLowerCase();
  let list = S.hosts.filter((h) =>
    (S.hostTag === "all" || (S.hostTag === "tun" ? !!h.via : h.group === S.hostTag)) &&
    (!q || `${h.name} ${h.user} ${h.group || ""} ${h.type}`.toLowerCase().includes(q)));
  const last = (h) => { const l = S.logs.find((x) => x.host === h.name); return l ? l.date : ""; };
  if (S.hostSort === "name") list = list.slice().sort((a, b) => a.name.localeCompare(b.name));
  if (S.hostSort === "recent") list = list.slice().sort((a, b) => last(b).localeCompare(last(a)));
  if (S.hostSort === "type") list = list.slice().sort((a, b) => a.type.localeCompare(b.type) || a.name.localeCompare(b.name));
  return list;
}

function hostsView() {
  const list = filteredHosts();
  const tags = [["all", "全部"], ...S.groups.map((g) => [g, g]), ["tun", "SSH 隧道"]];
  return `<div class="quickbar"><div class="quickbar-inner">
      <input id="quickInput" placeholder="Find a host or ssh user@hostname…" value="${esc(S.filters.hosts)}" oninput="A.filt('hosts',this.value)" onkeydown="A.quickConnect(event)">
      <button class="kbd pal-kbd" onclick="A.palette()" title="全局命令面板">⌘K</button>
      <button class="btn sm" onclick="A.quickConnectBtn()">Connect</button></div></div>
    <div class="toolbar">
      <span class="btn-split"><button class="btn" onclick="A.newHostModal()">${I("plus")} New host</button><button class="btn" onclick="A.newHostMenu(event)">▾</button></span>
      <button class="btn ghost" onclick="A.toastInfo('串口终端为 P2 功能（FR-14）')">${I("plug")} Serial</button>
      <div class="spacer"></div>
      <select class="sortsel" name="sort" onchange="A.hostSortSet(this.value)" title="排序">
        <option value="name" ${S.hostSort === "name" ? "selected" : ""}>按名称</option>
        <option value="recent" ${S.hostSort === "recent" ? "selected" : ""}>按最近连接</option>
        <option value="type" ${S.hostSort === "type" ? "selected" : ""}>按协议</option></select>
      <div class="viewtoggle"><button class="icon-btn ${S.hostView === "grid" ? "on" : ""}" title="卡片视图" onclick="A.hostViewSet('grid')">${I("grid")}</button><button class="icon-btn ${S.hostView === "list" ? "on" : ""}" title="列表视图" onclick="A.hostViewSet('list')">${I("list")}</button></div>
    </div>
    <div class="chiprow">${tags.map(([id, lbl]) => `<button class="chip ${S.hostTag === id ? "on" : ""}" onclick="A.hostTagSet('${id}')">${lbl}</button>`).join("")}</div>
    <div class="scroll"><div class="section-title">Hosts · ${list.length}</div>
    ${list.length === 0 ? `<div class="empty" style="padding:36px 0"><div class="e-ico">${I("search", "xl")}</div><div class="e-title">没有匹配的主机</div>
      <div class="e-sub">换个关键词或分组试试，也可以直接新建一台。</div>
      <div style="display:flex;gap:8px;margin-top:6px"><button class="btn sm" onclick="A.filt('hosts','');A.hostTagSet('all')">清除筛选</button><button class="btn sm primary" onclick="A.newHostModal()">New host</button></div></div>`
    : S.hostView === "grid" ? `<div class="card-grid">
      ${list.map((h) => `<div class="host-card" ondblclick="A.openHost('${h.id}')" onclick="A.hostDrawer('${h.id}')" title="单击查看详情 · 双击连接">
        <div class="os-ico ${h.os}">${osIco[h.os]}</div>
        <div style="min-width:0"><div class="h-name">${esc(h.name)}
          ${h.via ? '<span class="tag tun">SSH 隧道</span>' : ""}
          ${h.type === "local" ? '<span class="tag">本地</span>' : ""}</div>
        <div class="h-sub">${esc(h.sub || `${h.type}, ${h.user}`)}${h.group ? ` · ${esc(h.group)}` : ""}</div></div>
        <span class="card-go" onclick="event.stopPropagation();A.openHost('${h.id}')" title="连接">${I("plug", "sm")}</span>
      </div>`).join("")}
    </div>` : `<table class="table">
      <tr><th style="width:34%">名称</th><th>用户</th><th>端口</th><th>协议</th><th>分组</th><th style="width:86px"></th></tr>
      ${list.map((h) => `<tr class="rowlink" onclick="A.hostDrawer('${h.id}')">
        <td><b><span class="os-ico ${h.os}" style="width:22px;height:22px;border-radius:6px;display:inline-flex;vertical-align:-6px;margin-right:8px">${osIco[h.os]}</span>${esc(h.name)}</b></td>
        <td>${esc(h.user || "—")}</td><td class="mono">${h.port || "—"}</td><td>${h.type.toUpperCase()}</td><td>${h.group ? `<span class="tag">${esc(h.group)}</span>` : "—"}</td>
        <td><button class="btn sm" onclick="event.stopPropagation();A.openHost('${h.id}')">连接</button></td></tr>`).join("")}
    </table>`}</div>`;
}

function sessionTabs() { return S.tabs.filter((t) => ["term", "workspace", "connect", "remote"].includes(t.type)); }

function sessionState(t) {
  if (t.type === "connect") return "连接中…";
  if (t.type === "remote") return t.tunnelReady ? "远程桌面 · 隧道已建立" : "建立 SSH 隧道…";
  const s = t.sess || (t.panes && t.panes[0]);
  if (!s) return "";
  return s.ended ? "已结束" : s.busy ? "任务运行中" : t.type === "workspace" ? `2 分屏 · ${t.broadcast ? "Broadcast 开" : "独立输入"}` : "空闲 · 等待输入";
}

function sessionsView() {
  const list = sessionTabs();
  return `<div class="toolbar"><span class="btn-split"><button class="btn" onclick="A.openLocal()">${I("terminal")} Local 终端</button><button class="btn" onclick="A.openWorkspace()">${I("cols")} Workspace</button></span><div class="spacer"></div></div>
    <div class="scroll"><div class="section-title">会话</div>
    ${list.length === 0 ? `<div class="empty" style="padding:40px 0"><div class="e-ico">${I("terminal", "xl")}</div><div class="e-title">暂无会话</div>
      <div class="e-sub">从 Hosts 双击主机建立连接，或新建 Local 终端 / Workspace 分屏。</div></div>`
    : `<div class="card-grid">${list.map((t) => {
      const s = t.sess || (t.panes && t.panes[0]);
      const dot = s && s.shared ? "share" : t.dot;
      const osk = t.type === "remote" ? t.host.os : t.local ? "local" : "ubuntu";
      return `<div class="host-card" onclick="A.switchTab('${t.id}')">
        <div class="os-ico ${osk}">${osIco[osk]}</div>
        <div><div class="h-name">${esc(t.title)} ${dot ? `<span class="t-dot ${dot}" style="position:static;display:inline-block"></span>` : ""}
          ${s && s.shared ? '<span class="tag share-tag">共享中</span>' : ""}</div>
        <div class="h-sub">${sessionState(t)}</div></div></div>`;
    }).join("")}</div>`}</div>`;
}

function keychainView() {
  const q = S.filters.keys.trim().toLowerCase();
  const rows = S.keys.filter((k) => !q || `${k.name} ${k.type} ${k.fp}`.toLowerCase().includes(q));
  return `<div class="toolbar">
      <span class="btn-split"><button class="btn" onclick="A.newKeyModal()">${I("plus")} New key</button><button class="btn" onclick="A.keyMenu(event)">▾</button></span>
      <button class="btn ghost" onclick="A.toastInfo('证书导入（P1）')">${I("cert")} Certificate</button>
      <button class="btn ghost" onclick="A.toastInfo('Touch ID / 生物识别解锁已在 macOS 生效')">${I("fingerprint")} Touch ID</button>
      <button class="btn ghost" onclick="A.toastInfo('FIDO2 安全密钥（P1，libfido2）')">${I("fido")} FIDO2</button>
      <div class="spacer"></div><div class="field" style="width:220px"><input type="text" name="field" placeholder="搜索密钥…" value="${esc(S.filters.keys)}" oninput="A.filt('keys',this.value)"></div></div>
    <div class="scroll"><table class="table">
      <tr><th>名称</th><th>类型</th><th>指纹</th><th>生物识别</th><th>被引用</th><th style="width:92px"></th></tr>
      ${rows.length === 0 ? `<tr><td colspan="6" style="text-align:center;color:var(--text-3);padding:26px">无匹配密钥</td></tr>` : rows.map((k) => `<tr><td><b>${I("key")} ${esc(k.name)}</b></td><td>${k.type}</td><td class="mono">${k.fp}</td>
        <td>${k.bio ? '<span class="pill on"><span class="dot"></span>Touch ID</span>' : '<span class="pill off">关闭</span>'}</td><td>${k.used} 台主机</td>
        <td><button class="icon-btn" title="复制公钥" onclick="A.copyPub('${k.id}')">${I("link", "sm")}</button><button class="icon-btn del" title="删除密钥" onclick="A.delKey('${k.id}')">${I("trash", "sm")}</button></td></tr>`).join("")}
    </table>
    <div class="hintbox" style="margin-top:14px">私钥经 Secure Enclave / Android Keystore 包装加密存储，同步走端到端加密（FR-11），服务端零明文。</div></div>`;
}

function forwardingView() {
  const q = S.filters.fwd.trim().toLowerCase();
  const rows = S.forwards.filter((f) => !q || `${f.kind} ${f.bind} ${f.dest} ${f.via}`.toLowerCase().includes(q));
  return `<div class="toolbar"><span class="btn-split"><button class="btn" onclick="A.newForwardModal()">${I("plus")} New forwarding</button><button class="btn" onclick="A.fwdMenu(event)">▾</button></span>
      <div class="spacer"></div><div class="field" style="width:240px"><input type="text" name="field" placeholder="搜索监听 / 目标 / 主机…" value="${esc(S.filters.fwd)}" oninput="A.filt('fwd',this.value)"></div></div>
    <div class="scroll"><table class="table">
      <tr><th>类型</th><th>监听</th><th>目标</th><th>经由 SSH 主机</th><th>流量</th><th>状态</th><th style="width:92px"></th></tr>
      ${rows.length === 0 ? `<tr><td colspan="7" style="text-align:center;color:var(--text-3);padding:26px">无匹配规则</td></tr>` : rows.map((f) => `<tr><td><span class="tag">${f.kind}</span></td><td class="mono">${f.bind}</td><td class="mono">${f.dest}</td>
        <td>${f.via}</td><td>${f.traffic}</td>
        <td><span class="pill ${f.on ? "on" : "off"}"><span class="dot"></span>${f.on ? "监听中" : "已停止"}</span></td>
        <td><label class="switch green"><input type="checkbox" name="opt" ${f.on ? "checked" : ""} onchange="A.toggleForward('${f.id}',this.checked)"><span class="sl"></span></label><button class="icon-btn del" title="删除规则" onclick="A.delForward('${f.id}')">${I("trash", "sm")}</button></td></tr>`).join("")}
    </table>
    <div class="hintbox" style="margin-top:14px">VNC / RDP 主机勾选「SSH 隧道」后会自动创建临时 Local 规则：本机随机端口 → 服务器回环 127.0.0.1:3389/5901，桌面会话关闭即拆除（FR-07）。</div></div>`;
}

function snippetsView() {
  const q = S.filters.snip.trim().toLowerCase();
  const rows = S.snippets.filter((s) => !q || `${s.title} ${s.body} ${s.scope}`.toLowerCase().includes(q));
  return `<div class="toolbar"><span class="btn-split"><button class="btn" onclick="A.newSnippetModal()">${I("plus")} New snippet</button><button class="btn" onclick="A.snipMenu(event)">▾</button></span>
      <button class="btn ghost" onclick="A.toastInfo('Shell History：基于 OSC 133 采集，默认不同步（隐私）')">${I("clock")} Shell History</button>
      <div class="spacer"></div><div class="field" style="width:220px"><input type="text" name="field" placeholder="搜索 Snippets…" value="${esc(S.filters.snip)}" oninput="A.filt('snip',this.value)"></div></div>
    <div class="scroll"><div class="list-plain">
      ${rows.length === 0 ? `<div class="empty" style="padding:30px 0"><div class="e-ico">${I("search", "xl")}</div><div class="e-title">无匹配 Snippet</div></div>` : rows.map((s) => `<div class="snippet-card" onclick="A.sendSnippet('${s.id}')" title="点击发送到当前终端">
        <div class="sc-title">${esc(s.title)} <span class="tag">${esc(s.scope)}</span><span class="spacer"></span>
          <button class="icon-btn" title="编辑" onclick="event.stopPropagation();A.editSnippet('${s.id}')">${I("wand", "sm")}</button>
          <button class="icon-btn del" title="删除" onclick="event.stopPropagation();A.delSnippet('${s.id}')">${I("trash", "sm")}</button></div>
        <div class="sc-code">${esc(s.body)}</div></div>`).join("")}
    </div></div>`;
}

function knownView() {
  const q = S.filters.known.trim().toLowerCase();
  const rows = S.known.filter((k) => !q || `${k.host} ${k.kt} ${k.fp}`.toLowerCase().includes(q));
  return `<div class="toolbar"><button class="btn" onclick="A.importKnown()">${I("plus")} Import</button>
      <div class="field" style="flex:1;max-width:300px"><input type="text" name="field" placeholder="搜索主机 / 指纹…" value="${esc(S.filters.known)}" oninput="A.filt('known',this.value)"></div>
      <div class="spacer"></div></div>
    <div class="scroll"><table class="table">
      <tr><th>主机</th><th>密钥类型</th><th>指纹 (SHA256)</th><th>添加时间</th><th style="width:60px"></th></tr>
      ${rows.length === 0 ? `<tr><td colspan="5" style="text-align:center;color:var(--text-3);padding:26px">无匹配条目</td></tr>` : rows.map((k) => `<tr><td><b>${k.host}</b></td><td class="mono">${k.kt}</td><td class="mono">${k.fp}</td><td>${k.added}</td>
        <td><button class="icon-btn del" title="删除条目" onclick="A.delKnown('${k.id}')">${I("trash", "sm")}</button></td></tr>`).join("")}
    </table>
    <div class="hintbox" style="margin-top:14px">首次连接未知主机时会弹出指纹确认（TOFU）；指纹变更会红色告警并阻断连接，防中间人（FR-04）。</div></div>`;
}

function logsView() {
  const q = S.filters.logs.trim().toLowerCase();
  let rows = S.logs.map((l, i) => ({ l, i })).filter(({ l }) => !q || `${l.host} ${l.user} ${l.proto} ${l.result}`.toLowerCase().includes(q));
  if (!S.logDesc) rows = rows.slice().reverse();
  return `<div class="toolbar"><div class="field" style="flex:1;max-width:420px"><input type="text" name="field" placeholder="按主机 / 用户 / 结果筛选…" value="${esc(S.filters.logs)}" oninput="A.filt('logs',this.value)"></div>
      <div class="spacer"></div><button class="btn" onclick="A.exportLogs()">${I("bookmark")} 导出 CSV</button></div>
    <div class="scroll"><table class="table">
      <tr><th><button class="th-sort" onclick="A.logSort()">Date ${S.logDesc ? "↓" : "↑"}</button></th><th>User</th><th>Host</th><th>协议</th><th>结果</th><th>时长</th><th>Saved ${I("bookmark", "sm")}</th></tr>
      ${rows.length === 0 ? `<tr><td colspan="7" style="text-align:center;color:var(--text-3);padding:26px">无匹配记录</td></tr>` : rows.map(({ l, i }) => `<tr><td class="mono">${l.date}</td><td>${l.user}</td><td><b>${l.host}</b></td><td>${l.proto}</td>
        <td><span class="pill ${l.result === "成功" ? "on" : "err"}">${l.result}</span></td><td>${l.dur}</td>
        <td><button class="icon-btn bm ${l.saved ? "on" : ""}" title="${l.saved ? "取消 Saved" : "标记 Saved"}" onclick="A.logSaved(${i})">${I("bookmark", "sm")}</button></td></tr>`).join("")}
    </table></div>`;
}

function sharedView() {
  const shared = [];
  S.tabs.forEach((t) => {
    (t.panes || [t.sess]).forEach((s) => { if (s && s.shared) shared.push({ t, s }); });
  });
  return `<div class="toolbar"><button class="btn" onclick="A.joinByCode()">${I("link")} 通过配对码加入</button><div class="spacer"></div></div>
    <div class="scroll">
      <div class="section-title">本机正在共享的会话</div>
      ${shared.length === 0 ? `<div class="empty" style="padding:40px 0"><div class="e-ico">${I("broadcast", "xl")}</div><div class="e-title">暂无共享会话</div>
        <div class="e-sub">在任意终端 Tab 的工具栏点击「共享」即可开启多端同屏（FR-12）。同账号其它设备会自动发现。</div></div>`
      : `<div class="card-grid">${shared.map(({ t, s }) => `<div class="host-card" onclick="A.switchTab('${t.id}')">
          <div class="os-ico ubuntu">${I("broadcast")}</div><div><div class="h-name">${esc(t.title)} <span class="tag share-tag">${s.shareMode === "collab" ? "可协作" : "只读"}</span></div>
          <div class="h-sub">${s.participants.length} 个参与者 · E2EE · Relay 零知识</div></div></div>`).join("")}</div>`}
      <div class="section-title" style="margin-top:22px">来自其它设备的共享（同账号自动发现）</div>
      <div class="card-grid"><div class="host-card" onclick="A.joinPeer()">
        <div class="os-ico local">${I("phone")}</div><div><div class="h-name">Pixel 8 · ssh nian@10.88.2.11</div><div class="h-sub">只读 · 3 分钟前开始 · 点击加入观看</div></div></div></div>
    </div>`;
}

function accountView() {
  if (S.loggedOut) return `<div class="scroll" style="padding-top:14px">
    <div class="panel" style="max-width:460px;margin:26px auto">
      <h4>${I("cloud")} 登录 Harbor 账号</h4>
      <div class="p-sub">登录后从云端拉取密文并在本地解密，恢复全部 Vault 与同步。</div>
      <div class="field"><label>邮箱</label><input type="text" name="field" value="nian@harbor.dev"></div>
      <div class="field" style="margin-top:10px"><label>主密码</label><input type="password" name="passphrase" placeholder="经 Argon2id 派生主密钥"></div>
      <div class="field" style="margin-top:10px"><label>TOTP 验证码</label><input type="text" name="field" placeholder="123 456"></div>
      <div style="margin-top:14px;display:flex;gap:9px"><button class="btn primary" onclick="A.login()">${I("lock", "sm")} 登录并同步</button><button class="btn" onclick="A.recoveryModal()">用恢复码恢复</button></div>
      <div class="hintbox" style="margin-top:12px">零知识架构：密码错误则无法解密 Vault，服务端不能帮你重置。</div>
    </div></div>`;
  return `<div class="scroll" style="padding-top:14px">
    <div class="e2ee-banner"><span class="lock">${I("lock", "xl")}</span><div><div class="t1">端到端加密同步已开启 — 服务端零知识</div>
      <div class="t2">主密钥由密码经 Argon2id 派生；Vault 数据在设备端用 XChaCha20-Poly1305 加密后才上传。服务端仅存密文，无法解密任何凭据。</div></div>
      <div style="margin-left:auto"><button class="btn sm" onclick="A.recoveryModal()">恢复码</button></div></div>
    <div class="acct-grid">
      <div class="panel"><h4>${I("user")} 账号</h4>
        <div class="device-row"><span class="avatar" style="width:34px;height:34px;font-size:14px">N</span>
          <div><div class="d-name">nian@harbor.dev</div><div class="d-sub">TOTP 两步验证已开启 · 上次同步 ${now()}</div></div>
          <span class="spacer"></span><button class="btn sm" onclick="A.logoutModal()">退出</button></div>
        <h4 style="margin-top:16px">${I("phone")} 已登录设备</h4>
        ${S.devices.map((d) => `<div class="device-row"><span class="d-ico">${I(d.ico, "lg")}</span>
          <div><div class="d-name">${esc(d.name)} ${d.current ? '<span class="tag">当前</span>' : ""}</div><div class="d-sub">${esc(d.sub)}</div></div>
          <span class="spacer"></span>${d.current ? "" : `<button class="btn sm danger" onclick="A.revokeDevice('${d.id}')">吊销</button>`}</div>`).join("")}
      </div>
      <div class="panel"><h4>${I("cloud")} 同步范围</h4><div class="p-sub">关闭 history 以外的项目默认同步；冲突采用版本向量 + LWW 合并，同字段冲突保留双版本供选择。</div>
        ${Object.entries({ hosts: "主机与分组", keys: "密钥与凭据（E2EE）", snippets: "Snippets", forwards: "端口转发规则", known: "Known Hosts", settings: "外观与设置", history: "Shell History（默认关）" }).map(([k, lbl]) =>
          `<div class="sync-row"><span>${lbl}</span><label class="switch green"><input type="checkbox" name="opt" ${S.sync[k] ? "checked" : ""} onchange="A.toggleSync('${k}',this.checked)"><span class="sl"></span></label></div>`).join("")}
        <div class="hintbox" style="margin-top:12px">新设备登录同账号后自动拉取密文并在本地解密。忘记密码可用 24 词恢复码恢复；两者皆无则数据不可恢复（零知识代价）。</div>
      </div>
    </div></div>`;
}

/* ---------- connect view ---------- */
function connectView(tab) {
  const lines = tab.connLines || [];
  const done = lines.length;
  const nodeIcos = [I("link", "sm"), I("shield", "sm"), I("terminal", "sm")];
  const nodes = [0, 1, 2].map((i) => {
    const active = done > i * 3, cur = done >= i * 3 && done < (i + 1) * 3;
    return `<div class="node ${active ? "" : "pending"} ${cur ? "spin" : ""}">${nodeIcos[i]}</div>${i < 2 ? `<div class="bar ${done > (i + 1) * 3 ? "" : "pending"}"></div>` : ""}`;
  }).join("");
  return `<div class="connect-wrap">
    <div class="connect-head"><div class="os-ico ${tab.host.os}">${osIco[tab.host.os]}</div>
      <div><div class="h-name" style="font-weight:700">${esc(tab.host.name)}</div><div class="h-sub" style="color:var(--text-3);font-size:12px">SSH ${esc(tab.host.name)}:${tab.host.port}</div></div>
      <div style="margin-left:auto"><button class="btn sm" onclick="A.connHideLog('${tab.id}')">${tab.hideLog ? "Show logs" : "Hide logs"}</button></div></div>
    <div class="progress-track">${nodes}</div>
    ${tab.hideLog ? "" : `<div class="connect-log">${lines.map((l) => `<div>${I("user", "sm")} ${esc(l)}</div>`).join("")}</div>`}
    <div style="max-width:430px;margin:18px auto;display:flex;gap:9px"><button class="btn" onclick="A.closeTab('${tab.id}')">Close</button>
      <button class="btn" onclick="A.editHostModal('${tab.host.id}')">Edit host</button><div style="flex:1"></div><button class="btn primary" onclick="A.connRestart('${tab.id}')">Start over</button></div>
  </div>`;
}

/* ---------- terminal view ---------- */
function termToolbar(tab, sess) {
  return `<div class="term-toolbar">
    <span class="tt-title">${sess.local ? '<span class="mono-ico">&gt;_</span> Local · zsh' : `${I("ubuntu")} ssh ${esc(sess.host.user)}@${esc(sess.host.name)}`}</span>
    <div class="spacer"></div>
    <button class="tbtn ${sess.watch ? "on" : ""}" onclick="A.watchModal('${sess.id}','${tab.id}')">${I("eye")} 监控 ${sess.watch ? "开" : "关"}</button>
    <button class="tbtn ${sess.shared ? "share-on" : ""}" onclick="A.openShareModal('${tab.id}')">${I("broadcast")} 共享${sess.shared ? "中" : ""}</button>
    <button class="tbtn" onclick="A.openFind('${tab.id}')" title="终端内搜索 ⌘F">${I("search")}</button>
    ${sess.ended ? `<button class="tbtn" onclick="A.reconnect('${tab.id}')">${I("refresh")} 重连</button>` : ""}
  </div>`;
}

function findBar(sess) {
  if (!S.find || S.find.sid !== sess.id) return "";
  return `<div class="find-bar"><span>${I("search", "sm")}</span>
    <input data-filt="find" placeholder="在 scrollback 中搜索…" value="${esc(S.find.q)}" oninput="A.findInput(this.value)" onkeydown="A.findKey(event)">
    <span class="f-count mono" id="findCount">0/0</span>
    <button class="icon-btn" title="上一个" onclick="A.findNext(-1)">↑</button>
    <button class="icon-btn" title="下一个" onclick="A.findNext(1)">↓</button>
    <button class="icon-btn" title="关闭" onclick="A.closeFind()">${I("x", "sm")}</button></div>`;
}

function termBody(sess) {
  return `<div class="term-scroll" data-sess="${sess.id}" onclick="A.termClick(event,'${sess.id}')">
    ${sess.lines.map((l) => `<div class="term-line ${l.cls}">${esc(l.text)}</div>`).join("")}
    <div class="term-inputline" style="display:${sess.busy || sess.ended || sess.readonly ? "none" : "flex"}">
      <span class="term-prompt">${esc(sess.prompt)}</span><input class="term-input" name="cmd" aria-label="终端命令输入" spellcheck="false" autocomplete="off">
    </div></div>`;
}

function termStatus(sess) {
  return `<div class="term-status" data-status="${sess.id}">
    ${sess.ended ? `<span class="warn">● 会话已结束</span>` : `<span class="ok">● 已连接</span>`}
    <span>xterm-256color</span><span>UTF-8</span>
    <span>监控: ${sess.watch ? "EOF + OSC133 + AI 预设" : "关"}</span>
    ${sess.shared ? `<span style="color:#6cb8ff">${I("broadcast", "sm")} 共享中 · ${sess.participants.length} 人</span>` : ""}
    <span style="margin-left:auto">scrollback 10,000 行</span></div>`;
}

function termView(tab) {
  const peer = tab.peer ? `<div class="peer-banner">${I("phone", "sm")} 正在观看 Pixel 8 的会话 · 只读 · E2EE Relay<span class="spacer"></span><button class="tbtn" onclick="A.closeTab('${tab.id}')">离开</button></div>` : "";
  return `<div class="term-view">${peer}${termToolbar(tab, tab.sess)}${findBar(tab.sess)}${termBody(tab.sess)}${termStatus(tab.sess)}</div>`;
}

function workspaceView(tab) {
  const at = tab.assistTab || "snip";
  const assistBody = at === "snip"
    ? `<div style="color:#64718c;font-size:11.5px;line-height:1.7;margin-top:6px">Snippets 快捷发送：<br>${S.snippets.map((s) => `<div style="margin-top:6px"><button class="tbtn" style="width:100%;justify-content:flex-start" onclick="A.sendSnippet('${s.id}')">${BRACES} ${esc(s.title)}</button></div>`).join("")}</div>`
    : at === "theme"
    ? `<div style="color:#64718c;font-size:11.5px;margin-top:6px">终端主题（实时生效）：</div>${THEMES.map(([id, lbl]) => `<button class="tbtn ${S.prefs.theme === id ? "on" : ""}" style="width:100%;justify-content:flex-start;margin-top:6px" onclick="A.setPref('theme','${id}')"><span class="sw-dot" style="background:${swatch(id)}"></span>${lbl}</button>`).join("")}`
    : `<div class="a-row" style="margin-top:8px"><span>Autocomplete <span class="beta">BETA</span></span>
        <label class="switch green"><input type="checkbox" name="opt" ${S.prefs.autocomplete ? "checked" : ""} onchange="A.setPref('autocomplete',this.checked)"><span class="sl"></span></label></div>
       <div style="color:#64718c;font-size:11.5px;line-height:1.7;margin-top:8px">基于 Shell History 与 OSC 133 上下文的本地补全候选；默认关闭，历史不出设备（隐私）。</div>`;
  return `<div class="term-view">
    <div class="term-toolbar"><span class="tt-title">${I("cols")} Workspace · 2 分屏</span><div class="spacer"></div>
      <button class="tbtn ${tab.broadcast ? "on" : ""}" onclick="A.toggleBroadcast('${tab.id}')">${I("broadcast")} Broadcast ${tab.broadcast ? "开" : "关"}</button>
      <button class="tbtn ${tab.assist ? "on" : ""}" onclick="A.toggleAssist('${tab.id}')">${I("panel")} 助手栏</button>
      <button class="tbtn ${tab.focusMode ? "on" : ""}" onclick="A.toggleFocus('${tab.id}')">${I("focus")} Focus</button></div>
    ${findBar(tab.panes[tab.focused])}
    <div class="workspace ${tab.focusMode ? "focusmode" : ""}">
      ${tab.panes.map((p, i) => `<div class="pane ${tab.focused === i ? "focused" : ""} ${tab.broadcast ? "broadcast" : ""}" onclick="A.focusPane('${tab.id}',${i})">
        <div class="pane-head"><span>${I("ubuntu", "sm")} ${esc(p.host.name)} · ssh, ${esc(p.host.user)}</span><span class="spacer"></span>
          <button class="tbtn ${p.shared ? "share-on" : ""}" style="padding:2px 8px" onclick="event.stopPropagation();A.openShareModal('${tab.id}')">${I("broadcast", "sm")}</button>
          <button class="tbtn ${p.watch ? "on" : ""}" style="padding:2px 8px" onclick="event.stopPropagation();A.watchModal('${p.id}','${tab.id}')">${I("eye", "sm")}</button>
          ${p.ended ? `<button class="tbtn" style="padding:2px 8px" onclick="event.stopPropagation();A.reconnectPane('${tab.id}',${i})">${I("refresh", "sm")}</button>` : ""}</div>
        ${termBody(p)}</div>`).join("")}
      ${tab.assist ? `<div class="assist">
        <div class="a-tabs"><button class="tbtn ${at === "snip" ? "on" : ""}" onclick="A.assistTab('${tab.id}','snip')" title="Snippets">${BRACES}</button><button class="tbtn ${at === "theme" ? "on" : ""}" onclick="A.assistTab('${tab.id}','theme')" title="主题">${I("palette")}</button><button class="tbtn ${at === "auto" ? "on" : ""}" onclick="A.assistTab('${tab.id}','auto')" title="Autocomplete">${I("wand")}</button></div>
        <div class="a-search" style="cursor:pointer" onclick="A.openFind('${tab.id}')"><span>${I("search", "sm")} Search terminal · ⌘F</span><span>⌃ ⌄</span></div>
        ${assistBody}
      </div>` : ""}
    </div>${termStatus(tab.panes[tab.focused])}</div>`;
}

function mountSession(sess) {
  const wrap = sessEl(sess);
  if (!wrap) return;
  const inp = wrap.querySelector(".term-input");
  wrap.scrollTop = wrap.scrollHeight;
  if (!inp) return;
  inp.addEventListener("keydown", (e) => {
    if (isAndroid() && S.aCtrl) {
      S.aCtrl = false; refreshKeybar();
      const k = e.key.toLowerCase();
      if (k === "c") { e.preventDefault(); if (sess.busy) interruptSession(sess); else appendLine(sess, "^C", "dim"); return; }
      if (k === "d") { e.preventDefault(); endSession(sess); return; }
      if (k === "l") { e.preventDefault(); runCommand(sess, "clear"); return; }
    }
    if (e.key === "c" && e.ctrlKey && sess.busy) { e.preventDefault(); interruptSession(sess); return; }
    if (e.key === "Enter") {
      const v = inp.value; inp.value = "";
      const tab = tabById(sess.tabId);
      runCommand(sess, v);
      if (tab && tab.type === "workspace" && tab.broadcast) {
        tab.panes.forEach((p) => { if (p !== sess && !p.ended && !p.busy) runCommand(p, v); });
      }
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      if (sess.histIdx > 0) { sess.histIdx--; inp.value = sess.hist[sess.histIdx] || ""; }
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      if (sess.histIdx < sess.hist.length) { sess.histIdx++; inp.value = sess.hist[sess.histIdx] || ""; }
    } else if (e.key === "l" && e.ctrlKey) { e.preventDefault(); runCommand(sess, "clear"); }
  });
}

/* ---------- remote (VNC/RDP) ---------- */
function remoteView(tab) {
  const h = tab.host;
  if (!tab.tunnelReady) {
    return `<div class="connect-wrap"><div class="connect-head"><div class="os-ico ${h.os}">${osIco[h.os]}</div>
      <div><div style="font-weight:700">${esc(h.name)}</div><div style="color:var(--text-3);font-size:12px">${h.type.toUpperCase()} · 经由 SSH 隧道</div></div></div>
      <div class="connect-log"><div>${I("link", "sm")} 建立 SSH 连接 ${esc(tab.via.name)}…</div><div>${I("key", "sm")} publickey 认证成功</div>
      <div>${I("fwd", "sm")} 开通 Local 转发 127.0.0.1:${tab.localPort} → 127.0.0.1:${h.port}（目标回环）…</div>
      <div>${I("monitor", "sm")} ${h.type.toUpperCase()} 握手中…</div></div></div>`;
  }
  const isRdp = h.type === "rdp";
  if (!tab.rout) {
    tab.rout = isRdp
      ? ["PS C:\\Users\\nian> Get-Service sshd", "", "Status   Name    DisplayName", "------   ----    -----------", "Running  sshd    OpenSSH SSH Server", ""]
      : ["nian@lab:~$ systemctl status vncserver", "● vncserver@:1.service - TigerVNC", "   Active: active (running)", "   Listen: 127.0.0.1:5901 (loopback only)"];
    tab.rbuf = ""; tab.rpos = 0; tab.rhist = []; tab.rhi = 0; tab.rmod = null; tab.startMenu = false;
  }
  if (!tab.quality) tab.quality = "均衡";
  const prompt = isRdp ? "PS C:\\Users\\nian> " : "nian@lab:~$ ";
  const bodyHtml = tab.rout.map((l) => esc(l)).join("<br>") + (tab.rout.length ? "<br>" : "") +
    esc(prompt) + esc(tab.rbuf.slice(0, tab.rpos)) + '<span class="rcur">█</span>' + esc(tab.rbuf.slice(tab.rpos));
  return `<div class="remote-view">
    <div class="tunnel-banner">${I("lock", "sm")} 经由 <b>&nbsp;${esc(tab.via.user)}@${esc(tab.via.name)}&nbsp;</b> SSH 隧道 · 127.0.0.1:${tab.localPort} → 目标 127.0.0.1:${h.port} · 目标服务未暴露公网
      <span class="spacer"></span>
      ${tab.rmod ? `<span class="pill on"><span class="dot"></span>${tab.rmod.toUpperCase()}</span>` : `<span>${isRdp ? "NLA ✓ · 剪贴板双向 ✓" : "Tight 编码"}</span>`}
      <select class="qsel" name="opt" title="画质档位" onchange="A.rdpQuality('${tab.id}',this.value)">
        ${["流畅", "均衡", "高清"].map((q) => `<option ${tab.quality === q ? "selected" : ""}>${q}</option>`).join("")}</select>
      <button class="tbtn" onclick="A.closeTab('${tab.id}')">断开</button></div>
    <div class="desktop-wrap" tabindex="0" onkeydown="A.remoteKey(event,'${tab.id}')" title="点击桌面后可直接键入命令">
      <div class="desktop ${isRdp ? "" : "vnc"}">
      <div class="desk-area">
        <div class="desk-icon" style="left:3%;top:5%" ondblclick="A.toastInfo('回收站已打开（演示）')"><span class="di">${I("trash", "lg")}</span>回收站</div>
        <div class="desk-icon" style="left:3%;top:26%" ondblclick="A.toastInfo('projects 已打开（演示）')"><span class="di">${I("folder", "lg")}</span>projects</div>
        <div class="desk-icon" style="left:3%;top:47%" ondblclick="A.toastInfo('此电脑已打开（演示）')"><span class="di">${I("monitor", "lg")}</span>此电脑</div>
        <div class="desk-win"><div class="dw-head">${isRdp ? `${I("win", "sm")} Windows PowerShell` : `${I("terminal", "sm")} xterm — lab`}<span style="margin-left:auto">— ▢ ✕</span></div>
          <div class="dw-body" id="dwBody">${bodyHtml}</div></div>
        ${tab.startMenu ? `<div class="start-menu">${[["电源", I("plug")], ["设置", I("wand")], ["文件资源管理器", I("folder")], [isRdp ? "PowerShell" : "xterm", I("terminal")]].map(([n, ic]) => `<button onclick="A.toastInfo('${n} 已打开（演示）');A.remoteKeyBtn('${tab.id}','esc')">${ic} ${n}</button>`).join("")}</div>` : ""}
      </div>
      <div class="desk-taskbar"><span class="tb-start" style="cursor:pointer" onclick="A.remoteKeyBtn('${tab.id}','win')">${isRdp ? I("win") : I("asterisk")}</span><span>${I("search")}</span><span>${I("folder")}</span><span>${I("globe")}</span><span class="spacer"></span><span>ENG</span><span>${I("wifi")} ${I("battery")}</span><span>${now()}</span></div>
    </div></div>
    <div class="virtual-keys"><span class="vk-label">辅助键：</span>
      ${[["esc", "Esc"], ["ctrl", "Ctrl"], ["alt", "Alt"], ["win", "⊞ Win"], ["tab", "Tab"], ["back", "⌫"], ["up", "↑"], ["down", "↓"], ["left", "←"], ["right", "→"], ["enter", "⏎"]].map(([k, l]) => `<button class="vk ${tab.rmod === k ? "on" : ""}" onclick="A.remoteKeyBtn('${tab.id}','${k}')">${l}</button>`).join("")}
      <span style="color:#64718c;font-size:11px;margin-left:auto">单指=移动/点击 · 双指=缩放 · 长按=右键</span></div>
  </div>`;
}

function scrollDw() { const dw = $("#dwBody"); if (dw) dw.scrollTop = dw.scrollHeight; }

function remoteExec(t) {
  const cmd = t.rbuf.trim();
  const isRdp = t.host.type === "rdp";
  const prompt = isRdp ? "PS C:\\Users\\nian> " : "nian@lab:~$ ";
  t.rout.push(prompt + t.rbuf);
  if (cmd) {
    t.rhist.push(cmd); t.rhi = t.rhist.length;
    const c = cmd.toLowerCase();
    if (c === "get-service sshd" || c === "systemctl status vncserver") t.rout.push("Running  ✓  (loopback only)");
    else if (c === "ls" || c === "dir") t.rout.push("projects  deploy  notes.md  app.tar.gz");
    else if (c === "clear" || c === "cls") t.rout = [];
    else if (c.startsWith("echo ")) t.rout.push(cmd.slice(5));
    else if (c === "whoami") t.rout.push("nian");
    else t.rout.push(isRdp ? `'${cmd}' 不是内部或外部命令，也不是可运行的程序` : `${cmd.split(" ")[0]}: command not found`);
  }
  t.rbuf = ""; t.rpos = 0; t.startMenu = false;
}

/* ---------- SFTP ---------- */
const SUBFILES = [
  ["..", "", "--", "folder"], ["server.c", "Nov 17, 8:12 PM", "46 KB", "text"],
  ["vt.c", "Nov 16, 4:02 PM", "31 KB", "text"], ["share.c", "Nov 17, 9:41 PM", "18 KB", "text"],
  ["Makefile", "Nov 12, 10:08 AM", "3 KB", "text"], ["build", "Nov 17, 9:44 PM", "--", "folder"],
  ["harbor_core", "Nov 17, 9:44 PM", "2.1 MB", "archive"], ["tests.log", "Nov 17, 9:45 PM", "64 KB", "text"],
];

function ensureSftp() {
  if (S.sftp.files) return;
  S.sftp.files = { left: FILES.map((f) => f.slice()), right: FILES.map((f) => f.slice()) };
  S.sftp.sub = { left: SUBFILES.map((f) => f.slice()), right: SUBFILES.map((f) => f.slice()) };
  S.sftp.path = { left: ["home", "nian"], right: ["Users", "nian"] };
  S.sftp.fwdStack = { left: [], right: [] };
  S.sftp.filter = { left: "", right: "" };
  S.sftp.filterOpen = { left: false, right: false };
  S.sftp.showDot = { left: true, right: true };
  S.sftp.all = { left: false, right: false };
}

function sftpList(side) {
  ensureSftp();
  return S.sftp.path[side].length > 2 ? S.sftp.sub[side] : S.sftp.files[side];
}

function sftpFiles(side) {
  let list = sftpList(side);
  if (!S.sftp.showDot[side]) list = list.filter((f) => !f[0].startsWith("."));
  const q = S.sftp.filter[side].trim().toLowerCase();
  if (q) list = list.filter((f) => f[0].toLowerCase().includes(q));
  return list;
}

function parseSize(s) {
  const m = /([\d.]+)\s*(KB|MB|GB)/.exec(s || "");
  if (!m) return 0.01;
  const v = +m[1];
  return m[2] === "GB" ? v * 1024 : m[2] === "MB" ? v : v / 1024;
}

function xferBarHTML() {
  if (!S.transfers.length) return "";
  const act = S.transfers.filter((t) => t.state === "run");
  const done = S.transfers.filter((t) => t.state !== "run").slice(-3);
  return `<div class="xfer-head">${I("fwd", "sm")} 传输队列 · ${act.length} 进行中<span class="spacer"></span>
    ${done.length ? `<button class="btn sm ghost" onclick="A.xferClear()">清除已完成</button>` : ""}</div>` +
    [...act, ...done].map((t) => `<div class="xfer ${t.state}">
      <span class="x-ico">${t.state === "run" ? I("refresh", "sm") : t.state === "done" ? I("check", "sm") : I("x", "sm")}</span>
      <span class="x-name mono">${esc(t.name)}</span>
      <span class="x-dir">${t.from === "left" ? "→" : "←"} ${esc(t.toName)}</span>
      <span class="x-track"><span class="x-fill" style="width:${t.pct}%"></span></span>
      <span class="x-pct mono">${t.state === "run" ? `${t.pct}% · ${t.speed}` : t.state === "done" ? "完成" : "已取消"}</span>
      ${t.state === "run" ? `<button class="icon-btn" title="取消传输" onclick="A.xferCancel('${t.id}')">${I("x", "sm")}</button>` : ""}
    </div>`).join("");
}

function renderXfer() { document.querySelectorAll("#xferBar").forEach((b) => (b.innerHTML = xferBarHTML())); }

function pickTransferTarget(from, name, to) {
  const src = S.sftp[from];
  const opts = [["local", "Local · /Users/nian", osIco.local], ...S.hosts.filter((h) => h.type === "ssh" && h.id !== src).map((h) => [h.id, h.name, osIco[h.os]])];
  modal(`<div class="modal"><div class="modal-head"><h3>${I("plug")} 传输 ${esc(name)} 到…</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body"><div class="select-host-list">
      ${opts.map(([id, lbl, ico]) => `<div class="select-host-item" onclick="A.xferTarget('${from}','${esc(name)}','${to}','${id}')"><div class="os-ico ${id === "local" ? "local" : "ubuntu"}">${ico}</div><div><div class="h-name">${esc(lbl)}</div><div class="h-sub">${id === "local" ? "本机文件系统" : "SSH · SFTP 子系统"}</div></div></div>`).join("")}
    </div></div></div>`);
}

function startTransfer(from, name) {
  ensureSftp();
  const to = from === "left" ? "right" : "left";
  if (!S.sftp[to]) return pickTransferTarget(from, name, to);
  const entry = sftpList(from).find((f) => f[0] === name);
  if (!entry || entry[3] === "folder") return;
  if (S.transfers.some((t) => t.state === "run" && t.name === name && t.from === from)) return toastInfo(`${name} 已在传输中`);
  const sizeMB = parseSize(entry[2]);
  const t = { id: nid("x"), name, from, to, toName: S.sftp[to] === "local" ? "Local" : hostById(S.sftp[to]).name, pct: 0, state: "run", speed: "0 MB/s" };
  S.transfers.push(t);
  const dur = Math.max(1400, Math.min(9000, sizeMB * 22));
  const step = 100 / (dur / 120);
  t.timer = setInterval(() => {
    t.pct = Math.min(100, Math.round(t.pct + step * (0.7 + Math.random() * 0.6)));
    t.speed = ((sizeMB / (dur / 1000)) * (0.8 + Math.random() * 0.4)).toFixed(1) + " MB/s";
    if (t.pct >= 100) {
      clearInterval(t.timer);
      t.state = "done";
      const target = S.sftp.files[to];
      if (!target.some((f) => f[0] === name)) target.push([name, "Nov 17, " + now(), entry[2], entry[3]]);
      S.logs.unshift({ date: "2026-11-17 " + now(), user: "nian", host: t.toName, proto: "SFTP", result: "成功", dur: Math.round(dur / 1000) + "s", saved: false });
      renderXfer(); render();
      notify({ kind: "ok", title: "传输完成", body: `${name} → ${t.toName} · 写临时名后 rename，防半文件`, tabId: "sftp" });
      if (S.activeTab === "sftp") showToast(S.notifications[0]);
    } else renderXfer();
  }, 120);
  renderXfer();
}

function sftpView() {
  ensureSftp();
  const pane = (side) => S.sftp.pending && S.sftp.pending.side === side ? sftpConnectPanel(S.sftp.pending)
    : S.sftp[side] ? sftpBrowser(S.sftp[side], side) : sftpSelectHost(side);
  return `<div class="sftp-col"><div class="sftp-view">
    <div class="sftp-pane">${pane("left")}</div>
    <div class="sftp-pane">${pane("right")}</div>
  </div><div id="xferBar">${xferBarHTML()}</div></div>`;
}

function sftpConnectPanel(p) {
  const isLocal = p.host === "local";
  const h = isLocal ? null : hostById(p.host);
  const lines = p.lines || [];
  return `<div class="connect-wrap">
    <div class="connect-head"><div class="os-ico ${isLocal ? "local" : h.os}">${isLocal ? osIco.local : I("server")}</div>
      <div><div style="font-weight:700">${isLocal ? "Local" : esc(h.name)}</div>
      <div style="color:var(--text-3);font-size:12px">SFTP · ${isLocal ? "本机 loopback" : `SSH ${esc(h.name)}:${h.port}`}</div></div>
      <div style="margin-left:auto"><button class="btn sm" onclick="A.sftpHideLog()">${S.sftp.showLog ? "Hide logs" : "Show logs"}</button></div></div>
    ${S.sftp.showLog ? `<div class="connect-log">${lines.map((l) => `<div>${I("user", "sm")} ${esc(l)}</div>`).join("") || `<div>${I("info", "sm")} 正在建立 SFTP 会话…</div>`}</div>` : ""}
    <div style="max-width:430px;margin:18px auto;display:flex;gap:9px">
      <button class="btn" onclick="A.sftpCancelPending()">Close</button><div style="flex:1"></div>
      <button class="btn primary" onclick="A.sftpCancelPending();A.sftpPick('${p.side}','${p.host}')">Start over</button></div></div>`;
}

function sftpSelectHost(side) {
  return `<div class="sftp-head"><b style="font-size:13.5px">Select Host · ${side === "left" ? "左栏" : "右栏"}</b><span class="spacer"></span>
      <button class="btn primary sm" onclick="A.sftpPick('${side}','local')">${I("laptop", "sm")} Local</button></div>
    <div class="select-host-list">
      ${S.hosts.filter((h) => h.type === "ssh").map((h) => `<div class="select-host-item" onclick="A.sftpPick('${side}','${h.id}')">
        <div class="os-ico ${h.os}">${osIco[h.os]}</div><div><div class="h-name">${h.name}</div><div class="h-sub">ssh, ${h.user}</div></div></div>`).join("")}
    </div>
    <div class="sftp-empty" style="flex:0;padding:20px"><div class="e-sub">选择两端后可双栏互传（远端↔远端 / 远端↔本地）· 双击文件即开始传输</div></div>`;
}

function sftpBrowser(host, side) {
  ensureSftp();
  const isLocal = host === "local";
  const name = isLocal ? "Local · /Users/nian" : hostById(host).name;
  const list = sftpFiles(side);
  const path = S.sftp.path[side];
  return `<div class="sftp-head"><div class="os-ico ${isLocal ? "local" : "ubuntu"}" style="width:30px;height:30px;font-size:14px">${isLocal ? osIco.local : I("server")}</div>
      <b style="font-size:13.5px">${esc(name)}</b><span class="spacer"></span>
      <button class="btn ghost sm" onclick="A.sftpFilterToggle('${side}')">${I("search", "sm")} Filter</button>
      <button class="btn ghost sm" onclick="A.sftpActions(event,'${side}')">Actions ▾</button></div>
    ${S.sftp.filterOpen[side] ? `<div class="toolbar" style="padding:0 16px 8px"><div class="field" style="flex:1"><input type="text" name="field" data-filt="sftp-${side}" placeholder="过滤文件名…" value="${esc(S.sftp.filter[side])}" oninput="A.sftpFilter('${side}',this.value)"></div></div>` : ""}
    <div class="crumbs"><button class="navbtn" onclick="A.sftpUp('${side}')" title="上级目录">‹</button><button class="navbtn" onclick="A.sftpFwd('${side}')" title="前进" ${S.sftp.fwdStack[side].length ? "" : "disabled"}>›</button>
      ${path.map((p, i) => `<span class="crumb" onclick="A.sftpCrumb('${side}',${i})">${I("folder", "sm")} ${esc(p)}</span>`).join('<span style="color:var(--text-3)">›</span>')}</div>
    <div class="scroll" style="padding:0"><table class="filetable">
      <tr><th>Name</th><th>Date Modified</th><th>Size</th><th>Kind</th></tr>
      ${list.length === 0 ? `<tr><td colspan="4" style="text-align:center;color:var(--text-3);padding:22px">空目录 / 无匹配文件</td></tr>` : list.map(([n, d, s, k], i) => `<tr class="${S.sftp.selected === side + i || S.sftp.all[side] ? "selected" : ""}" onclick="A.sftpSelect('${side}${i}')" ondblclick="A.sftpDbl('${side}','${esc(n)}')">
        <td><span class="f-ico">${k === "folder" ? I("folder", "sm") : I("file", "sm")}</span>${esc(n)}</td><td style="color:var(--text-2)">${d}</td><td>${s}</td><td style="color:var(--text-2)">${k}</td></tr>`).join("")}
    </table></div>`;
}

/* ---------- Android renderers (FR-17) ---------- */
const bellAction = () => `<button class="icon-btn" data-bell title="通知中心">${I("bell", "md")}<span class="badge badge-bell" hidden>0</span></button>`;

function aBar(title, { back = false, dark = false, actions = "" } = {}) {
  return `<div class="a-bar ${dark ? "dark" : ""}">
    ${back ? `<button class="icon-btn" onclick="A.aBack()">${I("back", "md")}</button>` : ""}
    <h2>${title}</h2><span class="spacer"></span>${actions}</div>`;
}

function renderAndroid() {
  $("#pStatus").innerHTML = `<span>${now()}</span><span class="spacer"></span><span style="font-size:11px">5G</span>${I("wifi")}${I("battery")}`;
  const body = $("#phoneBody");
  const tab = tabById(S.activeTab) || S.tabs[0];
  S.activeTab = tab.id;
  if (tab.type === "vault") {
    const sec = S.vaultSection;
    if (sec === "hosts") body.innerHTML = aHosts();
    else if (sec === "sessions") body.innerHTML = aSessions();
    else if (sec === "keychain") body.innerHTML = aKeys();
    else if (sec === "account") body.innerHTML = aMe();
    else body.innerHTML = aSubPage(sec);
  } else if (tab.type === "sftp") {
    const pane = S.sftp.pending && S.sftp.pending.side === "left" ? sftpConnectPanel(S.sftp.pending)
      : S.sftp.left ? sftpBrowser(S.sftp.left, "left") : sftpSelectHost("left");
    body.innerHTML = aBar("SFTP", { back: true, actions: bellAction() }) +
      `<div class="a-scroll" style="padding:0;display:flex;flex-direction:column">${pane}<div id="xferBar">${xferBarHTML()}</div></div>`;
  } else if (tab.type === "connect") {
    body.innerHTML = aBar(esc(tab.host.name), { back: true, actions: bellAction() }) +
      `<div class="a-scroll" style="padding:0">${connectView(tab)}</div>`;
  } else if (tab.type === "term") {
    body.innerHTML = aTerm(tab); mountSession(tab.sess);
  } else if (tab.type === "workspace") {
    body.innerHTML = aWorkspace(tab); tab.panes.forEach(mountSession);
  } else if (tab.type === "remote") {
    body.innerHTML = aBar(esc(tab.host.name), { back: true, actions: bellAction() }) +
      `<div class="a-scroll" style="padding:0;background:#10141f">${remoteView(tab)}</div>`;
    scrollDw();
  }
  renderPhoneNav();
  updateBell();
  applyFind(false);
}

function renderPhoneNav() {
  const items = [["hosts", "server", "主机"], ["sessions", "terminal", "会话"], ["keychain", "key", "凭据"], ["account", "user", "我的"]];
  const tab = tabById(S.activeTab);
  let cur = tab && tab.type === "vault" ? S.vaultSection : null;
  if (tab && ["term", "workspace", "connect", "remote"].includes(tab.type)) cur = "sessions";
  $("#phoneNav").innerHTML = items.map(([id, ico, lbl]) =>
    `<button class="${cur === id ? "active" : ""}" onclick="A.section('${id}')">${I(ico)}<span>${lbl}</span></button>`).join("");
}

function aHosts() {
  const list = filteredHosts();
  return `${aBar("主机", { actions: `<button class="icon-btn" onclick="A.palette()" title="命令面板">${I("search", "md")}</button>` + bellAction() + `<button class="icon-btn" onclick="A.newHostModal()" title="新建主机">${I("plus", "md")}</button>` })}
    <div class="a-search">${I("search")}<input name="search" placeholder="搜索主机或 ssh user@host…" value="${esc(S.filters.hosts)}" oninput="A.filt('hosts',this.value)" onkeydown="A.quickConnect(event)"></div>
    <div class="a-scroll">
      <div class="a-sect">全部主机 · ${list.length}</div>
      ${list.length === 0 ? `<div class="empty" style="padding:30px 0"><div class="e-ico">${I("search", "xl")}</div><div class="e-title">没有匹配的主机</div>
        <div class="e-sub">换个关键词试试，或点右下角新建。</div>
        <button class="btn sm" style="margin-top:6px" onclick="A.filt('hosts','')">清除筛选</button></div>`
      : list.map((h) => `<div class="a-host" onclick="A.openHost('${h.id}')">
        <div class="os-ico ${h.os}">${osIco[h.os]}</div>
        <div style="min-width:0"><div class="h-name">${esc(h.name)}
          ${h.via ? '<span class="tag tun">隧道</span>' : ""}${h.type === "local" ? '<span class="tag">本地</span>' : ""}</div>
        <div class="h-sub">${esc(h.sub || `${h.type}, ${h.user}`)}${h.group ? ` · ${esc(h.group)}` : ""}</div></div>
        <button class="icon-btn" style="margin-left:auto;color:var(--text-3)" title="主机详情" onclick="event.stopPropagation();A.hostDrawer('${h.id}')">${I("info", "md")}</button></div>`).join("")}
    </div>
    <button class="a-fab" onclick="A.newHostModal()" title="新建主机">${I("plus")}</button>`;
}

function aSessions() {
  const list = sessionTabs();
  return `${aBar("会话", { actions: bellAction() })}
    <div class="a-scroll">
      <div class="a-newrow" style="margin-top:4px">
        <button class="btn" onclick="A.openLocal()">${I("terminal")} Local 终端</button>
        <button class="btn" onclick="A.openWorkspace()">${I("cols")} Workspace</button></div>
      <div class="a-sect">进行中的会话 · ${list.length}</div>
      ${list.length === 0 ? `<div class="empty" style="padding:34px 0"><div class="e-ico">${I("terminal", "xl")}</div>
        <div class="e-title">暂无会话</div><div class="e-sub">到「主机」页点一台服务器，或上方新建 Local 终端。</div></div>`
      : list.map((t) => {
        const s = t.sess || (t.panes && t.panes[0]);
        const dot = s && s.shared ? "share" : t.dot;
        const osk = t.type === "remote" ? t.host.os : t.local ? "local" : "ubuntu";
        return `<div class="a-host" onclick="A.switchTab('${t.id}')">
          <div class="os-ico ${osk}">${osIco[osk]}</div>
          <div><div class="h-name">${esc(t.title)} ${dot ? `<span class="t-dot ${dot}" style="position:static;display:inline-block"></span>` : ""}
            ${s && s.shared ? '<span class="tag share-tag">共享中</span>' : ""}</div>
          <div class="h-sub">${sessionState(t)}</div></div>
          <button class="icon-btn" style="margin-left:auto;color:var(--text-3)" onclick="event.stopPropagation();A.closeTab('${t.id}')" title="关闭会话">${I("x")}</button></div>`;
      }).join("")}
    </div>`;
}

function aKeys() {
  return `${aBar("凭据", { actions: bellAction() + `<button class="icon-btn" onclick="A.newKeyModal()" title="新建密钥">${I("plus", "md")}</button>` })}
    <div class="a-scroll">
      <div class="a-sect">SSH 密钥 · ${S.keys.length}</div>
      ${S.keys.map((k) => `<div class="a-card"><div class="a-row">
        <span class="os-ico generic" style="width:36px;height:36px">${I("key")}</span>
        <div><div class="t1">${esc(k.name)} ${k.bio ? '<span class="pill on"><span class="dot"></span>生物识别</span>' : ""}</div>
        <div class="t2 mono">${k.type} · ${k.fp}</div></div>
        <span class="spacer"></span><span class="t2">${k.used} 台主机</span></div></div>`).join("")}
      <div class="a-sect">认证方式</div>
      <div class="a-card click" onclick="A.toastInfo('BiometricPrompt：签名前需指纹/面容（FR-05）')"><div class="a-row">${I("fingerprint", "lg")}<span class="t1">生物识别解锁</span><span class="spacer"></span><span class="pill on"><span class="dot"></span>已启用</span></div></div>
      <div class="a-card click" onclick="A.toastInfo('FIDO2 安全密钥（P1，libfido2 over NFC/USB）')"><div class="a-row">${I("fido", "lg")}<span class="t1">FIDO2 安全密钥</span><span class="spacer"></span><span class="t2">P1</span></div></div>
      <div class="hintbox">私钥经 Android Keystore 硬件包装存储，仅密文经 E2EE 同步（FR-11），服务端零明文。</div>
    </div>`;
}

function aMe() {
  const links = [
    ["forwarding", I("fwd", "lg"), "端口转发", `${S.forwards.filter((f) => f.on).length} 条监听中`],
    ["snippets", BRACES, "Snippets", `${S.snippets.length} 条`],
    ["known", I("shield", "lg"), "Known Hosts", `${S.known.length} 台`],
    ["logs", I("clock", "lg"), "连接日志", `${S.logs.length} 条记录`],
    ["shared", I("broadcast", "lg"), "共享会话", "多端同屏 · E2EE"],
    ["settings", I("palette", "lg"), "外观与终端", "主题 · 字号 · 行距"],
  ];
  return `${aBar("我的", { actions: bellAction() })}
    <div class="a-scroll">
      <div class="a-card" style="margin-top:4px"><div class="a-row"><span class="avatar" style="width:38px;height:38px;font-size:15px">N</span>
        <div><div class="t1">nian@harbor.dev</div><div class="t2">E2EE 同步已开启 · TOTP 两步验证</div></div>
        <span class="spacer"></span><button class="btn sm" onclick="A.recoveryModal()">恢复码</button></div></div>
      ${links.map(([id, ico, lbl, sub]) => `<div class="a-card click" onclick="A.section('${id}')"><div class="a-row">${ico}
        <div><div class="t1">${lbl}</div><div class="t2">${sub}</div></div>
        <span class="spacer"></span><span class="chev" style="color:var(--text-3)">${I("chev")}</span></div></div>`).join("")}
      ${accountView()}
    </div>`;
}

function aSubPage(sec) {
  const titles = { forwarding: "端口转发", snippets: "Snippets", known: "Known Hosts", logs: "连接日志", shared: "共享会话", settings: "外观与终端" };
  const views = { forwarding: forwardingView, snippets: snippetsView, known: knownView, logs: logsView, shared: sharedView, settings: settingsView };
  return `${aBar(titles[sec] || sec, { back: true, actions: bellAction() })}
    <div class="a-scroll" style="padding:0;display:flex;flex-direction:column">${(views[sec] || (() => ""))()}</div>`;
}

function aKeysBar(sess) {
  return `<div class="a-keys" data-keybar="${sess.id}">
    <button class="vk" onclick="A.aKey('esc')">Esc</button>
    <button class="vk ${S.aCtrl ? "on" : ""}" onclick="A.aKey('ctrl')">Ctrl</button>
    <button class="vk" onclick="A.aKey('alt')">Alt</button>
    <button class="vk" onclick="A.aKey('tab')">Tab</button>
    <button class="vk" onclick="A.aKey('ctrlc')">^C</button>
    <button class="vk" onclick="A.aKey('up')">↑</button>
    <button class="vk" onclick="A.aKey('down')">↓</button>
    <button class="vk" onclick="A.aKey('left')">←</button>
    <button class="vk" onclick="A.aKey('right')">→</button>
    <span class="gap"></span>
    <button class="vk" onclick="A.aKey('kbd')" title="收起/唤起键盘">${I("keyboard")}</button>
  </div>`;
}

function refreshKeybar() {
  document.querySelectorAll(".a-keys").forEach((bar) => {
    const c = bar.querySelector(".vk:nth-child(2)");
    if (c) c.classList.toggle("on", S.aCtrl);
  });
}

function aTerm(tab) {
  const s = tab.sess;
  return `${aBar(s.local ? "Local · zsh" : `${esc(s.host.user)}@${esc(s.host.name)}`, {
    back: true, dark: true, actions: `
    <button class="icon-btn" onclick="A.watchModal('${s.id}','${tab.id}')" title="会话监控">${I("eye", "md")}</button>
    <button class="icon-btn" onclick="A.openShareModal('${tab.id}')" title="共享 Console">${I("broadcast", "md")}</button>
    ${bellAction()}
    <button class="icon-btn" onclick="A.termMore(event,'${tab.id}')" title="更多">${I("more", "md")}</button>` })}
    <div class="term-view">${findBar(s)}${termBody(s)}${aKeysBar(s)}${termStatus(s)}</div>`;
}

function aWorkspace(tab) {
  return `${aBar("Workspace · 2 分屏", {
    back: true, dark: true, actions: `
    <button class="icon-btn" onclick="A.toggleBroadcast('${tab.id}')" title="Broadcast" style="${tab.broadcast ? "color:#6fe09c" : ""}">${I("broadcast", "md")}</button>
    ${bellAction()}` })}
    <div class="term-view">${findBar(tab.panes[tab.focused])}<div class="workspace">
      ${tab.panes.map((p, i) => `<div class="pane ${tab.focused === i ? "focused" : ""} ${tab.broadcast ? "broadcast" : ""}" onclick="A.focusPane('${tab.id}',${i})">
        <div class="pane-head"><span>${I("ubuntu", "sm")} ${esc(p.host.name)} · ${esc(p.host.user)}</span></div>
        ${termBody(p)}</div>`).join("")}
    </div>${aKeysBar(tab.panes[tab.focused])}${termStatus(tab.panes[tab.focused])}</div>`;
}

/* ---------- modals ---------- */
function modal(html) { const m = $("#modalRoot"); m.hidden = false; m.innerHTML = html; }
function closeModal() { const m = $("#modalRoot"); m.hidden = true; m.innerHTML = ""; m.classList.remove("drawer-mode", "pal-mode"); S.pal = null; }

function newHostModal() { hostForm(null); }

function hostForm(h) {
  const t = h ? h.type : "ssh";
  modal(`<div class="modal"><div class="modal-head"><h3>${h ? "编辑主机" : "New host"}</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body">
      <input type="hidden" id="nhId" value="${h ? h.id : ""}">
      <div class="field-row"><div class="field"><label>类型</label><select id="nhType" name="opt" onchange="A.nhTypeChange()">
        <option value="ssh" ${t === "ssh" ? "selected" : ""}>SSH</option><option value="rdp" ${t === "rdp" ? "selected" : ""}>RDP</option><option value="vnc" ${t === "vnc" ? "selected" : ""}>VNC</option>
        ${t === "local" ? '<option value="local" selected>Local</option>' : ""}</select></div>
        <div class="field"><label>别名</label><input type="text" id="nhAlias" name="field" placeholder="my-server" value="${h ? esc(h.name) : ""}"></div></div>
      <div class="field-row"><div class="field" style="flex:2"><label>地址</label><input type="text" id="nhAddr" name="field" placeholder="10.0.0.5" value="${h ? esc(h.addr || (h.type === "ssh" ? h.name : "")) : ""}"></div>
        <div class="field"><label>端口</label><input type="number" id="nhPort" name="num" value="${h ? h.port : 22}"></div></div>
      <div class="field-row"><div class="field"><label>用户名</label><input type="text" id="nhUser" name="field" placeholder="root" value="${h ? esc(h.user) : ""}"></div>
        <div class="field"><label>分组</label><select id="nhGroup" name="opt"><option value="">未分组</option>${S.groups.map((g) => `<option ${h && h.group === g ? "selected" : ""}>${g}</option>`).join("")}</select></div></div>
      <div class="field"><label>跳板机 (ProxyJump / 隧道)</label><select id="nhJump" name="opt"><option value="无">无</option>${S.hosts.filter((x) => x.type === "ssh" && (!h || x.id !== h.id)).map((x) => `<option value="${x.id}" ${h && h.via === x.id ? "selected" : ""}>${x.user}@${x.name}</option>`).join("")}</select></div>
      <div id="nhTunnel" style="display:${t === "rdp" || t === "vnc" ? "block" : "none"}"><div class="check-row"><div class="cr-label"><b>通过 SSH 隧道连接（回环穿透）</b>
        <span class="cr-sub">目标只监听 127.0.0.1 时，借上方 SSH 主机自动建立 Local 转发</span></div>
        <label class="switch"><input type="checkbox" name="opt" id="nhTunOn" checked><span class="sl"></span></label></div></div>
      <div class="hintbox">保存后凭据将经 E2EE 同步到你的所有设备（FR-11）。</div>
    </div>
    <div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.saveHost()">保存</button></div></div>`);
}

function watchModal(sid, tabId) {
  const sess = findSess(sid);
  modal(`<div class="modal"><div class="modal-head"><h3>会话监控 · ${esc(sess.local ? "Local" : sess.host.name)}</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body">
      <div class="check-row"><div class="cr-label"><b>启用完成监控</b><span class="cr-sub">命中完成信号 → Tab 角标 + 通知中心 + 系统通知</span></div>
        <label class="switch"><input type="checkbox" name="opt" ${sess.watch ? "checked" : ""} onchange="A.setWatch('${sid}',this.checked)"><span class="sl"></span></label></div>
      <div class="field"><label>完成信号（任一命中即提醒）</label>
        ${[["eof", "会话/进程结束（EOF）", "最强信号"], ["osc133", "OSC 133 shell 集成", "含退出码，可区分成败"], ["ai", "AI CLI 输出预设", "Claude Code / Codex / aider / 编译 / pytest"], ["idle", "空闲启发式（输出≥10s 后静默≥5s）", "兜底信号，置信度低，默认关"]].map(([k, t, sub]) =>
          `<div class="check-row" style="padding:7px 0;border-bottom:1px solid #f0f3f6"><div class="cr-label"><span>${t}</span><span class="cr-sub">${sub}</span></div>
           <label class="switch green"><input type="checkbox" name="opt" ${sess.watchCfg[k] ? "checked" : ""} onchange="A.setWatchCfg('${sid}','${k}',this.checked)"><span class="sl"></span></label></div>`).join("")}
      </div>
      <div class="field"><label>自定义规则（正则，命中后需光标处于输入态）</label>
        <textarea name="rule" rows="2" placeholder="例：Task completed|Build succeeded|✔ All tests passed"></textarea></div>
      <div class="check-row"><div class="cr-label"><span>聚焦本 Tab 时免打扰</span><span class="cr-sub">仅更新角标，不弹系统通知</span></div>
        <label class="switch green"><input type="checkbox" name="opt" checked><span class="sl"></span></label></div>
      <div class="hintbox">规则测试器：粘贴样例输出即可验证是否命中，防止误报（FR-13 验收 6）。通知正文可配置为「仅主机名」以防敏感输出外显。</div>
    </div>
    <div class="modal-foot"><button class="btn" onclick="A.testRule()">规则测试器</button><button class="btn primary" onclick="A.closeModal()">完成</button></div></div>`);
}

function recoveryModal() {
  modal(`<div class="modal"><div class="modal-head"><h3>恢复码</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body">
      <div class="hintbox">忘记密码时，恢复码是找回 Vault 的唯一途径（服务端零知识，无法帮你重置）。请离线抄写保存。</div>
      <div class="fingerprint-art" style="font-size:12.5px;line-height:2">harbor maple tunnel velvet cipher anchor<br>quartz relay socket beacon velvet harbor<br>falcon cipher meadow tunnel anchor quartz<br>socket beacon maple falcon meadow velvet</div>
    </div><div class="modal-foot"><button class="btn" onclick="A.toastInfo('已复制到剪贴板（演示）')">复制</button><button class="btn primary" onclick="A.closeModal()">我已保存</button></div></div>`);
}

/* ---------- toast info helper ---------- */
function toastInfo(msg) {
  showToast({ kind: "info", title: "Harbor", body: msg, tabId: null });
}

/* ---------- global actions ---------- */
const SUB_SECTIONS = ["forwarding", "snippets", "known", "logs", "shared", "settings"];
window.A = {
  switchTab, closeTab, closeModal, toastInfo, openHost, openLocal, openWorkspace, setMode,
  section(id) {
    if (isAndroid()) {
      const t = tabById(S.activeTab);
      if (SUB_SECTIONS.includes(id)) S.aPrev = t && t.type === "vault" ? S.vaultSection : "sessions";
    }
    S.activeTab = "vault";
    S.vaultSection = id;
    render();
  },
  aBack() {
    const t = tabById(S.activeTab);
    if (t && t.type === "vault") S.vaultSection = S.aPrev || "account";
    else { S.activeTab = "vault"; S.vaultSection = "sessions"; }
    render();
  },
  aKey(k) {
    const tab = tabById(S.activeTab);
    const sess = tab && (tab.sess || (tab.panes && tab.panes[tab.focused]));
    if (!sess) return;
    if (k === "ctrl") { S.aCtrl = !S.aCtrl; refreshKeybar(); return; }
    if (k === "ctrlc") { if (sess.busy) interruptSession(sess); else appendLine(sess, "^C", "dim"); return; }
    const wrap = sessEl(sess);
    const inp = wrap && wrap.querySelector(".term-input");
    if (k === "kbd") { if (inp) inp.focus(); return; }
    if (k === "esc") { toastInfo("Esc 已发送（演示）"); return; }
    if (k === "alt") { toastInfo("Alt 组合键：先按 Alt 再按字母（演示）"); return; }
    if (k === "tab") { if (inp) { inp.value += "  "; inp.focus(); } return; }
    if (k === "up" || k === "down") {
      if (!inp) return;
      if (k === "up" && sess.histIdx > 0) { sess.histIdx--; inp.value = sess.hist[sess.histIdx] || ""; }
      if (k === "down" && sess.histIdx < sess.hist.length) { sess.histIdx++; inp.value = sess.hist[sess.histIdx] || ""; }
      inp.focus(); return;
    }
    if (inp) inp.focus();
  },
  termMore(ev, tid) {
    ev.stopPropagation();
    const t = tabById(tid);
    const s = t.sess || (t.panes && t.panes[t.focused]);
    showMenu(ev.clientX, ev.clientY, [
      ["终端内搜索 ⌘F", () => A.openFind(tid)],
      ["会话监控设置", () => watchModal(s.id, tid)],
      ["共享 Console", () => openShareModal(tid)],
      ["外观与终端", () => A.section("settings")],
      ["—"],
      [s && s.ended ? "重新连接" : "断开会话", () => { if (s && s.ended) A.reconnect(tid); else closeTab(tid); }, false, !(s && s.ended)],
    ]);
  },
  quickConnect(ev) {
    if (ev.key !== "Enter") return;
    const v = ev.target.value.trim();
    if (!v) return;
    const m = v.match(/^(?:(\w+)@)?([\w.\-]+)(?::(\d+))?$/);
    const found = S.hosts.find((h) => h.name.includes(v));
    if (found) { ev.target.value = ""; return openHost(found.id); }
    if (m) {
      const h = { id: nid("h"), name: m[2], user: m[1] || "root", port: +(m[3] || 22), type: "ssh", os: "generic" };
      S.hosts.push(h); ev.target.value = ""; openHost(h.id);
    } else toastInfo("格式：user@hostname[:port]");
  },
  quickConnectBtn() { const i = $("#quickInput"); A.quickConnect({ key: "Enter", target: i }); },
  newHostModal, newHostMenu(ev) {
    ev.stopPropagation();
    showMenu(ev.clientX, ev.clientY, [
      ["从 ~/.ssh/config 导入", () => {
        [["web-01.prod", "deploy"], ["db-01.prod", "admin"]].forEach(([name, user]) => {
          if (!S.hosts.some((h) => h.name === name)) S.hosts.push({ id: nid("h"), name, user, port: 22, type: "ssh", os: "ubuntu", group: "prod" });
        });
        render();
        toastInfo("已导入 2 台主机（含 ProxyJump / IdentityFile 映射），E2EE 同步中");
      }],
      ["新建分组…", () => promptModal("新建分组", "分组名称", "", (v) => {
        v = v.trim();
        if (!v) return;
        if (!S.groups.includes(v)) S.groups.push(v);
        render();
        toastInfo(`分组「${v}」已创建，可在主机编辑中选用`);
      })],
      ["导出主机 JSON", () => { download("harbor-hosts.json", JSON.stringify(S.hosts, null, 2), "application/json"); toastInfo("主机清单已导出（不含私钥）"); }],
    ]);
  },
  nhTypeChange() {
    const t = $("#nhType").value;
    $("#nhTunnel").style.display = t === "rdp" || t === "vnc" ? "block" : "none";
    $("#nhPort").value = t === "rdp" ? 3389 : t === "vnc" ? 5901 : t === "local" ? 0 : 22;
    if (t === "rdp" || t === "vnc") $("#nhAddr").value = "127.0.0.1";
  },
  saveHost() {
    const t = $("#nhType").value;
    const id = $("#nhId").value;
    const alias = $("#nhAlias").value.trim();
    const addr = $("#nhAddr").value.trim() || (t === "ssh" ? "10.0.0.5" : "127.0.0.1");
    const port = +$("#nhPort").value || (t === "rdp" ? 3389 : t === "vnc" ? 5901 : 22);
    const user = $("#nhUser").value.trim() || (t === "vnc" ? "" : "root");
    const group = $("#nhGroup").value;
    const jump = $("#nhJump").value;
    const tun = (t === "rdp" || t === "vnc") && $("#nhTunOn").checked && jump !== "无";
    const name = alias || addr;
    const sub = t === "local" ? "本机 shell · zsh" : tun ? `${t} · 经由 ${hostById(jump).name} 隧道` : t !== "ssh" ? `${t}, ${user}` : undefined;
    if (id) {
      const h = hostById(id);
      if (!h) return closeModal();
      Object.assign(h, { name, user, port, type: t, group: group || undefined, via: tun ? jump : undefined, sub });
      h.addr = t === "ssh" || t === "local" ? undefined : addr;
      h.os = t === "ssh" ? (h.os === "ubuntu" ? "ubuntu" : "generic") : t;
      closeModal(); render();
      toastInfo(`主机已更新，变更 5s 内经 E2EE 同步到其它设备`);
    } else {
      const h = { id: nid("h"), name, user, port, type: t, os: t === "ssh" ? "generic" : t, group: group || undefined };
      if (t !== "ssh" && t !== "local") h.addr = addr;
      if (tun) h.via = jump;
      if (sub) h.sub = sub;
      S.hosts.push(h);
      closeModal(); render();
      toastInfo(`主机已保存并经 E2EE 同步到 ${Math.max(1, S.devices.length - 1)} 台其它设备`);
    }
  },
  newKeyModal() {
    modal(`<div class="modal"><div class="modal-head"><h3>New key</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body">
      <div class="field"><label>名称</label><input type="text" id="nkName" value="id_ed25519_new"></div>
      <div class="field-row"><div class="field"><label>算法</label><select id="nkType"><option>ED25519</option><option>ECDSA P-256</option><option>RSA 4096</option><option>ED25519-SK (FIDO2)</option></select></div>
      <div class="field"><label>Passphrase</label><input type="password" name="passphrase" placeholder="可选"></div></div>
      <div class="check-row"><div class="cr-label"><b>签名需生物识别</b><span class="cr-sub">macOS Touch ID / Android BiometricPrompt</span></div>
        <label class="switch"><input type="checkbox" name="opt" id="nkBio" checked><span class="sl"></span></label></div>
      </div><div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.saveKey()">生成</button></div></div>`);
  },
  saveKey() {
    S.keys.push({ id: nid("k"), name: $("#nkName").value || "key", type: $("#nkType").value, fp: "SHA256:" + Math.random().toString(36).slice(2, 8) + "…", bio: $("#nkBio").checked, used: 0 });
    closeModal(); render(); toastInfo("密钥已生成（私钥仅存本机安全存储，密文同步）");
  },
  newForwardModal() {
    modal(`<div class="modal"><div class="modal-head"><h3>New forwarding</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body">
      <div class="field"><label>类型</label><select name="opt"><option>Local (-L)</option><option>Remote (-R)</option><option>Dynamic SOCKS5 (-D)</option></select></div>
      <div class="field-row"><div class="field"><label>绑定地址</label><input type="text" name="field" value="127.0.0.1"></div>
      <div class="field"><label>端口</label><input type="number" name="num" value="15432"></div></div>
      <div class="field-row"><div class="field"><label>目标地址</label><input type="text" name="field" value="db.internal"></div>
      <div class="field"><label>目标端口</label><input type="number" name="num" value="5432"></div></div>
      <div class="field"><label>经由 SSH 主机</label><select name="opt">${S.hosts.filter((h) => h.type === "ssh").map((h) => `<option>${h.user}@${h.name}</option>`).join("")}</select></div>
      <div class="check-row"><div class="cr-label"><span>随主机连接自动启动</span></div><label class="switch green"><input type="checkbox" name="opt" checked><span class="sl"></span></label></div>
      </div><div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button>
      <button class="btn primary" onclick="A.saveForward()">保存并启动</button></div></div>`);
  },
  saveForward() {
    S.forwards.push({ id: nid("f"), kind: "Local", bind: "127.0.0.1:15432", dest: "db.internal:5432", via: "10.88.2.11", on: true, traffic: "0 KB" });
    closeModal(); render();
  },
  toggleForward(id, on) { const f = S.forwards.find((x) => x.id === id); f.on = on; render(); },
  newSnippetModal() {
    modal(`<div class="modal"><div class="modal-head"><h3>New snippet</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body"><div class="field"><label>标题</label><input type="text" id="nsTitle" placeholder="重启服务"></div>
      <div class="field"><label>命令（支持 {{变量}} 占位）</label><textarea rows="3" id="nsBody" placeholder="sudo systemctl restart {{service}}"></textarea></div>
      </div><div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.saveSnippet()">保存</button></div></div>`);
  },
  saveSnippet() {
    const idEl = $("#nsId");
    const title = $("#nsTitle").value || "未命名";
    const body = $("#nsBody").value || "echo hello";
    if (idEl && idEl.value) {
      const s = S.snippets.find((x) => x.id === idEl.value);
      if (s) { s.title = title; s.body = body; }
      closeModal(); render(); toastInfo("Snippet 已更新并同步");
    } else {
      S.snippets.push({ id: nid("s"), title, body, scope: "全局" });
      closeModal(); render(); toastInfo("Snippet 已保存，5s 内同步到其它设备");
    }
  },
  sendSnippet(id) {
    const s = S.snippets.find((x) => x.id === id);
    const tab = tabById(S.activeTab);
    const sess = tab && (tab.sess || (tab.panes && tab.panes[tab.focused]));
    if (!sess || sess.ended || sess.busy) return toastInfo("请先打开一个终端会话再发送 Snippet");
    let body = s.body;
    const vars = body.match(/\{\{(\w+)\}\}/g);
    if (vars) { const v = prompt(`变量 ${vars[0]} 的值：`, "/var/log/app.log"); if (v === null) return; body = body.replace(/\{\{\w+\}\}/g, v); }
    runCommand(sess, body);
  },
  /* share */
  openShareModal,
  toggleShare(sid, tabId, on) {
    const sess = findSess(sid);
    sess.shared = on;
    const tab = tabById(tabId);
    if (on) {
      sess.participants = [];
      later(sess, () => {
        if (!sess.shared) return;
        const peer = isAndroid()
          ? { id: nid("p"), ico: I("laptop"), name: "Nian-MacBook Pro (macOS)", perm: sess.shareMode }
          : { id: nid("p"), ico: I("phone"), name: "Pixel 8 (Android)", perm: sess.shareMode };
        sess.participants.push(peer);
        notify({ kind: "info", title: `${peer.name.split(" (")[0]} 加入共享`, body: `正在${sess.shareMode === "collab" ? "协作" : "观看"}你的会话（快照已追平）`, tabId });
        if (!$("#modalRoot").hidden && $("#modalRoot")._renderShare) $("#modalRoot")._renderShare();
        if (S.activeTab === tabId) render();
      }, 3500);
    } else sess.participants = [];
    render();
    $("#modalRoot")._renderShare && $("#modalRoot")._renderShare();
  },
  shareMode(sid, mode) { const s = findSess(sid); s.shareMode = mode; s.participants.forEach((p) => (p.perm = mode)); $("#modalRoot")._renderShare(); },
  stopShare(sid, tabId) { const s = findSess(sid); s.shared = false; s.participants = []; closeModal(); render(); toastInfo("共享已停止，所有观看端已断开"); },
  kick(sid, pid, tabId) { const s = findSess(sid); s.participants = s.participants.filter((p) => p.id !== pid); $("#modalRoot")._renderShare(); render(); },
  copyLink(btn) { btn.textContent = "已复制 ✓"; setTimeout(() => (btn.textContent = "复制"), 1500); },
  joinByCode() {
    modal(`<div class="modal"><div class="modal-head"><h3>加入共享会话</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body"><div class="field"><label>配对码</label><input type="text" name="field" placeholder="482 913" style="letter-spacing:4px;font-size:16px"></div>
      <div class="hintbox">配对码通过 PAKE 交换会话密钥，Relay 全程只见密文。新加入者先收全量快照，2s 内追平画面。</div></div>
      <div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.closeModal();A.toastInfo('已加入（演示）：快照追平，实时同屏')">加入</button></div></div>`);
  },
  /* watch */
  watchModal,
  setWatch(sid, on) { const s = findSess(sid); s.watch = on; render(); $("#modalRoot")._renderShare = null; A.watchModal(sid, s.tabId); },
  setWatchCfg(sid, k, on) { findSess(sid).watchCfg[k] = on; },
  testRule() { toastInfo("规则测试：样例「✔ Task completed」命中 ✓；流式 token 行不命中 ✓"); },
  /* workspace */
  toggleBroadcast(tid) { const t = tabById(tid); t.broadcast = !t.broadcast; render(); focusTerminal(); },
  toggleAssist(tid) { const t = tabById(tid); t.assist = !t.assist; render(); focusTerminal(); },
  focusPane(tid, i) { const t = tabById(tid); if (t.focused !== i) { t.focused = i; render(); } focusTerminal(); },
  termClick(ev, sid) {
    const s = findSess(sid);
    const tab = tabById(s.tabId);
    if (tab.type === "workspace") { const i = tab.panes.indexOf(s); if (tab.focused !== i) { tab.focused = i; render(); } }
    focusTerminal();
  },
  reconnect(tid) {
    const t = tabById(tid);
    const old = t.sess;
    t.sess = newSession(tid, { host: old.host, local: old.local });
    appendSeed(t.sess, [["[重新连接成功 · 指数退避 3 次后恢复]", "dim"], ["提示: 远端会话已丢失，建议在 tmux 内工作以便 attach 恢复", "yellow"], ...MOTD.map((x) => [x, ""])]);
    render(); focusTerminal();
  },
  /* notifications */
  readAll() { S.notifications.forEach((n) => (n.unread = false)); updateBell(); renderNotifPanel(); },
  clearNotifs() { S.notifications = []; updateBell(); renderNotifPanel(); },
  openNotif(id) {
    const n = S.notifications.find((x) => x.id === id);
    n.unread = false; updateBell();
    if (n.tabId && tabById(n.tabId)) switchTab(n.tabId); else renderNotifPanel();
  },
  /* account */
  toggleSync(k, on) { S.sync[k] = on; toastInfo(`同步范围已更新：${k} ${on ? "开启" : "关闭"}（增量推送 ≤5s）`); },
  revokeDevice(id) {
    const d = S.devices.find((x) => x.id === id);
    S.devices = S.devices.filter((x) => x.id !== id);
    render(); toastInfo(`已吊销 ${d.name}：60s 内强制登出并停止同步`);
  },
  recoveryModal,
  /* sftp */
  xferTarget(from, name, to, hostId) {
    S.sftp[to] = hostId;
    closeModal();
    render();
    startTransfer(from, name);
  },
  sftpPick(side, hostId) {
    ensureSftp();
    if (S.sftp.pending) S.sftp.pending.timers.forEach(clearTimeout);
    const isLocal = hostId === "local";
    const h = isLocal ? null : hostById(hostId);
    const p = { side, host: hostId, lines: [], timers: [] };
    S.sftp.pending = p;
    S.sftp.selected = null;
    render();
    const stages = isLocal
      ? ["Opening local filesystem (loopback)…", "SFTP ready · /Users/nian"]
      : [`Resolving ${h.name}…`, `Connecting to ${h.name}:${h.port}…`, "Host key verified (known_hosts ✓)", "Authenticating using publickey (id_ed25519)…", "Authentication succeeded", "Opening SFTP channel (subsystem)…"];
    stages.forEach((line, i) => p.timers.push(setTimeout(() => {
      p.lines.push(line);
      if (S.activeTab === "sftp") render();
      if (i === stages.length - 1) p.timers.push(setTimeout(() => {
        if (S.sftp.pending !== p) return;
        S.sftp.pending = null;
        S.sftp[side] = hostId;
        render();
      }, 420));
    }, 260 * (i + 1))));
  },
  sftpCancelPending() {
    if (S.sftp.pending) S.sftp.pending.timers.forEach(clearTimeout);
    S.sftp.pending = null;
    render();
  },
  sftpHideLog() { S.sftp.showLog = !S.sftp.showLog; render(); },
  sftpStartOver() { A.sftpCancelPending(); A.sftpPick("left", "h2"); },
  sftpSelect(key) {
    S.sftp.selected = S.sftp.selected === key ? null : key;
    S.sftp.all = { left: false, right: false };
    render();
  },
  sftpDbl(side, name) {
    ensureSftp();
    if (name === "..") return A.sftpUp(side);
    const e = sftpList(side).find((f) => f[0] === name);
    if (e && e[3] === "folder") {
      S.sftp.path[side].push(name);
      S.sftp.fwdStack[side] = [];
      S.sftp.selected = null;
      render();
    } else startTransfer(side, name);
  },
  sftpUp(side) {
    ensureSftp();
    if (S.sftp.path[side].length <= 1) return toastInfo("已到根目录");
    S.sftp.fwdStack[side].push(S.sftp.path[side].pop());
    S.sftp.selected = null;
    render();
  },
  sftpFwd(side) {
    ensureSftp();
    if (!S.sftp.fwdStack[side].length) return;
    S.sftp.path[side].push(S.sftp.fwdStack[side].pop());
    render();
  },
  sftpCrumb(side, i) {
    ensureSftp();
    S.sftp.path[side] = S.sftp.path[side].slice(0, i + 1);
    S.sftp.fwdStack[side] = [];
    S.sftp.selected = null;
    render();
  },
  sftpFilterToggle(side) {
    ensureSftp();
    S.sftp.filterOpen[side] = !S.sftp.filterOpen[side];
    render();
    const i = document.querySelector(`[data-filt="sftp-${side}"]`);
    if (i) i.focus();
  },
  sftpFilter(side, v) { S.sftp.filter[side] = v; renderKeepFocus(); },
  sftpActions(ev, side) {
    ev.stopPropagation();
    ensureSftp();
    const selKey = S.sftp.selected;
    const hasSel = selKey && selKey.startsWith(side);
    const idx = hasSel ? +selKey.slice(side.length) : -1;
    const entry = hasSel ? sftpFiles(side)[idx] : null;
    const isFolder = entry && entry[3] === "folder";
    showMenu(ev.clientX, ev.clientY, [
      ["Open", () => { if (isFolder) A.sftpDbl(side, entry[0]); else toastInfo(`${entry[0]} 已下载到临时目录并用系统应用打开，保存时提示回传`); }, !entry],
      ["Copy to target pane", () => startTransfer(side, entry[0]), !entry || isFolder],
      ["Rename…", () => A.sftpRename(side, idx), !entry],
      ["Delete…", () => A.sftpDelete(side, idx), !entry, true],
      ["—"],
      ["Refresh", () => { render(); toastInfo("目录已刷新"); }],
      ["New Folder…", () => A.sftpNewFolder(side)],
      [S.sftp.showDot[side] ? "Hide Hidden Files" : "Show Hidden Files", () => { S.sftp.showDot[side] = !S.sftp.showDot[side]; render(); }],
      ["Edit Permissions…", () => A.sftpChmod(side, idx), !entry],
      ["Select All", () => { S.sftp.all[side] = true; render(); }],
      ["—"],
      ["Close Pane", () => { S.sftp[side] = null; S.sftp.selected = null; render(); }, false, true],
    ]);
  },
  sftpRename(side, idx) {
    const entry = sftpFiles(side)[idx];
    if (!entry) return;
    promptModal("重命名", "新文件名", entry[0], (v) => {
      if (!v.trim()) return;
      entry[0] = v.trim();
      S.sftp.selected = null;
      render();
      toastInfo("已重命名（远端 POSIX rename，原子操作）");
    });
  },
  sftpDelete(side, idx) {
    const entry = sftpFiles(side)[idx];
    if (!entry) return;
    confirmModal("删除文件", `确定删除 <b>${esc(entry[0])}</b>？删除后 9 秒内可撤销。`, "删除", () => {
      const list = sftpList(side);
      const i = list.indexOf(entry);
      list.splice(i, 1);
      S.sftp.selected = null;
      render();
      undoToast(`已删除 ${entry[0]}`, () => { list.splice(Math.min(i, list.length), 0, entry); render(); });
    });
  },
  sftpNewFolder(side) {
    promptModal("新建文件夹", "名称", "new-folder", (v) => {
      sftpList(side).push([v.trim() || "new-folder", "Nov 17, " + now(), "--", "folder"]);
      render();
      toastInfo("文件夹已创建");
    });
  },
  sftpChmod(side, idx) {
    const entry = sftpFiles(side)[idx];
    if (!entry) return;
    promptModal("修改权限", "chmod 数值", entry[3] === "folder" ? "755" : "644", (v) => {
      toastInfo(`chmod ${esc(v.trim() || "644")} ${esc(entry[0])} 已应用`);
    });
  },
  xferCancel(id) {
    const t = S.transfers.find((x) => x.id === id);
    if (!t || t.state !== "run") return;
    clearInterval(t.timer);
    t.state = "cancel";
    renderXfer();
  },
  xferClear() { S.transfers = S.transfers.filter((t) => t.state === "run"); renderXfer(); },
};

/* ---------- dropdown menu ---------- */
function showMenu(x, y, items) {
  closeMenu();
  const root = isAndroid() ? $("#phone") : $("#window");
  const rect = root.getBoundingClientRect();
  const m = document.createElement("div");
  m.className = "menu"; m.id = "ctxMenu";
  m.style.left = Math.max(8, Math.min(x - rect.left, rect.width - 230)) + "px";
  m.style.top = Math.max(8, Math.min(y - rect.top, rect.height - items.length * 36 - 20)) + "px";
  items.forEach((it) => {
    if (it[0] === "—") { const s = document.createElement("div"); s.className = "sep"; m.appendChild(s); return; }
    const b = document.createElement("button");
    b.textContent = it[0];
    if (it[2]) b.disabled = true;
    if (it[3]) b.className = "red";
    b.onclick = () => { closeMenu(); it[1] && it[1](); };
    m.appendChild(b);
  });
  root.appendChild(m);
  setTimeout(() => document.addEventListener("click", closeMenu, { once: true }), 0);
}
function closeMenu() { const m = $("#ctxMenu"); if (m) m.remove(); }

/* ---------- static wiring ---------- */
document.addEventListener("click", (e) => {
  const b = e.target.closest("[data-bell]");
  if (!b) return;
  e.stopPropagation();
  const p = $("#notifPanel");
  p.hidden = !p.hidden;
  if (!p.hidden) renderNotifPanel();
});
$("#sidebarToggle").innerHTML = I("panel", "md");
$("#bellBtn").insertAdjacentHTML("afterbegin", I("bell", "md"));
$("#sidebarToggle").addEventListener("click", () => {
  const t = tabById(S.activeTab);
  if (t && t.type === "vault") { S.sidebarHidden = !S.sidebarHidden; render(); }
});
$("#tabAdd").addEventListener("click", (e) => {
  e.stopPropagation();
  showMenu(e.clientX, e.clientY, [
    ["Local 终端", openLocal],
    ["Workspace（2 分屏）", openWorkspace],
    ["连接 10.88.2.11 (ssh)", () => openSSH(hostById("h2"))],
    ["home-win (RDP 隧道)", () => openRemote(hostById("h5"))],
    ["Serial（P2）", () => toastInfo("串口终端为 P2 功能"), true],
  ]);
});
document.querySelectorAll("#modeSwitch button").forEach((b) => b.addEventListener("click", () => setMode(b.dataset.mode)));
$("#modalRoot").addEventListener("click", (e) => { if (e.target === $("#modalRoot")) closeModal(); });
document.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") { e.preventDefault(); if (S.pal) closeModal(); else palette(); return; }
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "f") {
    const t = tabById(S.activeTab);
    if (t && (t.type === "term" || t.type === "workspace")) { e.preventDefault(); A.openFind(t.id); }
    return;
  }
  if ((e.metaKey || e.ctrlKey) && e.key === "t" && !isAndroid()) { e.preventDefault(); openLocal(); }
  if (e.key === "Escape") { if (S.find) { A.closeFind(); return; } closeModal(); closeNotifPanel(); closeMenu(); }
});

/* ---------- appearance & terminal settings (FR-21) ---------- */
const THEMES = [["harbor", "Harbor Dark"], ["ubuntu", "Ubuntu"], ["solarized", "Solarized Dark"], ["light", "Paper Light"]];
const THEME_META = {
  harbor: { bg: "#0a0f1e", fg: "#3fd97f", dim: "#8b96ad" },
  ubuntu: { bg: "#300a24", fg: "#8ae234", dim: "#c0a3b8" },
  solarized: { bg: "#002b36", fg: "#859900", dim: "#657b83" },
  light: { bg: "#f7f9fb", fg: "#1a7f4b", dim: "#8a93a3" },
};
const swatch = (id) => THEME_META[id].fg;

function applyPrefs() {
  document.documentElement.style.setProperty("--term-fs", S.prefs.fs + "px");
  document.documentElement.style.setProperty("--term-lh", String(S.prefs.lh));
  document.body.dataset.termTheme = S.prefs.theme;
}

function settingsView() {
  const p = S.prefs;
  return `<div class="scroll" style="padding-top:14px">
    <div class="set-grid">
      <div class="panel"><h4>${I("palette")} 终端主题</h4><div class="p-sub">实时应用到所有会话与分屏，经 E2EE 同步到其它设备。</div>
        <div class="theme-grid">${THEMES.map(([id, lbl]) => { const m = THEME_META[id]; return `
          <button class="theme-card ${p.theme === id ? "on" : ""}" onclick="A.setPref('theme','${id}')">
            <span class="theme-prev" style="background:${m.bg}"><span style="color:${m.fg}">nian@host:~$ ai-task</span><span style="color:${m.dim}">✔ Task completed (6.1s)</span></span>
            <span class="tc-name">${lbl} ${p.theme === id ? I("check", "sm") : ""}</span></button>`; }).join("")}
        </div>
        <div class="slider-row"><label>字号 <b id="fsVal">${p.fs} px</b></label>
          <input type="range" name="fs" min="10" max="20" step="1" value="${p.fs}" oninput="A.setPref('fs',+this.value)"></div>
        <div class="slider-row"><label>行距 <b id="lhVal">${(+p.lh).toFixed(1)}</b></label>
          <input type="range" name="lh" min="1.2" max="2" step="0.1" value="${p.lh}" oninput="A.setPref('lh',+this.value)"></div>
        <div class="check-row" style="margin-top:16px"><div class="cr-label"><span>Autocomplete <span class="beta" style="border-color:var(--line);color:var(--text-3)">BETA</span></span><span class="cr-sub">基于 Shell History 的本地补全，历史不出设备</span></div>
          <label class="switch green"><input type="checkbox" name="opt" ${p.autocomplete ? "checked" : ""} onchange="A.setPref('autocomplete',this.checked)"><span class="sl"></span></label></div>
        <div style="margin-top:16px"><button class="btn sm" onclick="A.resetPrefs()">${I("refresh", "sm")} 恢复默认</button></div>
      </div>
      <div class="panel"><h4>${I("terminal")} 实时预览</h4><div class="p-sub">拖动左侧滑杆 / 切换主题，立即生效。</div>
        <div class="term-view pref-preview">
          <div class="term-toolbar"><span class="tt-title">${I("ubuntu")} ssh nian@10.88.2.11</span><div class="spacer"></div><button class="tbtn on">${I("eye")} 监控 开</button></div>
          <div class="term-scroll">
            <div class="term-line dim">Welcome to Ubuntu 24.04.4 LTS</div>
            <div class="term-line white">nian@10.88.2.11:~$ ai-task</div>
            <div class="term-line dim">⠸ Applying edits to src/server.c …</div>
            <div class="term-line">✔ Task completed — 3 files changed, tests green.</div>
            <div class="term-inputline"><span class="term-prompt">nian@10.88.2.11:~$ </span><span class="rcur" style="color:var(--t-green,#3fd97f)">█</span></div>
          </div>
          <div class="term-status"><span class="ok">● 已连接</span><span>xterm-256color</span><span style="margin-left:auto">scrollback 10,000 行</span></div>
        </div>
        <div class="hintbox" style="margin-top:12px">外观设置属于同步范围「外观与设置」（FR-11）；字号 / 行距 / 主题在 macOS · Linux · Android 三端一致。</div>
      </div>
    </div></div>`;
}

/* ---------- focus-preserving render & undo ---------- */
function renderKeepFocus() {
  const a = document.activeElement;
  let key = null, sel = null;
  if (a && (a.tagName === "INPUT" || a.tagName === "TEXTAREA" || a.tagName === "SELECT")) {
    key = a.id ? "#" + a.id : a.dataset.filt ? `[data-filt="${a.dataset.filt}"]` : a.placeholder ? `[placeholder="${CSS.escape(a.placeholder)}"]` : null;
    if (typeof a.selectionStart === "number") sel = [a.selectionStart, a.selectionEnd];
  }
  render();
  if (key) {
    const el = document.querySelector(key);
    if (el) { el.focus(); if (sel && el.setSelectionRange) { try { el.setSelectionRange(sel[0], sel[1]); } catch (e) {} } }
  }
}

function undoToast(body, fn) {
  showToast({ kind: "info", title: "Harbor", body, tabId: null, action: { label: "撤销", fn } });
}

function confirmModal(title, body, okLabel, fn) {
  A._confirmFn = fn;
  modal(`<div class="modal" style="width:min(420px,92%)"><div class="modal-head"><h3>${title}</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body"><div style="font-size:13px;line-height:1.7;color:var(--text-2)">${body}</div></div>
    <div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn danger" onclick="A.confirmOk()">${okLabel}</button></div></div>`);
}

function promptModal(title, label, value, fn) {
  A._promptFn = fn;
  modal(`<div class="modal" style="width:min(420px,92%)"><div class="modal-head"><h3>${title}</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body"><div class="field"><label>${label}</label><input type="text" id="pmVal" name="field" value="${esc(value)}" onkeydown="if(event.key==='Enter')A.promptOk()"></div></div>
    <div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.promptOk()">确定</button></div></div>`);
  const i = $("#pmVal");
  if (i) { i.focus(); i.select(); }
}

function download(name, text, mime = "text/plain") {
  const blob = new Blob([text], { type: mime + ";charset=utf-8" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
}

function copyText(text, btn) {
  const done = () => {
    if (btn) { const old = btn.textContent; btn.textContent = "已复制 ✓"; setTimeout(() => (btn.textContent = old), 1400); }
    else toastInfo("已复制到剪贴板");
  };
  const fallback = () => {
    const ta = document.createElement("textarea");
    ta.value = text; ta.style.position = "fixed"; ta.style.opacity = "0";
    document.body.appendChild(ta); ta.select();
    try { document.execCommand("copy"); done(); } catch (e) { toastInfo("复制失败，请手动选择复制"); }
    ta.remove();
  };
  if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, fallback);
  else fallback();
}

/* ---------- host detail drawer (FR-18) ---------- */
function hostDrawer(id) {
  const h = hostById(id);
  if (!h) return;
  const last = S.logs.find((l) => l.host === h.name);
  const via = h.via ? hostById(h.via) : null;
  const rows = [
    ["协议", h.type.toUpperCase()],
    ["地址", h.addr || h.name],
    ["端口", h.port || "—"],
    ["用户名", h.user || "—"],
    ["认证", h.type === "ssh" ? "密钥 id_ed25519 · Touch ID" : h.type === "local" ? "本机用户" : "NLA / 密码"],
    ["分组", h.group || "未分组"],
    ["跳板 / 隧道", via ? `${via.user}@${via.name} → 127.0.0.1:${h.port}` : "直连"],
    ["最近连接", last ? `${last.date} · ${last.result}` : "尚未连接"],
  ];
  $("#modalRoot").classList.add("drawer-mode");
  modal(`<div class="modal drawer">
    <div class="modal-head"><h3 style="display:flex;align-items:center;gap:10px"><span class="os-ico ${h.os}" style="width:32px;height:32px;border-radius:9px">${osIco[h.os]}</span>${esc(h.name)}</h3>
      <button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
    <div class="modal-body">
      <div style="display:flex;gap:6px;flex-wrap:wrap">${h.via ? '<span class="tag tun">SSH 隧道</span>' : ""}${h.group ? `<span class="tag">${esc(h.group)}</span>` : ""}${h.type === "local" ? '<span class="tag">本地</span>' : ""}<span class="tag share-tag">E2EE 已同步</span></div>
      <div class="drawer-rows">${rows.map(([k, v]) => `<div class="dr-row"><span class="dr-k">${k}</span><span class="dr-v mono">${esc(String(v))}</span></div>`).join("")}</div>
      ${h.type === "ssh" ? `<div class="hintbox">主机指纹 ${fakeFP(h.name)} · 详见 Known Hosts；凭据变更 5s 内同步到其它设备。</div>` : ""}
    </div>
    <div class="modal-foot drawer-foot">
      <button class="btn primary" onclick="A.closeModal();A.openHost('${h.id}')">${I("plug", "sm")} 连接</button>
      <button class="btn" onclick="A.editHostModal('${h.id}')">编辑</button>
      <button class="btn" onclick="A.duplicateHost('${h.id}')">复制</button>
      <span style="flex:1"></span>
      <button class="btn danger" title="删除主机" onclick="A.deleteHost('${h.id}')">${I("trash", "sm")}</button>
    </div></div>`);
}

/* ---------- command palette (FR-20) ---------- */
function palItems() {
  const q = (S.pal ? S.pal.q : "").trim().toLowerCase();
  const items = [];
  S.hosts.forEach((h) => items.push({ ico: osIco[h.os], title: h.name, sub: `连接 · ${h.type.toUpperCase()}${h.user ? " " + h.user : ""}`, kw: `${h.name} ${h.user} ${h.group || ""}`, fn: () => openHost(h.id) }));
  SECTIONS.forEach(([id, ico, lbl]) => items.push({ ico, title: lbl, sub: "Vault 页面", kw: lbl + " " + id, fn: () => A.section(id) }));
  S.snippets.forEach((s) => items.push({ ico: BRACES, title: s.title, sub: "发送 Snippet", kw: s.title + " " + s.body, fn: () => A.sendSnippet(s.id) }));
  [["Local 终端", I("terminal"), () => openLocal()], ["Workspace 分屏", I("cols"), () => openWorkspace()], ["导出连接日志 CSV", I("clock"), () => A.exportLogs()], ["外观与终端", I("palette"), () => A.section("settings")]]
    .forEach(([title, ico, fn]) => items.push({ ico, title, sub: "命令", kw: title, fn }));
  return items.filter((it) => !q || (it.title + " " + it.kw).toLowerCase().includes(q)).slice(0, 9);
}

function palette() {
  S.pal = { q: "", idx: 0 };
  const m = $("#modalRoot");
  m.hidden = false;
  m.classList.add("pal-mode");
  m.innerHTML = `<div class="pal-box"><div class="pal-input">${I("search")}<input data-filt="pal" name="field" placeholder="搜索主机、页面、Snippet 或命令…" oninput="A.palInput(this.value)" onkeydown="A.palKey(event)"><span class="kbd">esc</span></div>
    <div class="pal-list" id="palList"></div></div>`;
  renderPalList();
  m.querySelector("input").focus();
}

function renderPalList() {
  const items = palItems();
  if (S.pal.idx >= items.length) S.pal.idx = Math.max(0, items.length - 1);
  const el = $("#palList");
  if (!el) return;
  el.innerHTML = items.length
    ? items.map((it, i) => `<div class="pal-item ${i === S.pal.idx ? "on" : ""}" onclick="A.palRun(${i})" onmousemove="A.palHover(${i})"><span class="pi-ico">${it.ico}</span><span class="pi-title">${esc(it.title)}</span><span class="pi-sub">${esc(it.sub)}</span>${i === S.pal.idx ? '<span class="kbd">⏎</span>' : ""}</div>`).join("")
    : `<div class="pal-empty">无匹配结果 — 试试主机名或「日志」</div>`;
}

/* ---------- terminal find (FR-20) ---------- */
function applyFind(scroll) {
  if (!S.find) return;
  const wrap = document.querySelector(`[data-sess="${S.find.sid}"]`);
  if (!wrap) return;
  const q = S.find.q.trim().toLowerCase();
  const lines = [...wrap.querySelectorAll(".term-line")];
  lines.forEach((l) => l.classList.remove("hit", "hit-cur"));
  const hits = q ? lines.filter((l) => l.textContent.toLowerCase().includes(q)) : [];
  S.find.n = hits.length;
  if (S.find.idx >= hits.length) S.find.idx = 0;
  hits.forEach((l, i) => { l.classList.add("hit"); if (i === S.find.idx) l.classList.add("hit-cur"); });
  const c = $("#findCount");
  if (c) c.textContent = hits.length ? `${S.find.idx + 1}/${hits.length}` : "0/0";
  if (scroll && hits.length) hits[S.find.idx].scrollIntoView({ block: "center" });
}

/* ---------- extended actions ---------- */
Object.assign(A, {
  /* filters / sort / view */
  filt(k, v) { S.filters[k] = v; renderKeepFocus(); },
  hostSortSet(v) { S.hostSort = v; renderKeepFocus(); },
  hostViewSet(v) { S.hostView = v; render(); },
  hostTagSet(v) { S.hostTag = v; renderKeepFocus(); },
  logSort() { S.logDesc = !S.logDesc; renderKeepFocus(); },
  logSaved(i) { S.logs[i].saved = !S.logs[i].saved; render(); },
  /* hosts CRUD */
  hostDrawer,
  editHostModal(id) { const h = hostById(id); if (h) hostForm(h); },
  duplicateHost(id) {
    const h = hostById(id);
    if (!h) return;
    S.hosts.push({ ...h, id: nid("h"), name: h.name + " (副本)" });
    closeModal(); render();
    toastInfo(`已复制为「${h.name} (副本)」，可继续编辑`);
  },
  deleteHost(id) {
    const h = hostById(id);
    if (!h) return;
    confirmModal("删除主机", `确定删除 <b>${esc(h.name)}</b>？其凭据将从所有设备移除。删除后 9 秒内可撤销。`, "删除", () => {
      const idx = S.hosts.indexOf(h);
      S.hosts.splice(idx, 1);
      S.tabs = S.tabs.filter((t) => !(t.host && t.host.id === id) && !(t.sess && t.sess.host && t.sess.host.id === id) && !(t.panes && t.panes.some((p) => p.host && p.host.id === id)));
      if (!tabById(S.activeTab)) S.activeTab = "vault";
      closeModal(); render();
      undoToast(`已删除主机 ${h.name}`, () => { S.hosts.splice(Math.min(idx, S.hosts.length), 0, h); render(); });
    });
  },
  confirmOk() { const fn = A._confirmFn; A._confirmFn = null; closeModal(); if (fn) fn(); },
  promptOk() { const fn = A._promptFn; A._promptFn = null; const v = $("#pmVal") ? $("#pmVal").value : ""; closeModal(); if (fn) fn(v); },
  copyText,
  trustHost(id) {
    const h = hostById(id);
    if (!h) return;
    S.known.push({ id: nid("n"), host: h.name, kt: "ssh-ed25519", fp: fakeFP(h.name), added: "2026-11-17" });
    closeModal();
    startSSH(h, true);
    toastInfo("指纹已写入 known_hosts 并经 E2EE 同步");
  },
  /* keychain / forwarding / snippets / known CRUD */
  keyMenu(ev) {
    ev.stopPropagation();
    showMenu(ev.clientX, ev.clientY, [
      ["生成新密钥", () => A.newKeyModal()],
      ["导入私钥…", () => promptModal("导入私钥", "粘贴 PEM / OpenSSH 私钥", "", (v) => {
        if (!v.trim()) return;
        S.keys.push({ id: nid("k"), name: "imported_" + (S.keys.length + 1), type: "ED25519", fp: "SHA256:" + Math.random().toString(36).slice(2, 8) + "…", bio: false, used: 0 });
        render();
        toastInfo("私钥已导入本机安全存储（仅密文同步，服务端零明文）");
      })],
      ["导出全部公钥", () => { download("harbor_pubkeys.txt", S.keys.map((k) => `# ${k.name}\nssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI${k.id} harbor`).join("\n")); toastInfo("公钥已导出（私钥永不导出）"); }],
    ]);
  },
  copyPub(id) { const k = S.keys.find((x) => x.id === id); if (k) copyText(`ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI${k.id} harbor:${k.name}`); },
  delKey(id) {
    const k = S.keys.find((x) => x.id === id);
    if (!k) return;
    confirmModal("删除密钥", `确定删除 <b>${esc(k.name)}</b>？${k.used ? `仍有 ${k.used} 台主机引用此密钥，删除后将改用密码认证。` : ""}删除后 9 秒内可撤销。`, "删除", () => {
      const idx = S.keys.indexOf(k);
      S.keys.splice(idx, 1);
      render();
      undoToast(`已删除密钥 ${k.name}`, () => { S.keys.splice(Math.min(idx, S.keys.length), 0, k); render(); });
    });
  },
  fwdMenu(ev) {
    ev.stopPropagation();
    showMenu(ev.clientX, ev.clientY, [
      ["新建规则", () => A.newForwardModal()],
      ["全部启动", () => { S.forwards.forEach((f) => (f.on = true)); render(); toastInfo("全部转发规则已启动"); }],
      ["全部停止", () => { S.forwards.forEach((f) => (f.on = false)); render(); toastInfo("全部转发规则已停止"); }],
      ["导出规则 JSON", () => { download("harbor-forwards.json", JSON.stringify(S.forwards, null, 2), "application/json"); toastInfo("转发规则已导出"); }],
    ]);
  },
  delForward(id) {
    const f = S.forwards.find((x) => x.id === id);
    if (!f) return;
    confirmModal("删除转发规则", `确定删除 <b>${esc(f.bind)} → ${esc(f.dest)}</b>？监听将立即停止。删除后 9 秒内可撤销。`, "删除", () => {
      const idx = S.forwards.indexOf(f);
      S.forwards.splice(idx, 1);
      render();
      undoToast(`已删除规则 ${f.bind}`, () => { S.forwards.splice(Math.min(idx, S.forwards.length), 0, f); render(); });
    });
  },
  snipMenu(ev) {
    ev.stopPropagation();
    showMenu(ev.clientX, ev.clientY, [
      ["新建 Snippet", () => A.newSnippetModal()],
      ["导入 JSON…", () => promptModal("导入 Snippets", "粘贴 JSON 数组 [{title, body}]", "", (v) => {
        try {
          const arr = JSON.parse(v);
          let n = 0;
          (Array.isArray(arr) ? arr : [arr]).forEach((s) => { if (s && s.title && s.body) { S.snippets.push({ id: nid("s"), title: String(s.title), body: String(s.body), scope: "导入" }); n++; } });
          render();
          toastInfo(n ? `已导入 ${n} 条 Snippet` : "未识别到有效条目");
        } catch (e) { toastInfo("JSON 解析失败，请检查格式"); }
      })],
      ["导出 JSON", () => { download("harbor-snippets.json", JSON.stringify(S.snippets.map(({ title, body, scope }) => ({ title, body, scope })), null, 2), "application/json"); toastInfo("Snippets 已导出"); }],
    ]);
  },
  editSnippet(id) {
    const s = S.snippets.find((x) => x.id === id);
    if (!s) return;
    modal(`<div class="modal"><div class="modal-head"><h3>编辑 Snippet</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body"><input type="hidden" id="nsId" value="${s.id}">
      <div class="field"><label>标题</label><input type="text" id="nsTitle" name="field" value="${esc(s.title)}"></div>
      <div class="field"><label>命令（支持 {{变量}} 占位）</label><textarea rows="3" id="nsBody" name="field">${esc(s.body)}</textarea></div>
      </div><div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button><button class="btn primary" onclick="A.saveSnippet()">保存</button></div></div>`);
  },
  delSnippet(id) {
    const s = S.snippets.find((x) => x.id === id);
    if (!s) return;
    confirmModal("删除 Snippet", `确定删除 <b>${esc(s.title)}</b>？删除后 9 秒内可撤销。`, "删除", () => {
      const idx = S.snippets.indexOf(s);
      S.snippets.splice(idx, 1);
      render();
      undoToast(`已删除 Snippet「${s.title}」`, () => { S.snippets.splice(Math.min(idx, S.snippets.length), 0, s); render(); });
    });
  },
  importKnown() {
    promptModal("导入 known_hosts", "每行：host keytype base64…", "", (v) => {
      let n = 0;
      v.split("\n").forEach((line) => {
        const parts = line.trim().split(/\s+/);
        if (parts.length >= 3 && !S.known.some((k) => k.host === parts[0])) {
          S.known.push({ id: nid("n"), host: parts[0], kt: parts[1], fp: fakeFP(parts[0]), added: "2026-11-17" });
          n++;
        }
      });
      render();
      toastInfo(n ? `已导入 ${n} 条 known_hosts 记录` : "未识别到新条目（已存在的会跳过）");
    });
  },
  delKnown(id) {
    const k = S.known.find((x) => x.id === id);
    if (!k) return;
    confirmModal("删除 Known Host", `确定删除 <b>${esc(k.host)}</b> 的指纹记录？下次连接将重新走 TOFU 指纹确认。`, "删除", () => {
      const idx = S.known.indexOf(k);
      S.known.splice(idx, 1);
      render();
      undoToast(`已删除 ${k.host} 的指纹记录`, () => { S.known.splice(Math.min(idx, S.known.length), 0, k); render(); });
    });
  },
  exportLogs() {
    const csv = ["date,user,host,proto,result,duration,saved", ...S.logs.map((l) => [l.date, l.user, l.host, l.proto, l.result, l.dur, l.saved ? "yes" : "no"].map((x) => `"${String(x).replace(/"/g, '""')}"`).join(","))].join("\n");
    download("harbor-logs.csv", csv, "text/csv");
    toastInfo(`已导出 ${S.logs.length} 条连接日志（CSV）`);
  },
  /* connect view */
  connHideLog(tid) { const t = tabById(tid); if (t) { t.hideLog = !t.hideLog; render(); } },
  connRestart(tid) {
    const t = tabById(tid);
    if (!t || !t.host) return;
    t.type = "connect";
    t.sess = null;
    render();
    runStages(t, t.host, false);
  },
  /* workspace */
  toggleFocus(tid) { const t = tabById(tid); t.focusMode = !t.focusMode; render(); focusTerminal(); },
  assistTab(tid, at) { const t = tabById(tid); t.assistTab = at; render(); focusTerminal(); },
  reconnectPane(tid, i) {
    const t = tabById(tid);
    const old = t.panes[i];
    const np = newSession(tid, { host: old.host, local: false });
    appendSeed(np, [["[重新连接成功 · 指数退避 3 次后恢复]", "dim"], ...MOTD.map((x) => [x, ""])]);
    t.panes[i] = np;
    render(); focusTerminal();
    toastInfo(`${old.host.name} 分屏已重连`);
  },
  /* prefs */
  setPref(k, v) {
    S.prefs[k] = v;
    applyPrefs();
    if (k === "fs") { const e = $("#fsVal"); if (e) e.textContent = v + " px"; return; }
    if (k === "lh") { const e = $("#lhVal"); if (e) e.textContent = (+v).toFixed(1); return; }
    renderKeepFocus();
  },
  resetPrefs() {
    S.prefs = { theme: "harbor", fs: 13, lh: 1.5, autocomplete: false };
    applyPrefs(); render();
    toastInfo("外观已恢复默认");
  },
  /* find */
  openFind(tid) {
    const t = tabById(tid);
    const sess = t && (t.sess || (t.panes && t.panes[t.focused]));
    if (!sess) return;
    S.find = { sid: sess.id, tid, q: "", idx: 0, n: 0 };
    render();
    const i = document.querySelector('[data-filt="find"]');
    if (i) i.focus();
  },
  findInput(v) { if (!S.find) return; S.find.q = v; S.find.idx = 0; applyFind(true); },
  findNext(d) { if (!S.find || !S.find.n) return; S.find.idx = (S.find.idx + d + S.find.n) % S.find.n; applyFind(true); },
  findKey(e) {
    if (e.key === "Enter") { e.preventDefault(); A.findNext(e.shiftKey ? -1 : 1); }
    if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); A.closeFind(); }
  },
  closeFind() { S.find = null; render(); focusTerminal(); },
  /* palette */
  palette,
  palInput(v) { if (!S.pal) return; S.pal.q = v; S.pal.idx = 0; renderPalList(); },
  palHover(i) { if (S.pal && S.pal.idx !== i) { S.pal.idx = i; renderPalList(); } },
  palKey(e) {
    if (!S.pal) return;
    const n = palItems().length;
    if (e.key === "ArrowDown") { e.preventDefault(); S.pal.idx = n ? (S.pal.idx + 1) % n : 0; renderPalList(); }
    else if (e.key === "ArrowUp") { e.preventDefault(); S.pal.idx = n ? (S.pal.idx - 1 + n) % n : 0; renderPalList(); }
    else if (e.key === "Enter") { e.preventDefault(); A.palRun(S.pal.idx); }
    else if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); closeModal(); }
  },
  palRun(i) { const it = palItems()[i]; closeModal(); if (it) it.fn(); },
  /* remote desktop */
  rdpQuality(tid, v) {
    const t = tabById(tid);
    if (!t) return;
    t.quality = v;
    render();
    toastInfo(`画质：${v} · ${v === "流畅" ? "30 fps · 低带宽优先" : v === "高清" ? "60 fps · 无损优先" : "自适应 30–60 fps"}`);
  },
  remoteKeyBtn(tid, k) {
    const t = tabById(tid);
    if (!t || !t.rout) return;
    if (k === "win") t.startMenu = !t.startMenu;
    else if (k === "esc") { t.startMenu = false; t.rmod = null; }
    else if (k === "ctrl" || k === "alt") t.rmod = t.rmod === k ? null : k;
    else if (k === "tab") { t.rbuf = t.rbuf.endsWith("Get-") ? t.rbuf.slice(0, -4) + "Get-Service" : t.rbuf + "  "; t.rpos = t.rbuf.length; }
    else if (k === "back") { if (t.rpos > 0) { t.rbuf = t.rbuf.slice(0, t.rpos - 1) + t.rbuf.slice(t.rpos); t.rpos--; } }
    else if (k === "left") t.rpos = Math.max(0, t.rpos - 1);
    else if (k === "right") t.rpos = Math.min(t.rbuf.length, t.rpos + 1);
    else if (k === "up" || k === "down") {
      if (t.rhist.length) {
        t.rhi = k === "up" ? Math.max(0, t.rhi - 1) : Math.min(t.rhist.length, t.rhi + 1);
        t.rbuf = t.rhist[t.rhi] || "";
        t.rpos = t.rbuf.length;
      }
    } else if (k === "enter") remoteExec(t);
    render(); scrollDw();
  },
  remoteKey(e, tid) {
    const t = tabById(tid);
    if (!t || !t.rout) return;
    const map = { Enter: "enter", Backspace: "back", Tab: "tab", Escape: "esc", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
    if (map[e.key]) { e.preventDefault(); A.remoteKeyBtn(tid, map[e.key]); return; }
    if (e.key.length === 1 && !e.metaKey && !e.ctrlKey && !e.altKey) {
      e.preventDefault();
      if (t.rmod === "ctrl" && e.key.toLowerCase() === "c") { t.rout.push("^C"); t.rmod = null; t.rbuf = ""; t.rpos = 0; }
      else { t.rbuf = t.rbuf.slice(0, t.rpos) + e.key + t.rbuf.slice(t.rpos); t.rpos++; }
      render(); scrollDw();
    }
  },
  /* share peer / account */
  joinPeer() {
    const host = hostById("h2");
    const tab = { id: nid("t"), type: "term", title: "Pixel 8 · 共享", icon: I("phone"), peer: true };
    const sess = newSession(tab.id, { host, local: false });
    sess.readonly = true;
    tab.sess = sess;
    appendSeed(sess, [["[已加入 Pixel 8 的共享会话 · 只读 · 快照 2s 内追平]", "cyan"], ...MOTD.map((t) => [t, ""])]);
    S.tabs.push(tab);
    switchTab(tab.id);
    [["nian@10.88.2.11:~$ docker ps", "white", 1400], ["CONTAINER ID   IMAGE        STATUS", "dim", 2100], ["a1b2c3d4e5f6   nginx:1.27   Up 3 hours", "", 2400], ["nian@10.88.2.11:~$ tail -n 2 /var/log/app.log", "white", 3600], ["[INFO] request handled in 12ms", "dim", 4300], ["[INFO] worker pool healthy", "dim", 4600]]
      .forEach(([txt, cls, ms]) => later(sess, () => appendLine(sess, txt, cls), ms));
  },
  logoutModal() {
    modal(`<div class="modal" style="width:min(440px,92%)"><div class="modal-head"><h3>退出登录</h3><button class="modal-x" onclick="A.closeModal()">${I("x", "sm")}</button></div>
      <div class="modal-body"><div class="hintbox">退出后停止同步。云端密文保留；本地 Vault 可选择保留或擦除（擦除后需重新登录解密拉取）。</div></div>
      <div class="modal-foot"><button class="btn" onclick="A.closeModal()">取消</button>
        <button class="btn" onclick="A.doLogout(false)">保留本地退出</button>
        <button class="btn danger" onclick="A.doLogout(true)">擦除并退出</button></div></div>`);
  },
  doLogout(wipe) {
    S.loggedOut = true;
    if (wipe) {
      S._cloud = { hosts: S.hosts, keys: S.keys, forwards: S.forwards, snippets: S.snippets, known: S.known };
      S.hosts = S.hosts.filter((h) => h.type === "local");
      S.keys = []; S.forwards = []; S.snippets = []; S.known = [];
      S.tabs = S.tabs.filter((t) => t.type === "vault" || t.type === "sftp");
      S.sftp.left = null; S.sftp.right = null; S.sftp.pending = null;
    }
    closeModal();
    S.activeTab = "vault";
    S.vaultSection = "account";
    render();
    toastInfo(wipe ? "已退出：本地 Vault 已擦除，云端密文保留" : "已退出：本地 Vault 保留，同步已暂停");
  },
  login() {
    S.loggedOut = false;
    if (S._cloud) { Object.assign(S, S._cloud); S._cloud = null; }
    render();
    toastInfo("登录成功：密文已拉取并在本地解密，E2EE 同步恢复");
  },
});

/* ---------- boot ---------- */
applyPrefs();
render();
/* 预置一条历史通知，演示通知中心 */
notify({ kind: "ok", title: "会话任务完成 · 10.88.2.11", body: "ai-task · AI CLI 完成预设（OSC 133 退出码 0）· 耗时 42s — 可以进行下一步了", tabId: null });
S.notifications[0].unread = false; updateBell();