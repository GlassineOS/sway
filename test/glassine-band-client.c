/* glassine-band-client — a window with exact greys for the dead band's host test
 * (../../sway/band-test.sh). The window (tile it to fill the
 * output) has 5 bands, top to bottom, grey = r = g = b (the grey output's luma
 * of it is that byte), one new picture per frame callback:
 * E frame code, always moves by more than any T: tells which frame this is
 * A drift, +1 a frame
 * B flicker, +-3
 * C big steps, +9 a frame (wraps)
 * D still (damaged only when it changes)
 * After frame N-1 a pause (motion over), then frame N: all but D move as ever,
 * D +2, smaller than T >= 3. The values: band-check.py has the same formulas.
 * glassine-band-client N [PAUSE_MS] [g]
 * g: A, B and D as gradients across the window (+0..47 left to right, the
 * band's value at the left): what a dither has to work on (band-dots.py) */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define NBUF 4
#define NBAND 5

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_surface *surf;
static struct { struct wl_buffer *b; uint32_t *px; bool busy; } bufs[NBUF];
static int W = 1404, H = 1872, N, pause_ms = 1000, f;  /* (sway's first configure is 0 x 0: the panel's size) */
static bool configured, have_bufs, gradient;

static uint8_t band_value(int band, int fr)
{
    switch (band) {
    case 0: return (uint8_t)(128 + fr * 17 % 128);
    case 1: return (uint8_t)(40 + fr);
    case 2: return (uint8_t)(126 + 3 * (fr & 1));
    case 3: return (uint8_t)(20 + fr * 9 % 207);
    default: return fr >= N ? 202 : 200;
    }
}

static void released(void *data, struct wl_buffer *b) { (void)b; *(bool *)data = false; }
static const struct wl_buffer_listener buf_l = { released };

static void make_bufs(void)
{
    size_t one = (size_t)W * H * 4;
    int fd = memfd_create("glassine-band-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, one * NBUF)) { perror("memfd"); exit(1); }
    uint8_t *m = mmap(NULL, one * NBUF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    struct wl_shm_pool *p = wl_shm_create_pool(shm, fd, (int)(one * NBUF));
    for (int i = 0; i < NBUF; i++) {
        bufs[i].b = wl_shm_pool_create_buffer(p, (int)(i * one), W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
        bufs[i].px = (uint32_t *)(m + i * one);
        wl_buffer_add_listener(bufs[i].b, &buf_l, &bufs[i].busy);
    }
    wl_shm_pool_destroy(p);
    close(fd);
    have_bufs = true;
}

static void frame_cb(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_l = { frame_cb };

static void draw(void)
{
    int k = 0;
    while (k < NBUF && bufs[k].busy) k++;
    if (k == NBUF) { fprintf(stderr, "no free buffer\n"); exit(1); }
    for (int y = 0; y < H; y++) {
        int band = y * NBAND / H;
        uint32_t g0 = band_value(band, f);
        bool ramp = gradient && (band == 1 || band == 2 || band == 4);
        uint32_t *row = bufs[k].px + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint32_t g = g0 + (ramp ? (uint32_t)(x * 48 / W) : 0);
            row[x] = 0xff000000u | g << 16 | g << 8 | g;
        }
    }
    wl_surface_attach(surf, bufs[k].b, 0, 0);
    bufs[k].busy = true;
    for (int b = 0; b < NBAND; b++)     /* (damage: only what changes, the first frame all) */
        if (f == 0 || band_value(b, f) != band_value(b, f - 1))
            wl_surface_damage_buffer(surf, 0, b * H / NBAND, W, (b + 1) * H / NBAND - b * H / NBAND);
    if (f < N) wl_callback_add_listener(wl_surface_frame(surf), &frame_l, NULL);
    wl_surface_commit(surf);
}

static void frame_cb(void *d, struct wl_callback *cb, uint32_t t)
{
    (void)d; (void)t;
    wl_callback_destroy(cb);
    f++;
    if (f == N) usleep(pause_ms * 1000);        /* (the motion's over: the quiet gap) */
    draw();
}

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{
    (void)d; (void)t; (void)s;
    /* (the frames are judged at fixed places: another size would only mislead) */
    if (w > 0 && h > 0 && (w != W || h != H)) { fprintf(stderr, "configured %dx%d, not %dx%d\n", w, h, W, H); exit(1); }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_l = { .configure = top_configure, .close = top_close };

static void surf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    if (!have_bufs) make_bufs();
    if (!configured) { configured = true; draw(); }
}
static const struct xdg_surface_listener surf_l = { surf_configure };

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *i, uint32_t v)
{
    (void)d; (void)v;
    if (!strcmp(i, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    if (!strcmp(i, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    if (!strcmp(i, xdg_wm_base_interface.name)) {
        wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm, &wm_l, NULL);
    }
}
static void reg_gone(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_gone };

int main(int argc, char **argv)
{
    N = argc > 1 ? atoi(argv[1]) : 120;
    if (argc > 2) pause_ms = atoi(argv[2]);
    gradient = argc > 3 && argv[3][0] == 'g';
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "globals missing\n"); return 1; }
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &surf_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_app_id(top, "glassine-band-client");
    wl_surface_commit(surf);
    while (wl_display_dispatch(dpy) != -1) {}
    return 0;
}
