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
void hal_beep(void);                        /* alarm horn pulse */
void hal_trim_memory(void);                 /* give back working-set pages (Windows); no-op elsewhere */
void hal_request_repaint(void);             /* call app_render() soon */
void hal_request_quit(void);                /* leave the main loop (--exit-after-ms) */
void hal_fatal(const char *message);        /* unrecoverable start-up error, shown to the operator */

#endif /* INDUSTRIAL_HMI_PLATFORM_H */
