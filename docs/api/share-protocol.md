# Harbor Share Protocol v1（FR-12 共享 Console）

所有者客户端把一个终端会话的输出经 E2EE 推给 Relay，参与者订阅；协作参与者的输入经
Relay 回到所有者注入 PTY。**Relay 只做路由，看不到任何终端内容或输入**。编码约定沿用
`sync-protocol.md` §2（base64url 无填充、XChaCha20-Poly1305-IETF、`seal(k,pt,ad)` =
`nonce24 || ct||tag`）。

## 1. 角色与密钥

| 名称 | 说明 |
|---|---|
| owner | 发起共享的设备（已登录） |
| member | 同账号的其它已登录设备 |
| guest | 跨账号/未登录，通过链接 + 6 位配对码加入；**永远只读** |
| `K_out` | 所有者为本次共享生成的 32 字节随机密钥，加密所有下行帧（广播） |
| `K_in[p]` | 所有者为每个**协作** member 单独生成的 32 字节输入密钥；只有该参与者与所有者知道 |
| `link_secret` | 16 字节随机数，只出现在共享链接的 fragment 中（不发给服务端） |
| `code` | 6 位十进制配对码（所有者显示，`randombytes_uniform(1000000)`，左补 0） |

只读参与者只拿到 `K_out`，因此**无法伪造输入**：所有者只接受用对应 `K_in[p]` 解得开的
INPUT 帧。Relay 盖在帧头的 `from` 只用于路由与展示，授权以密钥为准。

## 2. 共享登记（HTTP，Bearer token，同 sync 的基址 `/v1`）

- `POST /shares` `{share_id, meta, guest_ttl}` → `{}`。`share_id` 由客户端生成（uuid v4），
  重复返回 409 `share_exists`。`guest_ttl` 为链接 + 配对码的有效期（秒，默认 1800，上限 86400）；
  共享本身持续到所有者停止或断线。
  `meta = seal(VK, json, "hb:share:meta:v1" || 0x00 || share_id)`。meta 明文：`{"title":"…","host":"…","mode":"view"|"collab","k_out":"<b64>","owner_device":"…","started":ms}`。
- `PUT /shares/{id}` `{meta}`：仅所有者设备；更新 meta（例如改模式）。
- `POST /shares/{id}/guest` `{guest_ttl}`：仅所有者设备；所有者重新生成配对码/链接后调用，
  重置 guest 有效期并清零 PAKE 失败计数（解锁）。
- `GET /shares` → `{shares:[{share_id, owner_device, meta, created, guest_expires}]}`：本账号进行中的共享。
- `DELETE /shares/{id}`：仅所有者设备。结束共享并断开全部参与者。
- 账号 WebSocket（`/v1/ws`）新增推送 `{"t":"shares"}`：本账号共享列表变化（新建/结束），
  member 收到后刷新列表（FR-12 验收 1：5 s 内出现）。
- 服务端：所有者设备的 share socket 断开即视为共享结束（标记结束 + 通知 + 断开参与者）。
  `guest_ttl` 到期后拒绝新的 guest 连接（已加入的 guest 不受影响）。

member 用 VK 解开 meta 即得 `K_out`；服务端无 VK，看不到标题/主机/密钥。

## 3. 共享链接

```
harbor://share/<share_id>?s=<server_url_b64url>#<link_secret_b64url>
```

fragment 不出客户端。guest 需要链接与配对码二者才能完成 PAKE。

## 4. Relay WebSocket

`GET /v1/share/{share_id}/ws?role=owner|member|guest`

- owner、member：`Authorization: Bearer`。owner 必须是登记该共享的设备；member 必须同账号。
- guest：无认证。
- 并发参与者上限 5（owner 不计）；超出以 4008 关闭。
- Relay 每 5 s ping，10 s 无 pong 断开（所有者断线 ≤ 10 s 内被发现，FR-12 验收 5）。

### 4.1 帧格式（WebSocket binary message）

```
type(1) | to(4, BE) | from(4, BE) | seq(8, BE) | payload
```

- 参与者 id 由 Relay 分配：owner 恒为 1，参与者从 2 递增；`to = 0` 表示广播给全部参与者。
  参与者必须先收到 Relay 的 `hello`（得知自己的 id）才能发送任何二进制帧——帧头 `from` 在 AD 中，
  发送方须按 Relay 改写后的值计算。
- 参与者发出的帧 Relay 一律改写 `from` 为该连接的 id，`to` 强制为 1（只能发给所有者）。
  所有者可发广播（`to=0`）或定向（`to=p`）。
- `seq`：每个发送方各自严格递增（含发给不同接收方的帧，因此接收方看到的 seq 可以有间隔）；
  接收方拒绝不大于上一帧的 seq（防重放）。丢帧检测不用它，用 DELTA 自带的 `dseq`（§5）。
- Relay 文本消息（JSON，Relay 自己生成）：
  `{"t":"joined","id":p,"role":"member"|"guest","device":"…"}`、
  `{"t":"left","id":p}`（发给 owner）、`{"t":"hello","id":p}`（发给新连接自己）、
  `{"t":"ended","reason":"stopped"|"owner_gone"|"expired"|"kicked"}`（发给参与者，随后关闭）、
  `{"t":"locked"}`（guest PAKE 已被锁定）。
- owner 可发文本控制 `{"t":"kick","id":p}`、`{"t":"pake_fail","id":p}`、`{"t":"pake_ok","id":p}`；
  Relay 执行踢出/计数。Relay 只把 PAKE 帧（0x20–0x22）转发给尚未 `pake_ok` 的 guest 连接对应的
  所有者方向，且在 `pake_ok` 之前不向该 guest 转发 0x01–0x05 帧。

### 4.2 加密

除 PAKE 帧外，payload = `seal(key, pt, ad)`，
`ad = "hb:share:v1" || 0x00 || share_id || 0x00 || type(1) || to(4) || from(4) || seq(8)`
（与帧头字节一致）。解密失败的帧丢弃并计数。

| type | 名称 | 方向 | key | pt |
|---|---|---|---|---|
| 0x01 | SNAPSHOT | owner→all/p | `K_out` | 见 §5 |
| 0x02 | DELTA | owner→all | `K_out` | `dseq(8,BE) || 原始 PTY 输出字节` |
| 0x03 | RESIZE | owner→all | `K_out` | `rows(2,BE) cols(2,BE)` |
| 0x04 | PRESENCE | owner→all | `K_out` | JSON `{"participants":[{"id":p,"name":"…","perm":"view"|"collab"}],"mode":"view"|"collab"}` |
| 0x05 | NOTICE | owner→p | `K_out` | UTF-8 文本（如"只读模式，输入未生效"） |
| 0x10 | INPUT | p→owner | `K_in[p]` | 原始字节（参与者本地终端按自己镜像的 VT 模式编码） |
| 0x11 | SNAP_REQ | p→owner | `K_out` | 空；参与者检测到 seq 断档时请求全量快照 |
| 0x12 | JOIN | member→owner | `K_out` | JSON `{"device":"…","name":"…"}` |
| 0x13 | GRANT | owner→p | `K_out`（AD 含 to=p） | JSON `{"perm":"view"|"collab","k_in":"<b64>"?}`；`k_in` 仅协作时出现，其值为 `seal(VK, K_in[p], "hb:share:kin:v1" || 0x00 || share_id || 0x00 || p(4,BE))` 的 base64url（guest 永远不会收到 collab GRANT） |
| 0x20 | PAKE1 | guest→owner | 明文 | §6 |
| 0x21 | PAKE2 | owner→guest | 明文 | §6 |
| 0x22 | PAKE3 | guest→owner | 明文 | §6 |
| 0x23 | KEY | owner→guest | `ISK` 派生的 `K_pake` | `K_out`（32 字节） |

GRANT 的 `k_in` 用 VK 再包一层，保证只有本账号设备能拿到输入密钥；同账号其它设备理论上
也能解开——它们同属一个用户，可接受。

## 5. 快照与增量

- SNAPSHOT pt = `rows(2) cols(2) alt(1) dseq(8) | ansi_stream`，`dseq` 为该快照已包含的最后一个
  DELTA 序号。所有者必须在输出流的同一点上同时截取快照与分配 dseq（与 PTY 输出 tap 串行），
  保证快照之后的第一个 DELTA 恰为 `dseq+1`。DELTA 的 `dseq` 从 1 起逐个加 1。`ansi_stream` 是一段 VT 序列，
  在一个全新的 `rows×cols` 终端上回放即可重建：先输出 scrollback 尾部（最多 1000 行，带 SGR），
  再 `ESC[H ESC[2J` 并逐行绘制可见屏（带 SGR、宽字符正确占位），恢复光标位置/可见性、
  `DECCKM`、bracketed paste 等影响输入编码的模式；`alt=1` 时先绘主屏再 `ESC[?1049h` 绘备用屏。
  不追求像素级还原（选择、闪烁计时不同步）。
- 所有者在以下时机发 SNAPSHOT：参与者加入（定向）、收到 SNAP_REQ（定向）、每 10 s 广播一次
  （仅在有参与者时）。DELTA 只在快照之后发送，参与者以快照为基准顺序回放 DELTA。
- 参与者只回放 `dseq = 上一个 + 1` 的 DELTA；`dseq` 不大于当前基准的 DELTA 直接丢弃（快照已覆盖）；
  出现断档（Relay 对慢消费者丢帧时会出现）→ 丢弃后续 DELTA 直到收到新的 SNAPSHOT，
  并发 SNAP_REQ（每 2 s 至多一次）。
- 视图尺寸以所有者为准（RESIZE + 快照头）；参与者 letterbox 适配，不向所有者回传尺寸。
- Relay 对每个参与者维持有界发送队列（≥ 1 MiB）；溢出时丢弃该参与者的 DELTA（保留 SNAPSHOT/控制帧），
  从而触发 seq 断档 → 快照修复。

## 6. guest 配对：CPace over ristretto255

记 `H = SHA-512`，`lv(x) = len(x)(1 字节) || x`。

1. guest：`nonce_g = random(16)`；`sid = share_id(ASCII) || nonce_g`；
   `PRS = code(6 个 ASCII 数字) || link_secret(16)`；
   `G = crypto_core_ristretto255_from_hash(H("hb:cpace:v1" || lv(PRS) || lv(sid)))`；
   `ya = crypto_core_ristretto255_scalar_random()`；`Ya = ya·G`
   （`crypto_scalarmult_ristretto255`）。发 PAKE1 = `nonce_g(16) || Ya(32)`。
2. owner：校验 `Ya` 为合法点（`crypto_core_ristretto255_is_valid_point`），同法算 `G`、
   `yb`、`Yb`；`K = yb·Ya`（返回 -1 即恒等元 → 失败）；
   `ISK = H("hb:cpace:isk" || lv(sid) || K || Ya || Yb)`；
   `tag_o = HMAC-SHA-256(ISK[0..32], "owner")`。发 PAKE2 = `Yb(32) || tag_o(32)`。
3. guest：校验 `Yb`，`K = ya·Yb`，同法算 `ISK`；`sodium_memcmp` 校验 `tag_o`，失败 → 中止
   （本地提示"配对码错误"）。发 PAKE3 = `tag_g = HMAC-SHA-256(ISK[0..32], "guest")`。
4. owner：校验 `tag_g`。**凡未以合法 PAKE3 完成的尝试都算失败**——`tag_g` 不符、发出 PAKE2 后
   30 s 内无 PAKE3、或 guest 在 PAKE3 前断开（猜测者可在收到 PAKE2 后离线核对 `tag_o` 再断开，
   必须计数）——所有者发 `{"t":"pake_fail","id":p}`（Relay 计数）并踢出该 guest。Relay 同样把
   "收到过 PAKE1 但所有者未确认成功即断开"的 guest 计为失败（所有者离线时的兜底）。
   所有者确认成功时发 `{"t":"pake_ok","id":p}`。成功 → `K_pake = crypto_kdf_derive_from_key(32, 1, "hbpake01", ISK[32..64])`，
   发 KEY（定向给该 guest）后再发 PRESENCE、定向 SNAPSHOT。
5. 任何一方的 `ya/yb/K/ISK` 用完即 `sodium_memzero`。

**限流**：Relay 对每个共享累计 PAKE 失败，≥ 5 次即锁定 guest 加入（新的 guest 连接收到
`{"t":"locked"}` 后关闭）；同一时刻只允许 1 个未完成的 guest PAKE。所有者可"重新生成配对码"
（换 `code` 与 `link_secret`，即新链接）并调用 `POST /shares/{id}/guest` 解锁。在线猜测 5 次的成功概率 ≤ 5×10⁻⁶；Relay 得到的 PAKE 转写不能用于
离线穷举（CPace 性质）。

## 7. 生命周期

- 所有者开启共享：生成 `K_out`、`code`、`link_secret` → 登记 → 连接 relay（role=owner）→ 有参与者时开始推流。
- member 加入：`GET /shares` 解 meta 得 `K_out` → 连接 relay → 发 JOIN → 所有者回 GRANT（按共享模式：
  view 或 collab+`k_in`）→ 定向 SNAPSHOT → 持续 DELTA。
- guest 加入：解析链接 → 连接 relay（role=guest）→ PAKE → KEY → SNAPSHOT/DELTA；guest 永远只读。
- 只读参与者输入：参与者本地直接提示"只读模式"，不发送；所有者收到无效 INPUT 时回 NOTICE。
- 所有者"停止共享"：`DELETE /shares/{id}` → Relay 向全部参与者发 `ended(stopped)` 并关闭（3 s 内，验收 5）。
- 所有者踢人：文本 `kick` → Relay 发 `ended(kicked)` 并关闭该连接；所有者丢弃其 `K_in`。
- 改模式（view↔collab）：所有者更新 meta、向各 member 重发 GRANT；降级为 view 时丢弃其 `K_in`
  （之后该参与者的 INPUT 解不开即被拒）。`K_out` 不更换。
- 安全边界：持有 `K_out` 的参与者能解密输出；若 Relay 恶意，它与某参与者合谋可向其他参与者注入
  伪造的输出帧（输出完整性只对 `K_out` 持有者成立）。输入方向不受影响——伪造 INPUT 需要 `K_in[p]`。
