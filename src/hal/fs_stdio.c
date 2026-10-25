/**
 * @file fs_stdio.c
 * @brief hal/fs.h on C stdio (Windows, Linux, Raspberry Pi, ESP-IDF VFS)
 */

#include <stdio.h>
#include <string.h>

#include "../../include/hal/fs.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

struct hal_file {
    FILE *f;
    bool  used;
};

static struct hal_file s_pool[HAL_FILE_POOL];

hal_file_t *hal_file_open(const char *path, hal_file_mode_t mode)
{
    if (!path) return NULL;
    hal_file_t *h = NULL;
    for (int i = 0; i < HAL_FILE_POOL; i++) if (!s_pool[i].used) { h = &s_pool[i]; break; }
    if (!h) return NULL;
    const char *m = (mode == HAL_FILE_READ) ? "rb" : (mode == HAL_FILE_APPEND) ? "a+b" : "wb";
    h->f = fopen(path, m);
    if (!h->f) return NULL;
    h->used = true;
    return h;
}

int hal_file_read(hal_file_t *h, void *buf, size_t cap)
{
    if (!h || !buf) return -1;
    size_t n = fread(buf, 1, cap, h->f);
    if (n == 0 && ferror(h->f)) return -1;
    return (int)n;
}

bool hal_file_write(hal_file_t *h, const void *buf, size_t len)
{
    if (!h || !buf) return false;
    return fwrite(buf, 1, len, h->f) == len;
}

bool hal_file_sync(hal_file_t *h)
{
    if (!h || fflush(h->f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(h->f)) == 0;
#else
    return fsync(fileno(h->f)) == 0;
#endif
}

uint32_t hal_file_size(hal_file_t *h)
{
    if (!h) return 0;
    long here = ftell(h->f);
    if (fseek(h->f, 0, SEEK_END) != 0) return 0;
    long end = ftell(h->f);
    fseek(h->f, here < 0 ? 0 : here, SEEK_SET);
    return end < 0 ? 0 : (uint32_t)end;
}

bool hal_file_pread(hal_file_t *h, uint32_t offset, void *buf, size_t n)
{
    if (!h || !buf) return false;
    bool ok = fseek(h->f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, n, h->f) == n;
    fseek(h->f, 0, SEEK_END);            /* C requires a seek between a read and the next write */
    return ok;
}

void hal_file_close(hal_file_t *h)
{
    if (!h || !h->used) return;
    fclose(h->f);
    h->f = NULL;
    h->used = false;
}

bool hal_file_replace(const char *tmp, const char *dst)
{
#if defined(_WIN32)
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

bool hal_file_remove(const char *path)
{
    return path && remove(path) == 0;
}
