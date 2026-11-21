# Harbor Sync Protocol v1（FR-11）

本文是 Harbor 账号、端到端加密（E2EE）同步的对外协议规范，客户端（core `sync` +
平台层 HTTP/WebSocket）与服务端（`server/`）都以此为准。所有设备必须逐字节一致地实现
本文的密码学构造，否则跨端互通失败。

## 1. 原则

- **零知识**：服务端只见密文、对象 id、对象类型、服务端序号、服务端时间戳。主机、密钥、
  密码、snippet、规则、设置等任何业务字段都不出设备明文边界。
- **口令永不上传**：服务端拿到的是从主密钥派生出的 `auth_key`，再做一次 Argon2id 存储；
  拖库者必须先破解用户口令的 Argon2id 才能得到任何东西。
- **服务端是无知的乐观并发存储**：冲突判定与合并全部在客户端完成（服务端看不到版本向量）。

## 2. 编码约定

- 二进制字段在 JSON 中一律为 **base64url、无填充**（libsodium
  `sodium_base64_VARIANT_URLSAFE_NO_PADDING`）。
- 时间戳为 Unix 毫秒（int64）。
- 邮箱规范化：去首尾空白 + ASCII 小写。
- 口令规范化：平台层在交给 core 之前做 Unicode **NFKC**，再取 UTF-8 字节。
- AEAD 一律为 **XChaCha20-Poly1305-IETF**（`crypto_aead_xchacha20poly1305_ietf_*`），
  nonce 24 字节随机（`randombytes_buf`）。
- 封装格式 `seal(key, pt, ad)` = `nonce(24) || ciphertext||tag`。

## 3. 密钥体系

| 名称 | 来源 | 用途 |
|---|---|---|
| `MK` 主密钥 | `crypto_pwhash(32, pw, salt16, ops, mem, ARGON2ID13)` | 只用来派生下面两把 |
| `auth_key` | `crypto_kdf_derive_from_key(32, 1, "hbauth01", MK)` | 登录凭据（上传给服务端） |
| `wrap_key` | `crypto_kdf_derive_from_key(32, 2, "hbwrap01", MK)` | 包装 VK |
| `VK` 账号数据密钥 | 注册时 `randombytes_buf(32)` | 加密所有同步对象（PRD 中的 DEK） |
| `E` 恢复熵 | 注册时 `randombytes_buf(32)`，编码为 24 词恢复码 | 恢复 |
| `rc_wrap` | `crypto_kdf_derive_from_key(32, 1, "hbrcwrap", E)` | 恢复码包装 VK |
| `rc_auth` | `crypto_kdf_derive_from_key(32, 2, "hbrcauth", E)` | 恢复流程身份凭据 |

- KDF 参数：客户端注册时选择，默认 `ops = crypto_pwhash_OPSLIMIT_MODERATE`（3）、
  `mem = crypto_pwhash_MEMLIMIT_MODERATE`（256 MiB）；随账号存储于服务端，prelogin 返回。
  客户端拒绝 `ops < 2` 或 `mem < 64 MiB` 的参数（防服务端降级）。
- `wrapped_vk_pw = seal(wrap_key, VK, "hb:vk:pw:v1")`
- `wrapped_vk_rc = seal(rc_wrap, VK, "hb:vk:rc:v1")`
- 本机设备上的本地 vault DEK（libsecret）与 VK 无关：本地 SQLite 的 secret 列仍用本机 DEK。
- VK、登录 token 在本机存 libsecret（无 keyring 时与本地 DEK 同目录、0600 文件），
  重启无需再输口令（Linux 无生物识别，"绑定本机"即此）。所有明文密钥用 sodium 锁定内存、
  用完即擦。

### 3.1 恢复码（24 词）

BIP-39 英文词表（2048 词）编码 256 位熵：`E || SHA256(E)[0]`（264 位）每 11 位一词，
大端。解码校验和不符即拒绝。测试向量：32 字节全 0 → `abandon`×23 + `art`。

## 4. 同步对象

### 4.1 服务端记录

`{id, type, seq, blob, updated_at}`：`id` 为客户端 uuid；`type` 为对象类型（明文元数据，
PRD 允许）；`seq` 为服务端按账号单调递增的序号；`blob` 为密文；`updated_at` 为服务端时间。
服务端每个 `(account, id)` 只保留最新一条。

### 4.2 密文 blob

```
blob = 0x01 || seal(VK, sodium_pad(pt, 256), ad)
ad   = "hb:obj:v1" || 0x00 || id || 0x00 || type
pt   = UTF-8 JSON:
{
  "type": "host",
  "data": { ...业务字段, "_fv": {...} },   // 去掉设备本地字段（§4.4）
  "vv": { "<device_id>": <counter>, ... },
  "tomb": false,
  "updated_at": 1700000000000,
  "secret": { "password": "...", ... }    // 无 secret 时省略
}
```

- `ad` 绑定 id 与 type：服务端无法把一个对象的密文挪给另一个 id 或类型。解密失败的对象
  客户端跳过并计数上报（不得用部分数据），游标照常前进。
- `sodium_pad` 按 256 字节块填充以弱化长度泄露；解密后 `sodium_unpad`。
- `secret` 为本机 secret 列解密后的明文映射，只在锁定内存中组装，封装后立即擦除。

### 4.3 字段版本

本地 vault 每次 `put` 为变化的字段打 `_fv[field] = [device_id, counter]`（counter 为
本设备单调递增计数器），对象 `vv[device_id] = counter`。伪字段：

- `"$secret"`：secret 映射整体；
- `"$tomb"`：删除/恢复（tombstone）状态。删除与撤销删除必须打 `$tomb` 戳。

被删除的字段在 `data` 中缺失但 `_fv` 中有戳（表示"删除"这一写入）。

### 4.4 设备本地字段（不同步）

封装时剥离、合并时保留本地值：

| type | 字段 |
|---|---|
| host | `identity_file`、`last_connected` |
| forward | `enabled` |
| settings | `font_size`、`line_height`、`font_family` |

### 4.5 同步范围

本机设置（不同步，存 vault meta）：`hosts`（host+group）、`keys`（key）、`snippets`、
`forwards`（forward）、`known`、`settings`；`history` 暂不支持（UI 置灰）。范围外的本地
对象不上传；范围外的远端对象不落库（游标照常前进）；重新开启某类型时把游标重置为 0 全量
重拉。

## 5. 合并算法（客户端）

输入同 id 的本地 `L` 与远端 `R`（已解密）。`seen(X, s)` 定义为 `s` 为空或
`X.vv[s.dev] >= s.ctr`。

对字段全集 `F = keys(L.data) ∪ keys(R.data) ∪ keys(L._fv) ∪ keys(R._fv) ∪ {"$secret","$tomb"}`
（去掉 `_` 开头与设备本地字段），逐字段：

1. `sL == sR`（同为空或 `[dev,ctr]` 相同）→ 取 L。
2. `seen(L, sR) && !seen(R, sL)` → 取 L。
3. `seen(R, sL) && !seen(L, sR)` → 取 R。
4. 两者互见（不应出现）→ 取 `(ctr, dev)` 字典序较大的戳对应一方。
5. 互不可见 → **并发冲突**：临时胜者为对象 `updated_at` 较大者，相等取 `device_id`
   字符串较大者；该字段计入冲突。

结果：`vv = max(L.vv, R.vv)`（逐元素）、`_fv` 取各字段胜方的戳、`updated_at = max`、
设备本地字段保留 L。合并不产生新戳（不 bump 计数器）。

- 存在冲突字段时，在本地 `sync_conflicts` 表记录 `{id, type, fields, loser}`，`loser`
  为输方完整版本（data 用 JSON；secret 用本机 DEK 加密，同 secret 列）。冲突只出现在执行
  合并的那台设备上。
- 用户选择"保留另一版本"时以普通 `put` 写入该值（产生新戳，支配双方）；选择"保留当前"
  只删除冲突记录。
- 合并结果与 R 等价（data、secret、tomb、vv 全同）→ 本地标记为干净；否则标记 dirty 待推。

## 6. HTTP API

基址 `https://<server>/v1`，JSON。除 §6.1 外均需 `Authorization: Bearer <token>`。客户端
只允许 `https://`，`http://` 仅限回环地址（开发/自托管本机）。错误统一：

```json
{"error": "<code>", "message": "人类可读说明", ...}
```

| HTTP | error | 说明 |
|---|---|---|
| 400 | `bad_request` | 参数缺失/格式错 |
| 401 | `invalid_credentials` | 口令/恢复码错误（不区分账号是否存在） |
| 401 | `totp_required` | 口令正确但需 TOTP |
| 401 | `totp_invalid` | TOTP 错误 |
| 401 | `unauthorized` | token 无效 |
| 401 | `device_revoked` | 本设备已被吊销；附 `"wipe": bool` |
| 409 | `email_taken` | 注册邮箱已存在 |
| 429 | `rate_limited` | 附 `"retry_after": 秒` |

### 6.1 账号（无需 token）

- `POST /prelogin` `{email}` → `{kdf: {salt, ops, mem}}`。账号不存在时返回确定性伪造
  参数（`salt = HMAC-SHA256(server_secret, email)[0..16]`，默认 ops/mem），不泄露账号是否存在。
- `POST /register`
  `{email, auth_key, kdf: {salt, ops, mem}, wrapped_vk_pw, rc_auth, wrapped_vk_rc, device: {name, platform}}`
  → `{account_id, device_id, token}`。
- `POST /login` `{email, auth_key, totp?, device: {name, platform}}`
  → `{account_id, device_id, token, wrapped_vk_pw, totp_enabled}`。
  失败不返回任何 vault 数据。
- `POST /recover/start` `{email, rc_auth}` → `{recovery_token, wrapped_vk_rc}`（token 10 分钟有效）。
- `POST /recover/finish` `{recovery_token, auth_key, kdf, wrapped_vk_pw, device}`
  → `{account_id, device_id, token}`。改口令；其它设备的 token 保持有效。

### 6.2 已登录

- `POST /password` `{auth_key_old, auth_key, kdf, wrapped_vk_pw}` → `{}`。
- `POST /logout` → `{}`（吊销当前 token / 设备）。
- `GET /devices` → `{devices: [{id, name, platform, created, last_seen, current}]}`。
- `POST /devices/{id}/revoke` `{wipe: bool}` → `{}`。被吊销设备之后的任何请求得到
  `device_revoked`（含 wipe 标记），WebSocket 立即收到 `revoked`。
- `POST /totp/setup` → `{secret, uri}`（base32 密钥，`otpauth://totp/Harbor:<email>?secret=…&issuer=Harbor`）；
  `POST /totp/enable` `{code}`；`POST /totp/disable` `{code}`。TOTP 为 RFC 6238
  （SHA1、6 位、30 s、容忍 ±1 步）。
- `GET /objects?since=<seq>&limit=<n≤500>`
  → `{objects: [{id, type, seq, blob, updated_at}], next: <seq>, more: bool}`，按 seq 升序。
- `POST /objects` `{changes: [{id, type, base_seq, blob}]}`（≤ 200 条）
  → `{results: [{id, ok: true, seq} | {id, ok: false, conflict: {seq, blob, type}}]}`。
  **比较并交换**：服务端当前该 id 的 seq 等于 `base_seq`（不存在时须为 0）才接受并分配新 seq；
  否则返回当前版本。逐条原子。

### 6.3 WebSocket

`GET /ws`（Upgrade，带 Bearer 头）。服务端推送：

- `{"t":"changed","seq":N}`：账号有新提交（含其它设备）；
- `{"t":"revoked","wipe":bool}`：本设备被吊销，随后关闭连接。

客户端不发业务消息；用 WebSocket ping 保活（30 s）。

### 6.4 限流

- 每账号连续失败（登录/恢复/TOTP）≥ 5 次 → 锁定 60 s，之后每再失败一次锁定时长翻倍，
  上限 1 h；成功登录清零。锁定期间即使口令正确也返回 `rate_limited`。
- 每 IP：认证类接口 20 次/分钟。

## 7. 客户端同步循环

本地 vault 为每个对象保存 `sync_seq`（本地状态所基于的服务端 seq，0 = 服务端没有），
vault meta 保存 `sync_cursor`（已应用的最大服务端 seq）。

1. **拉**：`GET /objects?since=cursor` 直到 `more=false`。对每条：解密 → 本地无 → 落库为干净
   （`sync_seq = seq`）；本地干净且 `L.vv ≤ R.vv` → 直接替换；否则合并（§5），`sync_seq = seq`。
2. **推**：范围内的 dirty 对象封装后 `POST /objects`（`base_seq = sync_seq`）。成功 →
   若封装后本地未再修改则标干净，`sync_seq = 新 seq`；冲突 → 与返回版本合并、
   `sync_seq = conflict.seq`，再推（单轮最多重试 3 次）。
3. 触发：本地变更（防抖 1 s）、WebSocket `changed`、网络恢复（GNetworkMonitor）、
   WebSocket 断开时每 30 s 轮询一次。网络恢复后 5 s 内完成一轮。
4. `device_revoked`：停止同步、登出；`wipe=true` 时擦除本地同步范围内的全部数据。

## 8. 首次登录与登出

- 新设备登录：prelogin → 派生 MK → login → 解包 VK → 拉取并解密 → 完成。
  本地已有数据时询问："合并本机数据到账号" / "丢弃本机数据，使用云端"。
- 登出：可选"保留本地副本"（解绑账号，数据留在本机）或"擦除本地 Vault"。
- 无恢复码且无已登录设备时数据不可恢复；注册时明确提示并要求确认已保存恢复码。
