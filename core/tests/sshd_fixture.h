#ifndef HB_SSHD_FIXTURE_H
#define HB_SSHD_FIXTURE_H

/* 用户态 sshd fixture（test_ssh 同款思路的精简版，可多实例）。
 * 含 Subsystem sftp internal-sftp；与本机同 FS —— "远端"路径就是
 * fixture 目录下的真实路径，可直接 stat/写文件制造远端状态。
 *
 * hb_sshd_start: 0 成功；77 = 缺 sshd/ssh-keygen（调用方应 exit(77)）。 */

#include <limits.h>
#include <sys/types.h>

typedef struct {
    char   dir[PATH_MAX];        /* fixture 根（结束后 rm -rf） */
    int    port;
    pid_t  pid;                  /* sshd -D master */
    char   user[64];
    char   key[16384];           /* client 私钥文本（内存认证用） */
    char   keyfile[PATH_MAX + 64]; /* client 私钥路径（ssh CLI 用） */
    char   khline[1024];         /* "[127.0.0.1]:port ssh-ed25519 b64" */
} hb_sshd;

int  hb_sshd_start(hb_sshd *fx);
void hb_sshd_stop(hb_sshd *fx);

#endif
