#ifndef HB_SFTP_PRIV_H
#define HB_SFTP_PRIV_H

/* sftp.c 内部接口：文件名/路径安全检查，供 core/tests/test_sftp.c 直接
 * 做单元测试（无法伪造恶意 sftp 服务器，只能直接测判定逻辑）。 */

#include <stddef.h>

/* 服务器报来的目录项名是否合法：非空、非 "."/".."、不含 '/'、内嵌 NUL
 * （nlen 为线上长度，名字里塞 NUL 会截短成真路径，必须按长度拒）和控制
 * 字符 < 0x20。CVE-2019-6111 类目录穿越防护。 */
int   hb__sftp_name_ok(const char *n, size_t nlen);

/* 词法归一化路径（不查文件系统）：去掉重复 '/'、'.' 分量，弹掉 '..'。
 * 返回 malloc 串，调用方 free；NULL = OOM。 */
char *hb__sftp_normpath(const char *p);

/* path 词法归一后是否位于 root（同样归一）之内（含 root 本身）。 */
int   hb__sftp_path_under(const char *root, const char *path);

#endif
