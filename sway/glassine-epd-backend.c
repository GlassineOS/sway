/* glassine-epd-backend.c — the Glassine e-ink panel as a wlroots 0.18 output, built into
 * the patched sway (tools/pmos-glassine/sway/). Protocol: glassine-epd-output-proto.h.
 *
 * One output, EPD-1 (portrait 1404x1872; GLASSINE_EPD_SIZE=WxH for host tests).
 * Buffer caps: SHM and DMABUF. With the pixman renderer sway's allocator makes
 * memfd buffers; with GLES (WLR_RENDERER=gles2) GBM buffers on the
 * render node, LINEAR XRGB8888/ARGB8888 only (the primary formats), which go to
 * glassine-epdd-desktop as DMABUF and are read on its GPU. Per commit:
 * - a buffer not yet known to glassine-epdd-desktop: BUF + its fd (once);
 * - FRAME with the commit's damage (buffer coordinates);
 * - the buffer stays locked (not reused by the swapchain) until glassine-epdd-desktop
 * answers DONE; then the output's frame event fires and sway may render
 * the next frame: apps draw at the panel's pace. NEXT fires
 * the frame event but keeps the buffer locked until its DONE: glassine-epdd-desktop
 * reads a frame only when it needs one (HELLO tells it we can).
 * Safety valves: no DONE within DONE_TIMEOUT_MS -> the frame event fires
 * anyway (a hung glassine-epdd-desktop must not freeze every app); at most MAXLOCK frames
 * stay locked (the oldest is given back); no glassine-epdd-desktop connected -> frames are
 * dropped and paced by a NOCONSUMER_MS timer; the socket is retried every
 * RETRY_MS, and a reconnect resends every buffer and the current picture.
 * The pointer is a hardware cursor here: wlroots hands over its
 * sprite (set_cursor) and position (move_cursor) instead of drawing it into
 * the frames; both go to glassine-epdd-desktop (CURSOR_IMG / CURSOR_POS), which stamps the
 * sprite into what it reads and paints it fast where it moves.
 * The gamma (GLASSINE_EPD_CAP_GAMMA; GLASSINE_EPD_GAMMA=0: off): on the grey
 * output glassine-epdd-desktop's gamma table is applied on the GPU, not by its CPU. It
 * cannot go into the compositing shaders (the curve is not linear: blended
 * edges and translucent pixels would be wrong), so each commit's buffer gets
 * a final pass over its damage (+ what our own target missed since it was
 * last drawn: a damage ring) into one of our own R8 buffers (a swapchain from
 * the output's allocator), and that one goes to glassine-epdd-desktop with the table's
 * tag in FRAME. sway's buffer is held until the next commit (a new table
 * redraws the whole picture from it), not until DONE. Any failure: sway's
 * buffer goes as before, tag 0, and glassine-epdd-desktop applies the table itself.
 * The dead band (GLASSINE_EPD_CAP_BAND, ; T 0: nothing of
 * this runs, no extra buffer held): in that pass a pixel keeps its accepted
 * grey while |new - accepted| < T. The accepted picture is simply our last
 * target (complete thanks to the damage ring; kept locked so the swapchain
 * cannot hand it out as the next target, read only while glassine-epdd-desktop may read it
 * in place too). A pixel the ring redraws without new damage stays what it
 * was: its source did not change, so the comparison gives the same answer.
 * Only damage since the last true picture can hold a banded grey (band.pend).
 * band.quiet_ms after sway was last free to draw (the frame event: while
 * glassine-epdd-desktop holds a frame the quiet is not the apps') and did not, that region
 * goes through the pass without the band, as a FRAME marked TRUE. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <pixman.h>
#include <drm_fourcc.h>
#include <wlr/backend/interface.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include "glassine-epd-backend.h"
#include "glassine-epd-output-proto.h"

#define MAXBUF 12                       /* sway's swapchain + ours (the gamma) */
#define MAXLOCK 3
#define DONE_TIMEOUT_MS 2000
#define NOCONSUMER_MS 100
#define RETRY_MS 1000

struct epd_backend;

struct epd_buf {
    struct wlr_buffer *b;               /* NULL: free slot */
    uint32_t id;
    bool sent;                          /* BUF went to the current connection */
    uint32_t tag;                       /* the gamma table it went through (0: none) */
    struct wl_listener destroy;
    struct epd_backend *be;
};

struct epd_locked { struct epd_buf *buf; uint32_t seq, commit_seq; };

struct epd_output {
    struct wlr_output base;
    struct epd_backend *be;
    struct wl_event_source *frame_timer;
    bool waiting;                       /* a frame is out: the frame event is pending */
    struct epd_buf *front;              /* the last committed buffer (resent on reconnect) */
};

struct epd_backend {
    struct wlr_backend base;
    struct wl_event_loop *loop;
    bool started;
    int w, h;
    struct epd_output *out;
    int fd;
    struct wl_event_source *sock_ev, *retry;
    struct epd_buf bufs[MAXBUF];
    uint32_t next_id, seq;
    struct epd_locked locked[MAXLOCK + 1];
    int nlocked;
    struct wlr_drm_format_set fmt_dmabuf, fmt_shm;     /* the primary formats */
    struct wlr_drm_format_set fmt_cursor;              /* ARGB8888 LINEAR (dma-buf cursor buffers) */
    bool grey;                          /* R8 frames in panel order */
    enum wl_output_transform transform; /* grey: what sway sets on the output */
    struct {                            /* the gamma on the GPU (GLASSINE_EPD_CAP_GAMMA) */
        struct wlr_renderer *r;         /* GLES (grey output only) */
        bool on;                        /* offered (HELLO) */
        uint32_t tag;                   /* the table's (0: none yet) */
        struct wlr_swapchain *sc;       /* our R8 targets */
        struct wlr_damage_ring ring;
        struct wlr_buffer *src;         /* sway's last committed buffer, held */
        bool failed;                    /* (logged once) */
    } lut;
    struct {                            /* the dead band (GLASSINE_EPD_CAP_BAND; lut.on only) */
        int t;                          /* 0: off */
        int quiet_ms;
        struct wlr_buffer *acc;         /* the accepted picture: our last target, held (t > 0) */
        pixman_region32_t pend;         /* damage since the last true picture */
        struct wl_event_source *timer;
    } band;
    struct {                            /* the pointer: a copy of wlroots' sprite in a memfd */
        int fd;                         /* -1: none yet */
        uint8_t *map;
        int slot;                       /* the slot the current sprite is in */
        int w, h, hx, hy;               /* w 0: no image */
        int x, y;                       /* the hotspot, buffer coordinates */
        bool fd_sent;                   /* the memfd went to the current connection */
    } cur;
};

static const struct wlr_backend_impl backend_impl;
static const struct wlr_output_impl output_impl;

/* ---- presentation ----------------------------------------------------------- */
struct present_later { struct wlr_output *out; uint32_t commit_seq; bool presented; };

static void present_idle(void *data)
{
    struct present_later *p = data;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct wlr_output_event_present ev = {
        .commit_seq = p->commit_seq, .presented = p->presented, .when = p->presented ? &now : NULL,
        /* the nominal frame interval (the mode's refresh): Chromium times its
         * next frame from it, and with 0 it stopped after the first raster tile
         * until an input came (WAYLAND_DEBUG) */
        .refresh = p->out->refresh > 0 ? (int)(1000000000000LL / p->out->refresh) : 0,
    };
    wlr_output_send_present(p->out, &ev);
    free(p);
}

/* never from inside commit: wlroots wants the present after the commit returned */
static void present(struct epd_backend *be, uint32_t commit_seq, bool presented)
{
    if (!be->out) return;
    struct present_later *p = malloc(sizeof *p);
    if (!p) return;
    *p = (struct present_later){ &be->out->base, commit_seq, presented };
    if (!wl_event_loop_add_idle(be->loop, present_idle, p)) free(p);
}

/* ---- the socket ----------------------------------------------------------------- */
static void disconnect(struct epd_backend *be, const char *why);
static void frame_now(struct epd_backend *be);

static bool send_msg(struct epd_backend *be, const struct glassine_epd_msg *m, int fd)
{
    if (be->fd < 0) return false;
    size_t nr = m->type == GLASSINE_EPD_FRAME ? m->nrect : m->type == GLASSINE_EPD_CURSOR_IMG || m->type == GLASSINE_EPD_CURSOR_POS;
    size_t len = offsetof(struct glassine_epd_msg, rect) + nr * sizeof m->rect[0];
    struct iovec iov = { (void *)m, len };
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } ctl;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        memset(&ctl, 0, sizeof ctl);
        mh.msg_control = ctl.b;
        mh.msg_controllen = sizeof ctl.b;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    if (sendmsg(be->fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)len) return true;
    if (errno == EAGAIN) {
        if (m->type != GLASSINE_EPD_CURSOR_POS)  /* (the mouse's 125 Hz: the next move resends it) */
            wlr_log(WLR_INFO, "glassine-epd: glassine-epdd-desktop not reading, message %u dropped", m->type);
        return false;
    }
    disconnect(be, strerror(errno));
    return false;
}

static bool send_buf(struct epd_backend *be, struct epd_buf *eb)
{
    struct wlr_shm_attributes a;
    struct wlr_dmabuf_attributes d;
    if (wlr_buffer_get_dmabuf(eb->b, &d) && d.n_planes == 1) {
        /* the modifier in seq/nrect: see glassine-epd-output-proto.h */
        struct glassine_epd_msg m = { .type = GLASSINE_EPD_DMABUF, .id = eb->id, .width = d.width, .height = d.height,
                                 .stride = d.stride[0], .format = d.format, .offset = d.offset[0],
                                 .seq = (uint32_t)d.modifier, .nrect = (uint32_t)(d.modifier >> 32) };
        eb->sent = send_msg(be, &m, d.fd[0]);
        return eb->sent;
    }
    if (!wlr_buffer_get_shm(eb->b, &a)) {
        wlr_log(WLR_ERROR, "glassine-epd: buffer %u is neither shm nor a single-plane dma-buf", eb->id);
        return false;
    }
    struct glassine_epd_msg m = { .type = GLASSINE_EPD_BUF, .id = eb->id, .width = a.width, .height = a.height,
                             .stride = a.stride, .format = a.format, .offset = (uint32_t)a.offset };
    eb->sent = send_msg(be, &m, a.fd);
    return eb->sent;
}

static void unlock_at(struct epd_backend *be, int i, bool presented)
{
    struct epd_locked l = be->locked[i];
    memmove(&be->locked[i], &be->locked[i + 1], (be->nlocked - i - 1) * sizeof *be->locked);
    be->nlocked--;
    present(be, l.commit_seq, presented);
    wlr_buffer_unlock(l.buf->b);        /* may destroy it: DROP via its destroy listener */
}

static void lut_set(struct epd_backend *be, uint32_t tag, const uint8_t *lut);
static void band_set(struct epd_backend *be, int t, int quiet_ms);

/* a frame to glassine-epdd-desktop; nrect 0 = the whole buffer. Locks the buffer until DONE.
 * flags: FRAME's offset (GLASSINE_EPD_FRAME_TRUE). false: not sent (its present
 * event is sent as not presented) */
static bool send_frame(struct epd_backend *be, struct epd_buf *eb, const struct glassine_epd_rect *r, int n,
                       uint32_t commit_seq, uint32_t flags)
{
    if (be->fd < 0 || (!eb->sent && !send_buf(be, eb))) { present(be, commit_seq, false); return false; }
    struct glassine_epd_msg m = { .type = GLASSINE_EPD_FRAME, .id = eb->id, .seq = ++be->seq, .nrect = n, .format = eb->tag, .offset = flags };
    memcpy(m.rect, r, n * sizeof *r);
    if (!send_msg(be, &m, -1)) { present(be, commit_seq, false); return false; }
    wlr_buffer_lock(eb->b);
    be->locked[be->nlocked++] = (struct epd_locked){ eb, m.seq, commit_seq };
    if (be->nlocked > MAXLOCK) {
        wlr_log(WLR_INFO, "glassine-epd: %d frames unanswered, giving the oldest back", be->nlocked);
        unlock_at(be, 0, false);
    }
    return true;
}

static int on_sock(int fd, uint32_t mask, void *data)
{
    struct epd_backend *be = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { disconnect(be, "hangup"); return 0; }
    struct glassine_epd_msg m;
    ssize_t n;
    while ((n = recv(fd, &m, sizeof m, MSG_DONTWAIT)) > 0) {
        if ((size_t)n < offsetof(struct glassine_epd_msg, rect)) continue;
        if (m.type == GLASSINE_EPD_NEXT) {   /* draw the next frame; this buffer stays locked until its DONE */
            if (m.seq == be->seq) frame_now(be);
            continue;
        }
        if (m.type == GLASSINE_EPD_LUT) {
            if ((size_t)n >= offsetof(struct glassine_epd_msg, rect) + 256) lut_set(be, m.id, (const uint8_t *)m.rect);
            continue;
        }
        if (m.type == GLASSINE_EPD_BAND) {
            band_set(be, (int)m.seq, (int)m.width);
            continue;
        }
        if (m.type != GLASSINE_EPD_DONE) continue;
        for (int i = 0; i < be->nlocked; i++)
            if (be->locked[i].seq == m.seq) { unlock_at(be, i, true); break; }
        if (m.seq == be->seq) frame_now(be);            /* the newest frame is on the panel */
    }
    if (n == 0) disconnect(be, "closed");
    return 0;
}

static int retry_connect(void *data);
static void cursor_send(struct epd_backend *be, bool img);
static bool lut_frame_whole(struct epd_backend *be);

static void try_connect(struct epd_backend *be)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    const char *path = getenv("GLASSINE_EPD_SOCK");
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path && *path ? path : GLASSINE_EPD_SOCK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
        if (fd >= 0) close(fd);
        wl_event_source_timer_update(be->retry, RETRY_MS);
        return;
    }
    be->fd = fd;
    be->sock_ev = wl_event_loop_add_fd(be->loop, fd, WL_EVENT_READABLE, on_sock, be);
    wlr_log(WLR_INFO, "glassine-epd: connected to glassine-epdd-desktop (%s)", sa.sun_path);
    /* (an older glassine-epdd-desktop ignores it); LUM: wlroots' grey shader writes luminance (GLASSINE_EPD_GREY_LUM, 0003) */
    const char *lum = getenv("GLASSINE_EPD_GREY_LUM");
    struct glassine_epd_msg hello = { .type = GLASSINE_EPD_HELLO,
        .seq = GLASSINE_EPD_CAP_NEXT | (be->grey && lum && *lum && strcmp(lum, "0") ? GLASSINE_EPD_CAP_LUM : 0) |
               (be->lut.on ? GLASSINE_EPD_CAP_GAMMA | GLASSINE_EPD_CAP_BAND : 0) };
    send_msg(be, &hello, -1);
    for (int i = 0; i < MAXBUF; i++) if (be->bufs[i].b) send_buf(be, &be->bufs[i]);
    be->cur.fd_sent = false;
    if (be->cur.fd >= 0) { cursor_send(be, true); cursor_send(be, false); }    /* the pointer as it is */
    /* (banded greys left in our last target: the true picture instead) */
    if (pixman_region32_not_empty(&be->band.pend) && lut_frame_whole(be)) return;
    if (be->out && be->out->front && be->out->base.enabled &&   /* the current picture, whole */
        send_frame(be, be->out->front, NULL, 0, be->out->base.commit_seq, 0)) {
        be->out->waiting = true;
        wl_event_source_timer_update(be->out->frame_timer, DONE_TIMEOUT_MS);
    }
}

static int retry_connect(void *data)
{
    struct epd_backend *be = data;
    if (be->fd < 0) try_connect(be);
    return 0;
}

static void disconnect(struct epd_backend *be, const char *why)
{
    if (be->fd < 0) return;
    wlr_log(WLR_INFO, "glassine-epd: glassine-epdd-desktop gone (%s)", why);
    if (be->sock_ev) wl_event_source_remove(be->sock_ev);
    be->sock_ev = NULL;
    close(be->fd);
    be->fd = -1;
    while (be->nlocked) unlock_at(be, 0, false);
    for (int i = 0; i < MAXBUF; i++) be->bufs[i].sent = false;
    band_set(be, 0, 0);                 /* (the next glassine-epdd-desktop says what it wants; band.pend stays) */
    if (be->out && be->out->waiting) wl_event_source_timer_update(be->out->frame_timer, NOCONSUMER_MS);
    wl_event_source_timer_update(be->retry, RETRY_MS);
}

/* ---- buffers ---------------------------------------------------------------------- */
static void buf_destroyed(struct wl_listener *l, void *data)
{
    (void)data;
    struct epd_buf *eb = wl_container_of(l, eb, destroy);
    struct epd_backend *be = eb->be;
    if (eb->sent) send_msg(be, &(struct glassine_epd_msg){ .type = GLASSINE_EPD_DROP, .id = eb->id }, -1);
    if (be->out && be->out->front == eb) be->out->front = NULL;
    wl_list_remove(&eb->destroy.link);
    eb->b = NULL;
    eb->sent = false;
}

static struct epd_buf *track(struct epd_backend *be, struct wlr_buffer *b)
{
    struct epd_buf *free_slot = NULL;
    for (int i = 0; i < MAXBUF; i++) {
        if (be->bufs[i].b == b) return &be->bufs[i];
        if (!be->bufs[i].b && !free_slot) free_slot = &be->bufs[i];
    }
    if (!free_slot) { wlr_log(WLR_ERROR, "glassine-epd: more than %d buffers", MAXBUF); return NULL; }
    *free_slot = (struct epd_buf){ .b = b, .id = ++be->next_id, .be = be };
    free_slot->destroy.notify = buf_destroyed;
    wl_signal_add(&b->events.destroy, &free_slot->destroy);
    return free_slot;
}

/* ---- the gamma on the GPU ------------------------------------------------------------ */

/* the table from glassine-epdd-desktop: from the next frame on; the whole picture again now */
static void lut_set(struct epd_backend *be, uint32_t tag, const uint8_t *lut)
{
    if (!be->lut.on || !tag || tag == be->lut.tag) return;
    if (!wlr_gles2_renderer_glassine_set_lut(be->lut.r, lut)) {
        wlr_log(WLR_ERROR, "glassine-epd: gamma table: the GL upload failed, glassine-epdd-desktop applies it");
        be->lut.tag = 0;
        return;
    }
    be->lut.tag = tag;
    wlr_damage_ring_add_whole(&be->lut.ring);
    lut_frame_whole(be);
}

/* the accepted picture is now b (or none); t 0: none is ever held */
static void band_accept(struct epd_backend *be, struct wlr_buffer *b)
{
    if (b == be->band.acc) return;
    if (b) wlr_buffer_lock(b);
    if (be->band.acc) wlr_buffer_unlock(be->band.acc);
    be->band.acc = b;
}

/* src (sway's buffer) through the table into one of our targets, over damage
 * + what that target missed; banded: with the dead band, if on. Returns it
 * still acquired (the caller unlocks it after send_frame); NULL: not possible
 * (the caller sends src as it is) */
static struct epd_buf *lut_pass(struct epd_backend *be, struct wlr_buffer *src, const pixman_region32_t *damage, bool banded)
{
    struct wlr_output *wo = be->out ? &be->out->base : NULL;
    if (!be->lut.tag || !wo || !wo->allocator) return NULL;
    if (!be->lut.sc) {
        const struct wlr_drm_format *f = wlr_drm_format_set_get(&be->fmt_dmabuf, DRM_FORMAT_R8);
        be->lut.sc = f ? wlr_swapchain_create(wo->allocator, be->w, be->h, f) : NULL;
        if (!be->lut.sc) goto fail;
    }
    struct wlr_buffer *dst = wlr_swapchain_acquire(be->lut.sc, NULL);
    if (!dst) goto fail;
    struct wlr_texture *t = wlr_texture_from_buffer(be->lut.r, src);
    struct epd_buf *eb = t ? track(be, dst) : NULL;
    if (!eb) {
        if (t) wlr_texture_destroy(t);
        wlr_buffer_unlock(dst);
        goto fail;
    }
    pixman_region32_t reg;
    pixman_region32_init(&reg);
    wlr_damage_ring_add(&be->lut.ring, damage);
    wlr_damage_ring_rotate_buffer(&be->lut.ring, dst, &reg);
    bool ok = false;
    /* (no accepted picture yet: this one draws plain and becomes it) */
    struct wlr_texture *at = banded && be->band.t > 0 && be->band.acc ? wlr_texture_from_buffer(be->lut.r, be->band.acc) : NULL;
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(be->lut.r, dst, NULL);
    if (pass) {
        wlr_gles2_renderer_glassine_lut_pass(be->lut.r, true);
        if (at) wlr_gles2_renderer_glassine_band_pass(be->lut.r, at, be->band.t);
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture = t, .clip = &reg, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            .filter_mode = WLR_SCALE_FILTER_NEAREST });
        wlr_gles2_renderer_glassine_band_pass(be->lut.r, NULL, 0);
        wlr_gles2_renderer_glassine_lut_pass(be->lut.r, false);
        ok = wlr_render_pass_submit(pass);
    }
    pixman_region32_fini(&reg);
    if (at) wlr_texture_destroy(at);
    wlr_texture_destroy(t);
    if (!ok) {
        wlr_damage_ring_add_whole(&be->lut.ring);     /* (that target is now unknown) */
        wlr_buffer_unlock(dst);
        goto fail;
    }
    eb->tag = be->lut.tag;
    if (be->band.t > 0) band_accept(be, dst);
    return eb;
fail:
    if (!be->lut.failed) wlr_log(WLR_ERROR, "glassine-epd: the gamma pass failed: sway's frames go as they are");
    be->lut.failed = true;
    if (be->band.t > 0) {               /* the accepted picture misses this frame: start over */
        band_accept(be, NULL);
        wlr_damage_ring_add_whole(&be->lut.ring);
    }
    return NULL;
}

/* sway's buffer, held until the next commit (a new table redraws from it) */
static void lut_hold(struct epd_backend *be, struct wlr_buffer *src)
{
    if (src == be->lut.src) return;
    if (src) wlr_buffer_lock(src);
    if (be->lut.src) wlr_buffer_unlock(be->lut.src);
    be->lut.src = src;
}

/* the whole picture through the table (true: no band); false: not possible */
static bool lut_frame_whole(struct epd_backend *be)
{
    struct epd_output *o = be->out;
    if (!o || !o->base.enabled || !be->lut.src || be->fd < 0) return false;
    pixman_region32_t all;
    pixman_region32_init_rect(&all, 0, 0, be->w, be->h);
    struct epd_buf *eb = lut_pass(be, be->lut.src, &all, false);
    pixman_region32_fini(&all);
    if (!eb) return false;
    pixman_region32_clear(&be->band.pend);
    o->front = eb;
    if (send_frame(be, eb, NULL, 0, o->base.commit_seq, 0)) {
        o->waiting = true;
        wl_event_source_timer_update(o->frame_timer, DONE_TIMEOUT_MS);
    }
    wlr_buffer_unlock(eb->b);
    return true;
}

/* a region as FRAME rects (more than fit: the bounding box) */
static int region_rects(const pixman_region32_t *reg, struct glassine_epd_rect *r)
{
    int nr, n = 0;
    const pixman_box32_t *b = pixman_region32_rectangles((pixman_region32_t *)reg, &nr);
    if (nr > GLASSINE_EPD_MAXRECT) {
        const pixman_box32_t *e = pixman_region32_extents((pixman_region32_t *)reg);
        r[n++] = (struct glassine_epd_rect){ e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1 };
    } else
        for (int i = 0; i < nr; i++)
            r[n++] = (struct glassine_epd_rect){ b[i].x1, b[i].y1, b[i].x2 - b[i].x1, b[i].y2 - b[i].y1 };
    return n;
}

/* ---- the dead band -------------------------------------------------------------------- */
/* the frame event went out (or T changed): the quiet starts */
static void band_arm(struct epd_backend *be, int ms)
{
    if (be->band.timer && pixman_region32_not_empty(&be->band.pend))
        wl_event_source_timer_update(be->band.timer, ms > 0 ? ms : 1);
}

/* the quiet is over: band.pend without the band, as a true frame. While a
 * frame is out, not now (its answer's frame event arms the quiet again) */
static int band_quiet(void *data)
{
    struct epd_backend *be = data;
    struct epd_output *o = be->out;
    if (!o || o->waiting || !o->base.enabled || !be->lut.src || be->fd < 0 || !be->lut.tag ||
        !pixman_region32_not_empty(&be->band.pend)) return 0;
    struct epd_buf *eb = lut_pass(be, be->lut.src, &be->band.pend, false);
    if (!eb) return 0;
    struct glassine_epd_rect r[GLASSINE_EPD_MAXRECT];
    int n = region_rects(&be->band.pend, r);
    o->front = eb;
    if (send_frame(be, eb, r, n, o->base.commit_seq, GLASSINE_EPD_FRAME_TRUE)) {
        pixman_region32_clear(&be->band.pend);
        o->waiting = true;
        wl_event_source_timer_update(o->frame_timer, DONE_TIMEOUT_MS);
    } else band_arm(be, be->band.quiet_ms);
    wlr_buffer_unlock(eb->b);
    return 0;
}

/* T from glassine-epdd-desktop. A new T starts over: the accepted picture is let go (the
 * next frame draws plain and becomes it, T 0: nothing is held any more), and
 * what the old T may have kept comes true at the next quiet (at once if sway
 * is not drawing) */
static void band_set(struct epd_backend *be, int t, int quiet_ms)
{
    if (!be->lut.on) return;
    t = t < 0 ? 0 : t > GLASSINE_EPD_BAND_MAX ? GLASSINE_EPD_BAND_MAX : t;
    be->band.quiet_ms = quiet_ms > 0 ? quiet_ms : 125;
    if (t == be->band.t) return;
    wlr_log(WLR_INFO, "glassine-epd: dead band T %d, quiet %d ms", t, be->band.quiet_ms);
    be->band.t = t;
    band_accept(be, NULL);
    band_arm(be, 1);
}

/* ---- the output --------------------------------------------------------------------- */
static const uint32_t SUPPORTED = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
                                  WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;

static void frame_now(struct epd_backend *be)
{
    struct epd_output *o = be->out;
    if (!o || !o->waiting) return;
    o->waiting = false;
    wl_event_source_timer_update(o->frame_timer, 0);
    band_arm(be, be->band.quiet_ms);
    wlr_output_send_frame(&o->base);
}

static int frame_timeout(void *data)
{
    struct epd_output *o = data;
    if (o->waiting && o->be->fd >= 0) wlr_log(WLR_INFO, "glassine-epd: no DONE in %d ms", DONE_TIMEOUT_MS);
    frame_now(o->be);
    return 0;
}

/* a dma-buf glassine-epdd-desktop can import: single plane, LINEAR, XRGB8888/ARGB8888, the
 * panel's size (as for shm, glassine-epdd-desktop refuses anything else; a client buffer
 * offered for direct scan-out passes only if it is exactly that). Not "one of
 * the output's swapchain buffers": wlroots tests a configuration with buffers
 * of a swapchain it has not installed yet */
static bool importable_dmabuf(struct epd_backend *be, struct wlr_buffer *b)
{
    struct wlr_dmabuf_attributes d;
    return wlr_buffer_get_dmabuf(b, &d) && d.n_planes == 1 && d.modifier == DRM_FORMAT_MOD_LINEAR &&
           (be->grey ? d.format == DRM_FORMAT_R8 : d.format == DRM_FORMAT_XRGB8888 || d.format == DRM_FORMAT_ARGB8888) &&
           d.width == be->w && d.height == be->h;
}

static const struct wlr_drm_format_set *output_formats(struct wlr_output *wo, uint32_t caps)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    if (caps & WLR_BUFFER_CAP_DMABUF) return &o->be->fmt_dmabuf;
    if (caps & WLR_BUFFER_CAP_SHM) return o->be->grey ? NULL : &o->be->fmt_shm;
    return NULL;
}

/* the cursor buffer: ARGB (the grey output's primary formats are R8 only: no
 * alpha) and, as a dma-buf, LINEAR (a GPU-tiled one may import external-only,
 * which the renderer cannot read back) */
static const struct wlr_drm_format_set *output_cursor_formats(struct wlr_output *wo, uint32_t caps)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    if (caps & WLR_BUFFER_CAP_DMABUF) return &o->be->fmt_cursor;
    if (caps & WLR_BUFFER_CAP_SHM) return &o->be->fmt_shm;
    return NULL;
}

static bool output_test(struct wlr_output *wo, const struct wlr_output_state *st)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    uint32_t unsupported = st->committed & ~SUPPORTED;
    if (unsupported) {
        wlr_log(WLR_DEBUG, "glassine-epd: unsupported output state 0x%x", unsupported);
        return false;
    }
    /* only buffers glassine-epdd-desktop can map: a client buffer offered for direct
     * scan-out (a fullscreen window) is refused, and the scene composites it
     * into one of ours instead */
    struct wlr_shm_attributes a;
    if (st->committed & WLR_OUTPUT_STATE_BUFFER && !wlr_buffer_get_shm(st->buffer, &a) && !importable_dmabuf(o->be, st->buffer))
        return false;
    if (st->committed & WLR_OUTPUT_STATE_MODE &&
        (st->mode_type != WLR_OUTPUT_STATE_MODE_CUSTOM ||
         st->custom_mode.width != o->be->w || st->custom_mode.height != o->be->h)) {
        wlr_log(WLR_DEBUG, "glassine-epd: only %dx%d", o->be->w, o->be->h);
        return false;
    }
    return true;
}

static bool output_commit(struct wlr_output *wo, const struct wlr_output_state *st)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    struct epd_backend *be = o->be;
    if (!output_test(wo, st)) return false;
    bool enabled = st->committed & WLR_OUTPUT_STATE_ENABLED ? st->enabled : wo->enabled;
    if (!enabled) {
        o->waiting = false;
        wl_event_source_timer_update(o->frame_timer, 0);
        return true;
    }
    if (!(st->committed & WLR_OUTPUT_STATE_BUFFER)) return true;
    uint32_t commit_seq = wo->commit_seq + 1;
    if (be->lut.on) lut_hold(be, st->buffer);
    struct epd_buf *eb = NULL, *lb = NULL;
    if (be->lut.on && be->lut.tag && !(st->committed & WLR_OUTPUT_STATE_DAMAGE &&
                                       !pixman_region32_not_empty((pixman_region32_t *)&st->damage))) {
        pixman_region32_t all;
        pixman_region32_init_rect(&all, 0, 0, be->w, be->h);
        /* the band compares with what is on the panel: not across a new
         * transform or size of the desktop (that frame draws plain) */
        if (st->committed & (WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED))
            band_accept(be, NULL);
        const pixman_region32_t *dmg = st->committed & WLR_OUTPUT_STATE_DAMAGE ? &st->damage : &all;
        eb = lb = lut_pass(be, st->buffer, dmg, true);
        if (be->band.t > 0) {           /* (a timer of the last gap: this frame ends it, frame_now arms the next) */
            pixman_region32_union(&be->band.pend, &be->band.pend, dmg);
            wl_event_source_timer_update(be->band.timer, 0);
        }
        pixman_region32_fini(&all);
    }
    if (!eb) eb = track(be, st->buffer);
    if (!eb) { present(be, commit_seq, false); return true; }
    if (!lb) eb->tag = 0;
    o->front = eb;
    struct glassine_epd_rect r[GLASSINE_EPD_MAXRECT];
    int n = 0;
    if (st->committed & WLR_OUTPUT_STATE_DAMAGE) {
        if (!pixman_region32_not_empty((pixman_region32_t *)&st->damage)) {     /* nothing changed: no frame for glassine-epdd-desktop */
            present(be, commit_seq, true);
            o->waiting = true;
            wl_event_source_timer_update(o->frame_timer, 1);
            return true;
        }
        n = region_rects(&st->damage, r);
    }
    o->waiting = true;
    bool sent = be->fd >= 0 ? send_frame(be, eb, r, n, commit_seq, 0) : (present(be, commit_seq, false), false);
    if (lb) wlr_buffer_unlock(lb->b);   /* (our acquire: locked by send_frame until DONE, or free again) */
    wl_event_source_timer_update(o->frame_timer, sent ? DONE_TIMEOUT_MS : NOCONSUMER_MS);
    return true;
}

static void output_destroy(struct wlr_output *wo)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    wl_event_source_remove(o->frame_timer);
    o->be->out = NULL;
    free(o);
}

/* the pointer: a "hardware cursor" wlroots never draws into the frames (a
 * pointer drawn by sway's renderer would be part of every frame's damage).
 * Its sprite and position go to glassine-epdd-desktop, which paints it itself (* before: shown nowhere, as glassine-epdd-desktop's XFixesHideCursor on X).
 * GLASSINE_EPD_CURSOR=1: drawn into the frames (wlroots' software cursor). */
static void cursor_send(struct epd_backend *be, bool img)
{
    if (be->fd < 0 || be->cur.fd < 0) return;
    struct glassine_epd_msg m = { .type = img ? GLASSINE_EPD_CURSOR_IMG : GLASSINE_EPD_CURSOR_POS };
    if (img) {
        m.width = be->cur.w; m.height = be->cur.h; m.stride = be->cur.w * 4;
        m.offset = be->cur.slot * GLASSINE_EPD_CUR_SLOT;
        m.format = be->grey ? GLASSINE_EPD_CUR_PANEL : GLASSINE_EPD_CUR_PORTRAIT;
        m.rect[0] = (struct glassine_epd_rect){ be->cur.hx, be->cur.hy, 0, 0 };
    } else
        m.rect[0] = (struct glassine_epd_rect){ be->cur.x, be->cur.y, 0, 0 };
    bool fd = img && !be->cur.fd_sent;
    if (send_msg(be, &m, fd ? be->cur.fd : -1) && fd) be->cur.fd_sent = true;
}

/* the sprite's pixels as ARGB8888 into dst (stride w * 4): the buffer's own
 * memory where it has one (shm: the pixman renderer), else read back through
 * the renderer (GBM buffers have no CPU access in wlroots 0.18: GLES) — as
 * ABGR8888, which GLES always reads (BGRA needs EXT_read_format_bgra), swapped */
static bool cursor_pixels(struct wlr_output *wo, struct wlr_buffer *buf, uint32_t *dst)
{
    int w = buf->width, h = buf->height;
    void *data;
    uint32_t fmt;
    size_t stride;
    if (wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
        bool ok = fmt == DRM_FORMAT_ARGB8888;
        for (int r = 0; ok && r < h; r++) memcpy(dst + (size_t)r * w, (uint8_t *)data + r * stride, (size_t)w * 4);
        wlr_buffer_end_data_ptr_access(buf);
        if (ok) return true;
    }
    struct wlr_texture *t = wo->renderer ? wlr_texture_from_buffer(wo->renderer, buf) : NULL;
    if (!t) return false;
    bool ok = wlr_texture_read_pixels(t, &(struct wlr_texture_read_pixels_options){
        .data = dst, .format = DRM_FORMAT_ABGR8888, .stride = w * 4 });
    wlr_texture_destroy(t);
    for (size_t i = 0; ok && i < (size_t)w * h; i++) {
        uint32_t p = dst[i];                    /* R G B A bytes -> B G R A (ARGB8888 little endian) */
        dst[i] = (p & 0xff00ff00u) | (p & 0xffu) << 16 | (p >> 16 & 0xffu);
    }
    return ok;
}

/* wlroots unlocks buf right after this returns: the pixels are copied (into
 * the slot glassine-epdd-desktop is not told to read now). Hotspot: buffer space already
 * (output_cursor_attempt_hardware turns it with the output transform). Always
 * true: false would make wlroots fall back to its software cursor = drawn
 * into every frame; a sprite we cannot copy is sent as no image */
static bool output_set_cursor(struct wlr_output *wo, struct wlr_buffer *buf, int hx, int hy)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    struct epd_backend *be = o->be;
    if (be->cur.fd < 0) {
        int fd = memfd_create("glassine-epd-cursor", MFD_CLOEXEC);
        void *m = fd < 0 || ftruncate(fd, 2 * GLASSINE_EPD_CUR_SLOT) ? MAP_FAILED :
                  mmap(NULL, 2 * GLASSINE_EPD_CUR_SLOT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            wlr_log(WLR_ERROR, "glassine-epd: cursor memfd: %s", strerror(errno));
            if (fd >= 0) close(fd);
            return true;
        }
        be->cur.fd = fd;
        be->cur.map = m;
        be->cur.slot = 1;
    }
    int slot = !be->cur.slot;
    be->cur.w = be->cur.h = 0;
    if (buf && (buf->width > GLASSINE_EPD_CUR_MAX || buf->height > GLASSINE_EPD_CUR_MAX))
        wlr_log(WLR_INFO, "glassine-epd: cursor %dx%d is larger than %d px: not shown", buf->width, buf->height, GLASSINE_EPD_CUR_MAX);
    else if (buf && cursor_pixels(wo, buf, (uint32_t *)(be->cur.map + slot * GLASSINE_EPD_CUR_SLOT))) {
        be->cur.w = buf->width; be->cur.h = buf->height;
        be->cur.slot = slot;
    } else if (buf)
        wlr_log(WLR_INFO, "glassine-epd: cursor %dx%d: pixels not readable, not shown", buf->width, buf->height);
    be->cur.hx = hx; be->cur.hy = hy;      /* (cur.x/y is the hotspot's place: unchanged) */
    cursor_send(be, true);
    return true;
}

/* x, y: the hotspot in output-local (transformed, scaled) coordinates; into
 * buffer coordinates exactly as wlroots' drm backend does for its cursor
 * plane (drm_connector_move_cursor), so the box lines up with FRAME rects
 * whatever the output transform (the grey output, autorotate). Unlike drm.c
 * the hotspot is not subtracted here: glassine-epdd-desktop gets both */
static bool output_move_cursor(struct wlr_output *wo, int x, int y)
{
    struct epd_output *o = wl_container_of(wo, o, base);
    struct epd_backend *be = o->be;
    struct wlr_box box = { .x = x, .y = y };
    int w, h;
    wlr_output_transformed_resolution(wo, &w, &h);
    wlr_box_transform(&box, &box, wlr_output_transform_invert(wo->transform), w, h);
    if (box.x == be->cur.x && box.y == be->cur.y) return true;
    be->cur.x = box.x; be->cur.y = box.y;
    cursor_send(be, false);
    return true;
}

static const struct wlr_output_impl output_impl = {
    .destroy = output_destroy,
    .test = output_test,
    .commit = output_commit,
    .set_cursor = output_set_cursor,
    .move_cursor = output_move_cursor,
    .get_cursor_formats = output_cursor_formats,
    .get_primary_formats = output_formats,
};

static const struct wlr_output_impl output_impl_swcursor = {
    .destroy = output_destroy,
    .test = output_test,
    .commit = output_commit,
    .get_primary_formats = output_formats,
};

/* ---- the backend ----------------------------------------------------------------------- */
static bool backend_start(struct wlr_backend *b)
{
    struct epd_backend *be = wl_container_of(b, be, base);
    if (be->started) return true;
    be->started = true;
    struct epd_output *o = calloc(1, sizeof *o);
    if (!o) return false;
    o->be = be;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    /* a nominal refresh for clients (the panel has no fixed one): with 0 in the
     * mode and the presentation feedback, Chromium/Electron left a page half
     * drawn until the next input. GLASSINE_EPD_REFRESH=<Hz> (0 = none), default 60 */
    const char *rf = getenv("GLASSINE_EPD_REFRESH");
    int hz = rf && *rf ? atoi(rf) : 60;
    wlr_output_state_set_custom_mode(&st, be->w, be->h, hz > 0 ? hz * 1000 : 0);
    wlr_output_init(&o->base, &be->base, getenv("GLASSINE_EPD_CURSOR") ? &output_impl_swcursor : &output_impl, be->loop, &st);
    wlr_output_state_finish(&st);
    wlr_output_set_name(&o->base, "EPD-1");
    wlr_output_set_description(&o->base, "Glassine e-ink panel (glassine-epdd-desktop)");
    o->base.phys_width = be->grey ? 209 : 157;     /* mm, 10.3" 4:3 at 227 dpi, of the mode */
    o->base.phys_height = be->grey ? 157 : 209;
    o->frame_timer = wl_event_loop_add_timer(be->loop, frame_timeout, o);
    be->out = o;
    wl_signal_emit_mutable(&be->base.events.new_output, &o->base);
    try_connect(be);
    return true;
}

static void backend_destroy(struct wlr_backend *b)
{
    struct epd_backend *be = wl_container_of(b, be, base);
    if (be->out) wlr_output_destroy(&be->out->base);
    disconnect(be, "backend destroyed");
    lut_hold(be, NULL);
    band_accept(be, NULL);
    pixman_region32_fini(&be->band.pend);
    if (be->band.timer) wl_event_source_remove(be->band.timer);
    if (be->lut.sc) wlr_swapchain_destroy(be->lut.sc);
    wlr_damage_ring_finish(&be->lut.ring);
    for (int i = 0; i < MAXBUF; i++) if (be->bufs[i].b) wl_list_remove(&be->bufs[i].destroy.link);
    if (be->retry) wl_event_source_remove(be->retry);
    if (be->cur.fd >= 0) { munmap(be->cur.map, 2 * GLASSINE_EPD_CUR_SLOT); close(be->cur.fd); }
    wlr_drm_format_set_finish(&be->fmt_dmabuf);
    wlr_drm_format_set_finish(&be->fmt_shm);
    wlr_drm_format_set_finish(&be->fmt_cursor);
    wlr_backend_finish(&be->base);
    free(be);
}

static uint32_t backend_caps(struct wlr_backend *b)
{
    (void)b;
    /* memfd buffers (pixman) or GBM dma-bufs (GLES): their fd goes to glassine-epdd-desktop */
    return WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DMABUF;
}

static const struct wlr_backend_impl backend_impl = {
    .start = backend_start,
    .destroy = backend_destroy,
    .get_buffer_caps = backend_caps,
};

bool glassine_epd_backend_is(struct wlr_backend *b) { return b->impl == &backend_impl; }

/* grey output : GLASSINE_EPD_GREY=1 and a GLES renderer that renders
 * R8 LINEAR. Then the mode is the panel's own order (1872x1404) and sway sets
 * the output transform that turns the portrait desktop into it, and the
 * patched wlroots GLES shaders composite luma into the R8 buffers: glassine-epdd-desktop
 * gets 1 byte per pixel, rotated, only its gamma table left to apply.
 * Anything else (pixman, no R8): today's portrait XRGB frames. */
void glassine_epd_backend_renderer(struct wlr_backend *b, void *data)
{
    struct wlr_renderer *r = data;
    if (!glassine_epd_backend_is(b)) return;
    struct epd_backend *be = wl_container_of(b, be, base);
    const char *g = getenv("GLASSINE_EPD_GREY");
    if (!g || !*g || !strcmp(g, "0") || be->started || be->grey) return;
    /* (wlr_renderer_get_render_formats: made public by our wlroots, sway/wlroots/) */
    const struct wlr_drm_format_set *rf = wlr_renderer_is_gles2(r) ? wlr_renderer_get_render_formats(r) : NULL;
    if (!rf || !wlr_drm_format_set_has(rf, DRM_FORMAT_R8, DRM_FORMAT_MOD_LINEAR)) {
        wlr_log(WLR_ERROR, "glassine-epd: GLASSINE_EPD_GREY: %s, frames stay XRGB", !rf ? "not a GLES renderer" : "no R8 LINEAR render target");
        return;
    }
    be->grey = true;
    int t = be->w;
    be->w = be->h;
    be->h = t;
    /* buffer (panel) pixel (c, r) = portrait (r, 1871 - c): the content turned
     * 90 degrees clockwise. GLASSINE_EPD_GREY_TRANSFORM=0..7 overrides (tests;
     * input-grey.conf's touch/pen matrices assume 270) */
    be->transform = WL_OUTPUT_TRANSFORM_270;
    const char *tr = getenv("GLASSINE_EPD_GREY_TRANSFORM");
    if (tr && *tr >= '0' && *tr <= '7') be->transform = *tr - '0';
    wlr_drm_format_set_finish(&be->fmt_dmabuf);
    wlr_drm_format_set_add(&be->fmt_dmabuf, DRM_FORMAT_R8, DRM_FORMAT_MOD_LINEAR);
    const char *ga = getenv("GLASSINE_EPD_GAMMA");
    be->lut.r = r;
    be->lut.on = !(ga && !strcmp(ga, "0"));
    wlr_damage_ring_set_bounds(&be->lut.ring, be->w, be->h);
    wlr_log(WLR_INFO, "glassine-epd: grey output, R8 %dx%d, transform %d, gamma on the GPU %s", be->w, be->h, be->transform,
            be->lut.on ? "offered" : "off (GLASSINE_EPD_GAMMA=0)");
}

bool glassine_epd_output_grey(struct wlr_output *wo, enum wl_output_transform *t)
{
    if (wo->impl != &output_impl && wo->impl != &output_impl_swcursor) return false;
    struct epd_output *o = wl_container_of(wo, o, base);
    if (!o->be->grey) return false;
    if (t) *t = o->be->transform;
    return true;
}

struct wlr_backend *glassine_epd_backend_create(struct wl_event_loop *loop)
{
    struct epd_backend *be = calloc(1, sizeof *be);
    if (!be) return NULL;
    wlr_backend_init(&be->base, &backend_impl);
    be->loop = loop;
    be->fd = -1;
    be->cur.fd = -1;
    be->w = 1404;
    be->h = 1872;
    const char *sz = getenv("GLASSINE_EPD_SIZE");
    if (sz) sscanf(sz, "%dx%d", &be->w, &be->h);
    be->retry = wl_event_loop_add_timer(loop, retry_connect, be);
    wlr_damage_ring_init(&be->lut.ring);
    pixman_region32_init(&be->band.pend);
    be->band.timer = wl_event_loop_add_timer(loop, band_quiet, be);
    static const uint32_t fmts[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    for (int i = 0; i < 2; i++) {
        wlr_drm_format_set_add(&be->fmt_dmabuf, fmts[i], DRM_FORMAT_MOD_LINEAR);
        wlr_drm_format_set_add(&be->fmt_shm, fmts[i], DRM_FORMAT_MOD_INVALID);   /* as the pixman renderer lists them */
        wlr_drm_format_set_add(&be->fmt_shm, fmts[i], DRM_FORMAT_MOD_LINEAR);
    }
    wlr_drm_format_set_add(&be->fmt_cursor, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
    wlr_log(WLR_INFO, "glassine-epd: backend, %dx%d", be->w, be->h);
    return &be->base;
}
