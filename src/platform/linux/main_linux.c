/**
 * @file main_linux.c
 * @brief Linux shell (PC, Raspberry Pi, any ARM/x86 board): framebuffer output, evdev input
 *
 * No X11, Wayland, SDL or other library: the HMI draws straight into /dev/fb0 one 4-bpp band
 * at a time (converted through the 16-colour palette to the panel's 16/24/32-bpp format) and
 * reads touch screens, mice and keyboards from /dev/input/event*. Input devices are re-scanned
 * every few seconds, so a USB keyboard or touch panel plugged in later just works.
 *
 * Shell options (all other options go to the application, see src/app.c):
 *   --fb <device>     framebuffer device (default /dev/fb0, or $HMI_FBDEV)
 *   --headless        no display at all: Modbus, alarms, log and report only (servers, CI)
 * Touch calibration via environment: HMI_TOUCH_SWAP_XY=1, HMI_TOUCH_INVERT_X=1, HMI_TOUCH_INVERT_Y=1
 *
 * Run from a text console (Raspberry Pi OS Lite, or Ctrl+Alt+F2 on a desktop). The user needs
 * access to the framebuffer and input devices: groups "video" and "input", or root.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#include "../../../include/app.h"
#include "../../../include/hal/platform.h"
#include "../../../include/hal/sys.h"
#include "../../../include/hal/timer.h"

/* ------------------------------------------------------------------------- */
/* Hooks for the application core                                             */
/* ------------------------------------------------------------------------- */
static volatile sig_atomic_t g_repaint = 1;

void hal_console_print(const char *text) { fputs(text, stdout); fflush(stdout); }
void hal_beep(void)            {}
void hal_trim_memory(void)     {}
void hal_request_repaint(void) { g_repaint = 1; }
void hal_request_quit(void)    { app_stop(); }
void hal_fatal(const char *message) { fprintf(stderr, "industrial_hmi: %s\n", message); }

/* ------------------------------------------------------------------------- */
/* Framebuffer                                                                */
/* ------------------------------------------------------------------------- */
static struct {
    int       fd;
    uint8_t  *mem;
    size_t    mem_len;
    int       xres, yres;
    int       bpp;                  /* bytes per pixel: 2, 3 or 4 */
    uint32_t  line;                 /* bytes per line */
    uint32_t  origin;               /* byte offset of the visible (0,0) */
    struct fb_bitfield r, g, b;
    uint32_t  lut[16];              /* palette in the panel's pixel format */
    uint32_t  lut_src[16];
} s_fb = { -1, NULL, 0, 0, 0, 0, 0, 0, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0}, {0} };

static int s_tty = -1;              /* console switched to graphics mode while we run */

static uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - s_fb.r.length)) << s_fb.r.offset) |
           ((g >> (8 - s_fb.g.length)) << s_fb.g.offset) |
           ((b >> (8 - s_fb.b.length)) << s_fb.b.offset);
}

static bool fb_open(const char *dev)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    s_fb.fd = open(dev, O_RDWR | O_CLOEXEC);
    if (s_fb.fd < 0) { fprintf(stderr, "industrial_hmi: cannot open %s: %s\n", dev, strerror(errno)); return false; }
    if (ioctl(s_fb.fd, FBIOGET_VSCREENINFO, &v) != 0 || ioctl(s_fb.fd, FBIOGET_FSCREENINFO, &f) != 0) {
        fprintf(stderr, "industrial_hmi: %s is not a framebuffer\n", dev);
        return false;
    }
    if (v.bits_per_pixel != 16 && v.bits_per_pixel != 24 && v.bits_per_pixel != 32) {
        fprintf(stderr, "industrial_hmi: %u bpp framebuffers are not supported (16/24/32)\n", v.bits_per_pixel);
        return false;
    }
    if (v.red.length == 0 || v.red.length > 8 || v.green.length > 8 || v.blue.length > 8) {
        fprintf(stderr, "industrial_hmi: unsupported framebuffer colour layout\n");
        return false;
    }
    s_fb.xres = (int)v.xres;
    s_fb.yres = (int)v.yres;
    s_fb.bpp = (int)v.bits_per_pixel / 8;
    s_fb.line = f.line_length;
    s_fb.origin = v.yoffset * f.line_length + v.xoffset * (uint32_t)s_fb.bpp;
    s_fb.r = v.red; s_fb.g = v.green; s_fb.b = v.blue;
    s_fb.mem_len = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres_virtual;
    s_fb.mem = (uint8_t *)mmap(NULL, s_fb.mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, s_fb.fd, 0);
    if (s_fb.mem == MAP_FAILED) {
        s_fb.mem = NULL;
        fprintf(stderr, "industrial_hmi: cannot map %s: %s\n", dev, strerror(errno));
        return false;
    }
    for (int i = 0; i < 16; i++) { s_fb.lut_src[i] = 0xFFFFFFFFu; }

    /* Keep the text console from drawing over the HMI (restored on exit) */
    s_tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (s_tty >= 0 && ioctl(s_tty, KDSETMODE, KD_GRAPHICS) != 0) { close(s_tty); s_tty = -1; }
    return true;
}

static void fb_close(void)
{
    if (s_tty >= 0) { ioctl(s_tty, KDSETMODE, KD_TEXT); close(s_tty); s_tty = -1; }
    if (s_fb.mem) { munmap(s_fb.mem, s_fb.mem_len); s_fb.mem = NULL; }
    if (s_fb.fd >= 0) { close(s_fb.fd); s_fb.fd = -1; }
}

static void present_band(void *ctx, const uint8_t *band, int y, int rows, int width, const uint32_t *pal)
{
    (void)ctx;
    if (!s_fb.mem || !band || !pal) return;
    for (int i = 0; i < 16; i++) {
        if (s_fb.lut_src[i] != pal[i]) { s_fb.lut_src[i] = pal[i]; s_fb.lut[i] = pack(pal[i]); }
    }
    int w = width < s_fb.xres ? width : s_fb.xres;        /* renderer rows are padded to 8 px */
    int stride = width / 2;
    for (int r = 0; r < rows; r++) {
        int sy = y + r;
        if (sy >= s_fb.yres) break;
        size_t off = s_fb.origin + (size_t)sy * s_fb.line;
        if (off + (size_t)w * (size_t)s_fb.bpp > s_fb.mem_len) break;
        const uint8_t *src = band + (size_t)r * (size_t)stride;
        uint8_t *dst = s_fb.mem + off;
        if (s_fb.bpp == 2) {
            uint16_t *d = (uint16_t *)(void *)dst;
            for (int x = 0; x < w; x++) d[x] = (uint16_t)s_fb.lut[(x & 1) ? (src[x >> 1] & 15) : (src[x >> 1] >> 4)];
        } else if (s_fb.bpp == 4) {
            uint32_t *d = (uint32_t *)(void *)dst;
            for (int x = 0; x < w; x++) d[x] = s_fb.lut[(x & 1) ? (src[x >> 1] & 15) : (src[x >> 1] >> 4)];
        } else {
            for (int x = 0; x < w; x++) {
                uint32_t c = s_fb.lut[(x & 1) ? (src[x >> 1] & 15) : (src[x >> 1] >> 4)];
                dst[3 * x] = (uint8_t)c; dst[3 * x + 1] = (uint8_t)(c >> 8); dst[3 * x + 2] = (uint8_t)(c >> 16);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* evdev input (touch screens, mice, keyboards), hot-plugged                  */
/* ------------------------------------------------------------------------- */
#define MAX_INPUTS 8
#define BITS_LONG  (8 * sizeof(unsigned long))
#define TEST_BIT(arr, bit) (((arr)[(bit) / BITS_LONG] >> ((bit) % BITS_LONG)) & 1UL)

typedef struct {
    int  fd;
    int  node;                       /* N of /dev/input/eventN */
    bool abs_dev, rel_dev;
    int  ax_min, ax_max, ay_min, ay_max;
    int  raw_x, raw_y;               /* last absolute position */
    bool down, was_down, moved;
} input_t;

static input_t s_in[MAX_INPUTS];
static int     s_ptr_x, s_ptr_y;     /* pointer position in screen pixels */
static bool    s_shift;
static bool    s_swap_xy, s_inv_x, s_inv_y;

static bool env_on(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] == '1';
}

static bool input_known(int node)
{
    for (int i = 0; i < MAX_INPUTS; i++) if (s_in[i].fd >= 0 && s_in[i].node == node) return true;
    return false;
}

static void input_scan(void)
{
    for (int node = 0; node < 32; node++) {
        if (input_known(node)) continue;
        int slot = -1;
        for (int i = 0; i < MAX_INPUTS; i++) if (s_in[i].fd < 0) { slot = i; break; }
        if (slot < 0) return;
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", node);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long ev[(EV_MAX + BITS_LONG) / BITS_LONG];
        unsigned long keys[(KEY_MAX + BITS_LONG) / BITS_LONG];
        unsigned long abs_bits[(ABS_MAX + BITS_LONG) / BITS_LONG];
        memset(ev, 0, sizeof(ev)); memset(keys, 0, sizeof(keys)); memset(abs_bits, 0, sizeof(abs_bits));
        ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        input_t *in = &s_in[slot];
        memset(in, 0, sizeof(*in));
        in->node = node;
        in->abs_dev = TEST_BIT(ev, EV_ABS) && TEST_BIT(abs_bits, ABS_X) && TEST_BIT(abs_bits, ABS_Y);
        in->rel_dev = TEST_BIT(ev, EV_REL) && TEST_BIT(keys, BTN_LEFT);
        bool keyboard = TEST_BIT(ev, EV_KEY) && TEST_BIT(keys, KEY_A) && TEST_BIT(keys, KEY_ENTER);
        if (!in->abs_dev && !in->rel_dev && !keyboard) { close(fd); continue; }
        if (in->abs_dev) {
            struct input_absinfo ax, ay;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 &&
                ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                in->ax_min = ax.minimum; in->ax_max = ax.maximum;
                in->ay_min = ay.minimum; in->ay_max = ay.maximum;
            } else {
                in->abs_dev = false;
            }
        }
        in->fd = fd;
    }
}

static int scale(int v, int lo, int hi, int size, bool invert)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    long p = (long)(v - lo) * (size - 1) / (hi - lo);
    return invert ? size - 1 - (int)p : (int)p;
}

static uint16_t map_key(int code)
{
    static const struct { int code; uint16_t key; } table[] = {
        { KEY_A, 'A' }, { KEY_B, 'B' }, { KEY_C, 'C' }, { KEY_D, 'D' }, { KEY_E, 'E' }, { KEY_F, 'F' },
        { KEY_G, 'G' }, { KEY_H, 'H' }, { KEY_I, 'I' }, { KEY_J, 'J' }, { KEY_K, 'K' }, { KEY_L, 'L' },
        { KEY_M, 'M' }, { KEY_N, 'N' }, { KEY_O, 'O' }, { KEY_P, 'P' }, { KEY_Q, 'Q' }, { KEY_R, 'R' },
        { KEY_S, 'S' }, { KEY_T, 'T' }, { KEY_U, 'U' }, { KEY_V, 'V' }, { KEY_W, 'W' }, { KEY_X, 'X' },
        { KEY_Y, 'Y' }, { KEY_Z, 'Z' },
        { KEY_0, '0' }, { KEY_1, '1' }, { KEY_2, '2' }, { KEY_3, '3' }, { KEY_4, '4' },
        { KEY_5, '5' }, { KEY_6, '6' }, { KEY_7, '7' }, { KEY_8, '8' }, { KEY_9, '9' },
        { KEY_KP0, '0' }, { KEY_KP1, '1' }, { KEY_KP2, '2' }, { KEY_KP3, '3' }, { KEY_KP4, '4' },
        { KEY_KP5, '5' }, { KEY_KP6, '6' }, { KEY_KP7, '7' }, { KEY_KP8, '8' }, { KEY_KP9, '9' },
        { KEY_TAB, HMI_K_TAB }, { KEY_LEFT, HMI_K_LEFT }, { KEY_RIGHT, HMI_K_RIGHT },
        { KEY_ENTER, HMI_K_ENTER }, { KEY_KPENTER, HMI_K_ENTER }, { KEY_ESC, HMI_K_ESC },
        { KEY_BACKSPACE, HMI_K_BACK }, { KEY_SPACE, ' ' },
        { KEY_DOT, '.' }, { KEY_KPDOT, '.' }, { KEY_MINUS, '-' }, { KEY_KPMINUS, '-' },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) if (table[i].code == code) return table[i].key;
    return 0;
}

static void input_close(input_t *in)
{
    if (in->down) app_pointer_cancel();            /* touch controller unplugged mid-gesture */
    close(in->fd);
    in->fd = -1;
}

static void input_read(input_t *in, int sw, int sh)
{
    struct input_event e[32];
    for (;;) {
        ssize_t n = read(in->fd, e, sizeof(e));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            input_close(in);                       /* ENODEV: device unplugged */
            return;
        }
        if (n == 0) return;
        for (size_t i = 0; i < (size_t)n / sizeof(e[0]); i++) {
            const struct input_event *ev = &e[i];
            if (ev->type == EV_ABS && in->abs_dev) {
                if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X) { in->raw_x = ev->value; in->moved = true; }
                if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y) { in->raw_y = ev->value; in->moved = true; }
            } else if (ev->type == EV_REL) {
                if (ev->code == REL_X) { s_ptr_x += ev->value; in->moved = true; }
                if (ev->code == REL_Y) { s_ptr_y += ev->value; in->moved = true; }
            } else if (ev->type == EV_KEY) {
                if (ev->code == BTN_TOUCH || ev->code == BTN_LEFT) {
                    in->down = ev->value != 0;
                } else if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT) {
                    s_shift = ev->value != 0;
                } else if (ev->value == 1) {                   /* press; 2 = auto-repeat, ignored */
                    uint16_t k = map_key(ev->code);
                    if (k) app_key(k, s_shift);
                }
            } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                if (in->abs_dev && in->moved) {
                    int x = s_swap_xy ? scale(in->raw_y, in->ay_min, in->ay_max, sw, s_inv_x)
                                      : scale(in->raw_x, in->ax_min, in->ax_max, sw, s_inv_x);
                    int y = s_swap_xy ? scale(in->raw_x, in->ax_min, in->ax_max, sh, s_inv_y)
                                      : scale(in->raw_y, in->ay_min, in->ay_max, sh, s_inv_y);
                    s_ptr_x = x; s_ptr_y = y;
                }
                if (s_ptr_x < 0) s_ptr_x = 0;
                if (s_ptr_y < 0) s_ptr_y = 0;
                if (s_ptr_x >= sw) s_ptr_x = sw - 1;
                if (s_ptr_y >= sh) s_ptr_y = sh - 1;
                if (in->down && !in->was_down)      app_pointer_down(s_ptr_x, s_ptr_y);
                else if (!in->down && in->was_down) app_pointer_up(s_ptr_x, s_ptr_y);
                else if (in->down && in->moved)     app_pointer_move(s_ptr_x, s_ptr_y);
                in->was_down = in->down;
                in->moved = false;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Signals                                                                    */
/* ------------------------------------------------------------------------- */
static char s_crash_path[HMI_PATH_MAX + 16];

static void on_stop_signal(int sig)
{
    (void)sig;
    app_stop();                                   /* one atomic store: async-signal-safe */
}

static void on_crash_signal(int sig)
{
    /* async-signal-safe only: open / write / close, then die with the default action */
    int fd = open(s_crash_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        char line[64] = "crash: signal ";
        size_t n = strlen(line);
        if (sig >= 10) line[n++] = (char)('0' + (sig / 10) % 10);
        line[n++] = (char)('0' + sig % 10);
        line[n++] = '\n';
        ssize_t w = write(fd, line, n);
        (void)w;
        close(fd);
    }
    if (s_tty >= 0) ioctl(s_tty, KDSETMODE, KD_TEXT);   /* give the console back */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_signals(void)
{
    char dir[HMI_PATH_MAX];
    hal_exe_dir(dir, sizeof(dir));
    snprintf(s_crash_path, sizeof(s_crash_path), "%scrash.log", dir);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);                /* a PLC closing its socket must not kill us */
    sa.sa_handler = on_crash_signal;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    static char *app_argv[64];
    int app_argc = 0;
    const char *fbdev = getenv("HMI_FBDEV") ? getenv("HMI_FBDEV") : "/dev/fb0";
    bool headless = false;

    for (int i = 0; i < argc && app_argc < 63; i++) {         /* take the shell's own options out */
        if (i > 0 && strcmp(argv[i], "--fb") == 0 && i + 1 < argc) { fbdev = argv[++i]; continue; }
        if (i > 0 && strcmp(argv[i], "--headless") == 0) { headless = true; continue; }
        app_argv[app_argc++] = argv[i];
    }
    app_argv[app_argc] = NULL;

    install_signals();
    int rc = app_parse_args(app_argc, app_argv);
    if (rc >= 0) return rc;
    rc = app_init();
    if (rc != 0) return rc;

    int sw = HMI_DISPLAY_WIDTH_DEFAULT, sh = HMI_DISPLAY_HEIGHT_DEFAULT;
    for (int i = 0; i < MAX_INPUTS; i++) s_in[i].fd = -1;     /* never fd 0 (stdin) by accident */
    if (!headless) {
        if (!fb_open(fbdev)) {
            fprintf(stderr, "industrial_hmi: no usable display; use --headless to run without one\n");
            fb_close();
            app_shutdown();
            return 1;
        }
        sw = s_fb.xres;
        sh = s_fb.yres;
        app_resize(sw, sh);
        s_swap_xy = env_on("HMI_TOUCH_SWAP_XY");
        s_inv_x = env_on("HMI_TOUCH_INVERT_X");
        s_inv_y = env_on("HMI_TOUCH_INVERT_Y");
        input_scan();
    }
    if (!app_start_workers()) { fb_close(); app_shutdown(); return 2; }

    uint32_t last_scan = hmi_timer_get_ms();
    while (app_running()) {
        uint32_t wait_ms = app_tick();
        if (!headless && g_repaint) {
            g_repaint = 0;
            app_render(present_band, NULL);
        }
        if (headless) {
            hmi_timer_sleep_ms(wait_ms ? wait_ms : 1);
            continue;
        }

        struct pollfd pf[MAX_INPUTS];
        int idx[MAX_INPUTS];
        int n = 0;
        for (int i = 0; i < MAX_INPUTS; i++) {
            if (s_in[i].fd < 0) continue;
            pf[n].fd = s_in[i].fd; pf[n].events = POLLIN; pf[n].revents = 0; idx[n++] = i;
        }
        if (n == 0) hmi_timer_sleep_ms(wait_ms ? wait_ms : 1);
        else if (poll(pf, (nfds_t)n, (int)wait_ms) > 0) {
            for (int k = 0; k < n; k++) {
                if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) input_close(&s_in[idx[k]]);
                else if (pf[k].revents & POLLIN) input_read(&s_in[idx[k]], sw, sh);
            }
        }
        if ((uint32_t)(hmi_timer_get_ms() - last_scan) >= 3000u) {   /* hot-plugged keyboards / touch panels */
            last_scan = hmi_timer_get_ms();
            input_scan();
        }
    }

    for (int i = 0; i < MAX_INPUTS; i++) if (s_in[i].fd >= 0) { close(s_in[i].fd); s_in[i].fd = -1; }
    app_shutdown();
    fb_close();
    return 0;
}
