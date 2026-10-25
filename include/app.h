/**
 * @file app.h
 * @brief Portable HMI application core: state, worker threads, input handling, rendering
 *
 * A platform shell creates the window or opens the framebuffer, then drives this API:
 *
 *   rc = app_parse_args(argc, argv);   if (rc >= 0) return rc;
 *   rc = app_init();                   if (rc != 0)  return rc;
 *   ... create the display, call app_resize(w, h) ...
 *   app_start_workers();
 *   while (app_running()) {
 *       ... deliver input: app_pointer_*(), app_key() ...
 *       wait_ms = app_tick();           // runs a frame when one is due
 *       ... if a repaint was requested: app_render(present, ctx) ...
 *       ... sleep up to wait_ms or until input arrives ...
 *   }
 *   app_shutdown();
 *
 * The core never includes an OS header; everything platform-specific is behind include/hal/.
 */

#ifndef INDUSTRIAL_HMI_APP_H
#define INDUSTRIAL_HMI_APP_H

#include "config.h"

/* Key codes handed to app_key(): letters 'A'..'Z' (upper case), digits '0'..'9' (numpad too),
 * ' ' for space, '.' and '-' (setpoint entry), and the codes below. */
#define HMI_K_TAB    0x101
#define HMI_K_LEFT   0x102
#define HMI_K_RIGHT  0x103
#define HMI_K_ENTER  0x104
#define HMI_K_ESC    0x105
#define HMI_K_BACK   0x106

/* Called once per band, top to bottom. palette16 holds 16 entries 0x00RRGGBB. */
typedef void (*app_present_fn)(void *ctx, const uint8_t *band, int y, int rows, int width,
                               const uint32_t *palette16);

/* -1 = carry on running; anything else is the process exit code (also for --verify-log, --set-pin). */
int  app_parse_args(int argc, char **argv);
int  app_init(void);                 /* 0 = ok */
bool app_start_workers(void);        /* fieldbus thread (+ discovery) */

void     app_resize(int width, int height);
uint32_t app_tick(void);             /* runs a frame if due; returns ms until the next one is due */
void     app_frame(uint32_t now_ms); /* force one frame (a shell's modal loop calls this to keep the heartbeat) */

void app_pointer_down(int x, int y);
void app_pointer_move(int x, int y);
void app_pointer_up(int x, int y);
void app_pointer_cancel(void);
bool app_pointer_active(void);
void app_key(uint16_t key, bool shift);

void app_render(app_present_fn present, void *ctx);
int  app_width(void);
int  app_height(void);

bool app_running(void);
void app_stop(void);                 /* thread-safe */
void app_shutdown(void);             /* joins the workers, flushes and closes the log, writes --report */

#endif /* INDUSTRIAL_HMI_APP_H */
