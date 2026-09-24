#include "lv_fs_littlefs.h"

#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "LVFS";

#define LITTLEFS_BASE_PATH "/littlefs"

static bool lvfs_ready_cb(lv_fs_drv_t *drv)
{
    (void)drv;
    return true;
}

static void *lvfs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    /* LVGL 传入的 path 已去掉盘符前缀（如 "littlefs/xxx.bin"），直接挂到挂载点下 */
    char full[LV_FS_MAX_PATH_LENGTH];
    if (path[0] == '/') {
        snprintf(full, sizeof(full), LITTLEFS_BASE_PATH "%s", path);
    } else {
        snprintf(full, sizeof(full), LITTLEFS_BASE_PATH "/%s", path);
    }
    const char *m = (mode & LV_FS_MODE_WR) ? "r+b" : "rb";
    FILE *f = fopen(full, m);
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", full);
    }
    return (void *)f;
}

static lv_fs_res_t lvfs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    if (file_p != NULL) {
        fclose((FILE *)file_p);
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t lvfs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf,
                                uint32_t btr, uint32_t *br)
{
    (void)drv;
    if (file_p == NULL) {
        return LV_FS_RES_UNKNOWN;
    }
    const size_t n = fread(buf, 1, btr, (FILE *)file_p);
    if (br != NULL) {
        *br = (uint32_t)n;
    }
    return LV_FS_RES_OK;   /* EOF 由 br 体现 */
}

static lv_fs_res_t lvfs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                                lv_fs_whence_t whence)
{
    (void)drv;
    if (file_p == NULL) {
        return LV_FS_RES_UNKNOWN;
    }
    int w = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) {
        w = SEEK_CUR;
    } else if (whence == LV_FS_SEEK_END) {
        w = SEEK_END;
    }
    return fseek((FILE *)file_p, (long)pos, w) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t lvfs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    if (file_p == NULL) {
        return LV_FS_RES_UNKNOWN;
    }
    const long p = ftell((FILE *)file_p);
    if (pos_p != NULL) {
        *pos_p = (uint32_t)p;
    }
    return p >= 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

void lv_fs_littlefs_register(void)
{
    static lv_fs_drv_t drv;

    lv_fs_drv_init(&drv);
    drv.letter = LV_FS_LITTLEFS_LETTER;
    drv.cache_size = 256;
    drv.ready_cb = lvfs_ready_cb;
    drv.open_cb = lvfs_open_cb;
    drv.close_cb = lvfs_close_cb;
    drv.read_cb = lvfs_read_cb;
    drv.seek_cb = lvfs_seek_cb;
    drv.tell_cb = lvfs_tell_cb;
    lv_fs_drv_register(&drv);
    ESP_LOGI(TAG, "registered: %c:/littlefs", (char)LV_FS_LITTLEFS_LETTER);
}
