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
bool        hal_file_flush(hal_file_t *f);                             /* hand buffered data to the OS (survives a crash, not a power cut) */
uint32_t    hal_file_size(hal_file_t *f);
/* Read exactly n bytes at an absolute offset (the append position is kept). */
bool        hal_file_pread(hal_file_t *f, uint32_t offset, void *buf, size_t n);
void        hal_file_close(hal_file_t *f);                             /* NULL is allowed */

/* Make `dst` the file `tmp`, replacing dst. Atomic where the OS allows (rename on POSIX,
 * MoveFileEx with write-through on Windows). */
bool        hal_file_replace(const char *tmp, const char *dst);
bool        hal_file_remove(const char *path);

/* Limit a secret file (device key, credential vault) to the account that runs the runtime.
 *   like == NULL or a folder: POSIX mode 0600; when run as root, the file is also given to the
 *       owner of `like` (the install folder), so a key made by an administrator stays readable
 *       by the HMI service account. Windows: a protected DACL with full control for SYSTEM,
 *       Administrators and the current account only (inherited folder rights are dropped).
 *   like == an existing file: the same owner (POSIX, when root) or the same DACL (Windows) as
 *       that file, so the vault always follows the key.
 * ESP32: nothing to do (single-user flash file system). */
bool        hal_file_restrict(const char *path, const char *like);

/* The protected folder for secrets (device key, credential vault), created on first use and then
 * limited to the account that created it:
 *   Windows  %ProgramData%\Prometheus HMI\   protected DACL: SYSTEM, Administrators, this account
 *   Linux    /var/lib/prometheus-hmi/ when run as root, else ~/.local/state/prometheus-hmi/   mode 0700
 * A folder that already exists is used only when it is a real folder owned by this account (or, on
 * Windows, by SYSTEM or Administrators); it is then protected again. One that another account
 * made first is refused.
 * Writes the path with a trailing separator. create = false only works out the path (nothing is
 * made). false: no such folder on this system (ESP32), or it could not be created; the caller
 * then keeps its secrets next to the program. */
bool        hal_secure_dir(char *out, size_t cap, bool create);

#endif /* INDUSTRIAL_HMI_HAL_FS_H */
