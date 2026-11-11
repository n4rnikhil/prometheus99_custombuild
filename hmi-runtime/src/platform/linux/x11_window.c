/**
 * @file x11_window.c
 * @brief Desktop window for the Linux shell: X11 (and XWayland on Wayland desktops)
 *
 * libX11 is loaded at run time with dlopen(): the same industrial_hmi binary runs on a desktop
 * (a normal window, mouse and keyboard), on a bare panel (framebuffer) and on a server
 * (--headless). A machine without the X11 library simply never uses this file.
 * No heap: one band-sized image buffer, statically allocated.
 */

#include "x11_window.h"

#if defined(__has_include) && !defined(__CPPCHECK__)    /* cppcheck cannot evaluate __has_include */
#  if __has_include(<X11/Xlib.h>)
#    define HMI_HAVE_X11_HEADERS 1
#  endif
#endif

#ifdef HMI_HAVE_X11_HEADERS
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include "app.h"
#include "config.h"
#include "app_icon.h"
#include "renderer/layout.h"

/* --- libX11 entry points, resolved with dlsym --- */
#define X11_FUNCS(F) \
    F(Display *, XOpenDisplay, (const char *)) \
    F(int, XCloseDisplay, (Display *)) \
    F(Window, XCreateSimpleWindow, (Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long)) \
    F(int, XStoreName, (Display *, Window, const char *)) \
    F(int, XSelectInput, (Display *, Window, long)) \
    F(int, XMapWindow, (Display *, Window)) \
    F(GC, XCreateGC, (Display *, Drawable, unsigned long, XGCValues *)) \
    F(XImage *, XCreateImage, (Display *, Visual *, unsigned, int, int, char *, unsigned, unsigned, int, int)) \
    F(int, XPutImage, (Display *, Drawable, GC, XImage *, int, int, int, int, unsigned, unsigned)) \
    F(int, XPending, (Display *)) \
    F(int, XNextEvent, (Display *, XEvent *)) \
    F(int, XFlush, (Display *)) \
    F(Atom, XInternAtom, (Display *, const char *, Bool)) \
    F(Status, XSetWMProtocols, (Display *, Window, Atom *, int)) \
    F(KeySym, XLookupKeysym, (XKeyEvent *, int)) \
    F(int, XChangeProperty, (Display *, Window, Atom, Atom, int, int, const unsigned char *, int)) \
    F(void, XSetWMNormalHints, (Display *, Window, XSizeHints *)) \
    F(int, XSetClassHint, (Display *, Window, XClassHint *)) \
    F(int, XFree, (void *))

#define X11_PTR(ret, name, args) static ret (*p_##name) args;
X11_FUNCS(X11_PTR)

static void     *s_lib;
static Display  *s_dpy;
static Window    s_win;
static GC        s_gc;
static Visual   *s_vis;
static int       s_depth;
static Atom      s_wm_delete;
static int       s_w, s_h;
static int       s_rshift, s_gshift, s_bshift;
static bool      s_mouse_down;
static const char *s_why = "no desktop session ($DISPLAY is not set)";

/* one band at the widest supported frame: HMI_DISPLAY_MAX_BUF_SIZE bytes of 4-bpp = 2x pixels */
static uint32_t  s_px[HMI_DISPLAY_MAX_BUF_SIZE * 2];
static XImage   *s_img;
static int       s_img_w, s_img_h;

static int mask_shift(unsigned long m)
{
    int s = 0;
    if (!m) return 0;
    while (!(m & 1ul)) { m >>= 1; s++; }
    return s;
}

static bool load_x11(void)
{
    s_lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!s_lib) s_lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    if (!s_lib) return false;
#define X11_LOAD(ret, name, args) \
    *(void **)(&p_##name) = dlsym(s_lib, #name); \
    if (!p_##name) { dlclose(s_lib); s_lib = NULL; return false; }
    X11_FUNCS(X11_LOAD)
    return true;
}

bool x11_available(void)
{
    const char *d = getenv("DISPLAY");
    return d && d[0];
}

bool x11_open(int w, int h, const char *title)
{
    if (!x11_available()) return false;
    if (!load_x11()) { s_why = "libX11 is not installed (Debian/Ubuntu: apt install libx11-6)"; return false; }
    s_dpy = p_XOpenDisplay(NULL);
    if (!s_dpy) { s_why = "cannot connect to the X display in $DISPLAY"; dlclose(s_lib); s_lib = NULL; return false; }
    int scr = DefaultScreen(s_dpy);
    s_vis = DefaultVisual(s_dpy, scr);
    s_depth = DefaultDepth(s_dpy, scr);
    if (s_vis->class != TrueColor || (s_depth != 24 && s_depth != 32)) {
        s_why = "the X display is not 24/32-bit true colour";
        p_XCloseDisplay(s_dpy);
        s_dpy = NULL;
        dlclose(s_lib);
        s_lib = NULL;
        return false;
    }
    s_rshift = mask_shift(s_vis->red_mask);
    s_gshift = mask_shift(s_vis->green_mask);
    s_bshift = mask_shift(s_vis->blue_mask);

    s_w = w; s_h = h;
    s_win = p_XCreateSimpleWindow(s_dpy, RootWindow(s_dpy, scr), 0, 0, (unsigned)w, (unsigned)h, 0,
                                  BlackPixel(s_dpy, scr), BlackPixel(s_dpy, scr));
    p_XStoreName(s_dpy, s_win, title);
    Atom utf8 = p_XInternAtom(s_dpy, "UTF8_STRING", False);
    p_XChangeProperty(s_dpy, s_win, p_XInternAtom(s_dpy, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                      (const unsigned char *)title, (int)strlen(title));
    XClassHint ch;
    ch.res_name = (char *)"industrial_hmi";
    ch.res_class = (char *)"Prometheus-HMI";                 /* matches StartupWMClass in the .desktop file */
    p_XSetClassHint(s_dpy, s_win, &ch);

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = HMI_LAYOUT_MIN_W;
    hints.min_height = HMI_LAYOUT_MIN_H;
    p_XSetWMNormalHints(s_dpy, s_win, &hints);

    static unsigned long icon[2 + HMI_APP_ICON_W * HMI_APP_ICON_H_PX];   /* format 32 = C long */
    icon[0] = HMI_APP_ICON_W;
    icon[1] = HMI_APP_ICON_H_PX;
    for (int i = 0; i < HMI_APP_ICON_W * HMI_APP_ICON_H_PX; i++) icon[2 + i] = s_app_icon[i];
    p_XChangeProperty(s_dpy, s_win, p_XInternAtom(s_dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace,
                      (const unsigned char *)icon, 2 + HMI_APP_ICON_W * HMI_APP_ICON_H_PX);

    s_wm_delete = p_XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);
    p_XSetWMProtocols(s_dpy, s_win, &s_wm_delete, 1);
    p_XSelectInput(s_dpy, s_win, ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask |
                   ButtonReleaseMask | PointerMotionMask);
    s_gc = p_XCreateGC(s_dpy, s_win, 0, NULL);
    p_XMapWindow(s_dpy, s_win);
    p_XFlush(s_dpy);
    return true;
}

int x11_fd(void) { return s_dpy ? ConnectionNumber(s_dpy) : -1; }

void x11_size(int *w, int *h) { *w = s_w; *h = s_h; }

/* app_present_fn: convert one 4-bpp band to the display's pixel format and put it on screen */
void x11_present_band(void *ctx, const uint8_t *band, int y, int rows, int width, const uint32_t *pal)
{
    (void)ctx;
    if (!s_dpy || rows <= 0 || width <= 0) return;
    if ((size_t)width * (size_t)rows > sizeof(s_px) / sizeof(s_px[0])) return;
    uint32_t lut[16];
    for (int i = 0; i < 16; i++) {
        uint32_t c = pal[i];
        lut[i] = (((c >> 16) & 0xFFu) << s_rshift) | (((c >> 8) & 0xFFu) << s_gshift) | ((c & 0xFFu) << s_bshift);
    }
    int row_bytes = (width + 1) / 2;
    for (int r = 0; r < rows; r++) {
        const uint8_t *src = band + (size_t)r * (size_t)row_bytes;
        uint32_t *dst = s_px + (size_t)r * (size_t)width;
        for (int x = 0; x < width; x++) {
            uint8_t b = src[x >> 1];
            dst[x] = lut[(x & 1) ? (b & 0x0F) : (b >> 4)];
        }
    }
    if (!s_img || s_img_w != width || s_img_h != rows) {
        if (s_img) { s_img->data = NULL; XDestroyImage(s_img); }  /* our static buffer: never freed */
        s_img = p_XCreateImage(s_dpy, s_vis, (unsigned)s_depth, ZPixmap, 0, (char *)s_px, (unsigned)width,
                               (unsigned)rows, 32, 0);
        s_img_w = width; s_img_h = rows;
        if (!s_img) return;
    }
    p_XPutImage(s_dpy, s_win, s_gc, s_img, 0, 0, 0, y, (unsigned)width, (unsigned)rows);
}

void x11_flush(void) { if (s_dpy) p_XFlush(s_dpy); }

static uint16_t map_keysym(KeySym k)
{
    if (k >= XK_a && k <= XK_z) return (uint16_t)('A' + (k - XK_a));
    if (k >= XK_A && k <= XK_Z) return (uint16_t)('A' + (k - XK_A));
    if (k >= XK_0 && k <= XK_9) return (uint16_t)('0' + (k - XK_0));
    if (k >= XK_KP_0 && k <= XK_KP_9) return (uint16_t)('0' + (k - XK_KP_0));
    switch (k) {
        case XK_Tab: case XK_ISO_Left_Tab: return HMI_K_TAB;
        case XK_Left:      return HMI_K_LEFT;
        case XK_Right:     return HMI_K_RIGHT;
        case XK_Return: case XK_KP_Enter: return HMI_K_ENTER;
        case XK_Escape:    return HMI_K_ESC;
        case XK_BackSpace: return HMI_K_BACK;
        case XK_space:     return ' ';
        case XK_period: case XK_KP_Decimal: return '.';
        case XK_minus: case XK_KP_Subtract: return '-';
        default:           return 0;
    }
}

/* Drain pending X events into the app. Returns true when a repaint is needed. */
bool x11_events(void)
{
    bool repaint = false;
    while (s_dpy && p_XPending(s_dpy)) {
        XEvent e;
        p_XNextEvent(s_dpy, &e);
        switch (e.type) {
            case Expose:
                if (e.xexpose.count == 0) repaint = true;
                break;
            case ConfigureNotify:
                if (e.xconfigure.width != s_w || e.xconfigure.height != s_h) {
                    s_w = e.xconfigure.width;
                    s_h = e.xconfigure.height;
                    app_resize(s_w, s_h);
                    repaint = true;
                }
                break;
            case ButtonPress:
                if (e.xbutton.button == Button1) { s_mouse_down = true; app_pointer_down(e.xbutton.x, e.xbutton.y); }
                break;
            case MotionNotify:
                if (s_mouse_down) app_pointer_move(e.xmotion.x, e.xmotion.y);
                break;
            case ButtonRelease:
                if (e.xbutton.button == Button1 && s_mouse_down) {
                    s_mouse_down = false;
                    app_pointer_up(e.xbutton.x, e.xbutton.y);
                    repaint = true;
                }
                break;
            case KeyPress: {
                uint16_t k = map_keysym(p_XLookupKeysym(&e.xkey, 0));
                if (k) { app_key(k, (e.xkey.state & ShiftMask) != 0); repaint = true; }
                break;
            }
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == s_wm_delete) app_stop();
                break;
            default:
                break;
        }
    }
    return repaint;
}

void x11_close(void)
{
    if (s_img) { s_img->data = NULL; XDestroyImage(s_img); s_img = NULL; }
    if (s_dpy) { p_XCloseDisplay(s_dpy); s_dpy = NULL; }
    if (s_lib) { dlclose(s_lib); s_lib = NULL; }
}

const char *x11_failure(void) { return s_why; }

#else  /* no X11 headers at build time: desktop window not available in this build */
const char *x11_failure(void) { return "this program was built without the X11 headers (rebuild it with libx11-dev installed)"; }

bool x11_available(void) { return false; }
bool x11_open(int w, int h, const char *title) { (void)w; (void)h; (void)title; return false; }
int  x11_fd(void) { return -1; }
void x11_size(int *w, int *h) { *w = 0; *h = 0; }
void x11_present_band(void *ctx, const uint8_t *band, int y, int rows, int width, const uint32_t *pal)
{ (void)ctx; (void)band; (void)y; (void)rows; (void)width; (void)pal; }
void x11_flush(void) {}
bool x11_events(void) { return false; }
void x11_close(void) {}

#endif
