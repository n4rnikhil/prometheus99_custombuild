/**
 * @file platform.h
 * @brief What the portable application core asks of the platform shell
 *
 * Each shell (src/platform/win32, src/platform/linux, ports/esp32) implements these and
 * drives the application through app.h. Every function may be a no-op when the platform
 * has no equivalent (a headless server has no beep and nothing to repaint).
 */

#ifndef INDUSTRIAL_HMI_PLATFORM_H
#define INDUSTRIAL_HMI_PLATFORM_H

void hal_console_print(const char *text);   /* command-line messages (usage errors, --verify-log result) */
void hal_beep(void);                        /* short pulse, 1 Hz while the horn sounds (where hal_horn has no tone) */
/* Steady alarm output (buzzer, stack light, beacon relay): true while unacknowledged alarms
 * exist and the horn is not silenced. Called every UI frame. A no-op where nothing is wired. */
#include <stdbool.h>
void hal_horn(bool on);
/* The panel's own alarm sound (two-tone, looped while the horn sounds). On by default; --no-sound
 * turns it off. A wired horn output (GPIO, board pin) is not affected. */
void hal_sound_enable(bool on);
void hal_trim_memory(void);                 /* give back working-set pages (Windows); no-op elsewhere */
void hal_request_repaint(void);             /* call app_render() soon */
void hal_request_quit(void);                /* leave the main loop (--exit-after-ms) */
void hal_fatal(const char *message);        /* unrecoverable start-up error, shown to the operator */

#include <stddef.h>
#include <stdbool.h>
/* Show prompt and read one line from the operator without echoing it (PIN entry for --set-pin).
 * The line end is stripped. false = no interactive console on this platform, or input ended. */
bool hal_console_read_secret(const char *prompt, char *out, size_t cap);
/* One HMI per data folder (path: a lock file next to hmi.key). false = another HMI already runs
 * with it; the platform says so (Windows brings the running window forward). If the lock cannot
 * be taken for another reason (read-only medium) the start is not blocked. */
bool hal_instance_lock(const char *path);

#endif /* INDUSTRIAL_HMI_PLATFORM_H */
