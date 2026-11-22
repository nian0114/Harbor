# Harbor 加密备份文件格式 v1（FR-15 数据导入导出）

跨平台通用的离线备份：导出本机 Vault 的全部对象（含 secret），以用户另设的备份口令加密。
编码约定沿用 `sync-protocol.md` §2（口令 NFKC、XChaCha20-Poly1305-IETF）。扩展名 `.harbor-backup`。

## 1. 文件布局

```
offset  size  字段
0       4     magic  "HBBK"
4       1     version 0x01
5       1     kdf_alg 0x01 = Argon2id13
6       4     ops   (uint32 BE)
10      4     mem_kib (uint32 BE，内存上限 / 1024)
14      16    salt
30      24    nonce
54      …     ciphertext || tag(16)
```

- `key = crypto_pwhash(32, password_nfkc, salt, ops, mem_kib*1024, ARGON2ID13)`。
- 导出默认 `ops = OPSLIMIT_MODERATE`、`mem = MEMLIMIT_MODERATE`；导入拒绝 `ops < 2`、`mem < 64 MiB`、
  `mem > 1 GiB`（防恶意文件耗尽内存）。
- `ad = 文件前 30 字节`（magic 到 salt），篡改头部即解密失败。
- 明文 = `sodium_pad(json, 4096)` 后的字节（弱化长度泄露），解密后 `sodium_unpad`。

## 2. 明文 JSON

```json
{
  "format": "harbor-backup",
  "version": 1,
  "created": 1700000000000,
  "device": "<导出设备名>",
  "objects": [
    { "id": "…", "type": "host", "data": { …业务字段，不含 _fv… }, "secret": { "password": "…" } }
  ]
}
```

- 包含所有非 tombstone 对象（host/group/key/snippet/forward/known/settings…）；`secret` 为该对象 secret
  映射明文，没有则省略。不含 logs、shell history、同步元数据（vv/_fv/sync_seq/冲突）、账号凭据。
- 设备本地字段（`sync-protocol.md` §4.4）**包含**在备份中（备份是本机数据的完整副本），导入时同样写回。
- 明文 JSON 与 secret 只在锁定内存中组装/解析，用完即擦。

## 3. 导入语义

- 口令错误 / 文件损坏 → 统一报"口令错误或文件已损坏"，不写入任何数据。
- 逐对象以普通写入落库（产生新的字段版本戳，之后正常经 FR-11 同步）：
  - 本地不存在该 id → 新建；
  - 本地存在 → 用备份内容覆盖业务字段与 secret（本地已删除的对象被恢复）；
  - `settings` 单例按字段覆盖。
- 导入前向用户显示对象数量统计并要求确认；导入完成报告新建/覆盖数量。
