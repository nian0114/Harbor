#ifndef HB_OBJ_PRIV_H
#define HB_OBJ_PRIV_H

/* 内部头文件：暴露 hb_obj 布局与构造函数给 vault.c / 测试。 */

#include "harbor/vault.h"
#include "cJSON.h"

struct hb_obj {
    char   *id;          /* malloc'd */
    char   *type;        /* malloc'd */
    cJSON  *data;        /* JSON object，含保留成员 "_fv" */
    cJSON  *vv;          /* version vector {device_id: counter} */
    int64_t updated_at;  /* ms */
    int     tombstone;
};

hb_obj *hb_obj_new_with_id(const char *type, const char *id);

#endif
