/**
 * @file fs.h
 * @brief Small file layer for the alarm log, credential vault and runtime report
 *
 * One implementation (fs_stdio.c) on top of C stdio, with the two operations stdio cannot
 * express (flush to disk, atomic replace) done per OS. Handles come from a static pool
 * of HAL_FILE_POOL entries: no heap. On a target without a file system, provide a
 * replacement fs_*.c (for example a flash ring) with the same functions.
 */

#ifndef INDUSTRIAL_HMI_HAL_FS_H
#define INDUSTRIAL_HMI_HAL_FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HAL_FILE_POOL 4

typedef struct hal_file hal_file_t;

typedef enum {
    HAL_FILE_READ,       /* existing file, read only */
    HAL_FILE_APPEND,     /* create if missing; every write goes to the end; readable via hal_file_pread */
    HAL_FILE_WRITE_NEW   /* create or truncate */
} hal_file_mode_t;

hal_file_t *hal_file_open(const char *path, hal_file_mode_t mode);     /* NULL on failure */
int         hal_file_read(hal_file_t *f, void *buf, size_t cap);       /* bytes, 0 = end of file, -1 = error */
bool        hal_file_write(hal_file_t *f, const void *buf, size_t len);/* all or nothing as far as reported */
bool        hal_file_sync(hal_file_t *f);                              /* flush to the medium */
uint32_t    hal_file_size(hal_file_t *f);
/* Read exactly n bytes at an absolute offset (the append position is kept). */
bool        hal_file_pread(hal_file_t *f, uint32_t offset, void *buf, size_t n);
void        hal_file_close(hal_file_t *f);                             /* NULL is allowed */

/* Make `dst` the file `tmp`, replacing dst. Atomic where the OS allows (rename on POSIX,
 * MoveFileEx with write-through on Windows). */
bool        hal_file_replace(const char *tmp, const char *dst);
bool        hal_file_remove(const char *path);

#endif /* INDUSTRIAL_HMI_HAL_FS_H */
