#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL 文件系统驱动：桥接 esp_littlefs (/littlefs)。
 * 注册后可用 "S:/littlefs/<file>" 路径访问，如 lv_binfont_create()。 */
#define LV_FS_LITTLEFS_LETTER 'S'

void lv_fs_littlefs_register(void);

#ifdef __cplusplus
}
#endif
