/* glassine-epd-sink — a stand-in for glassine-epdd-desktop on the glassine-epd socket (host tests of
 * the sway backend, glassine-epd-output-proto.h). Listens, maps every BUF, logs each FRAME
 * (buffer, damage, time since the last DONE), answers DONE after DELAY ms
 * (a panel update's duration), and writes the frame as a PPM:
 * glassine-epd-sink [-s SOCK] [-d DELAY_MS] [-o DIR] [-n FRAMES]
 * -o DIR: DIR/frame-SEQ.ppm for every frame (and DIR/last.ppm)
 * -n N: exit after N frames (0: run until killed)
 * The dead band's test (band-test.sh), for a GLES grey sway:
 * -g GAMMA: after HELLO, a gamma table for its GPU (LUT; 1: the identity)
 * -b T: after HELLO, the dead band's T (BAND), -q QUIET_MS its gap (default 125)
 * -c FRAMES:T: after that many frames, T changes
 * R8 frames: mapped dma-bufs (waits for their write fences); per frame a BAND line:
 * its tag, true marker, 5 samples along the middle row, FNV-1a of all its bytes;
 * DIR/last.pgm is the newest one */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "glassine-epd-output-proto.h"

#define MAXBUF 16
static struct { uint32_t id, w, h, stride, format, offset; uint8_t *map; size_t len; int fd; } bufs[MAXBUF];
#define FOURCC_R8 0x20203852u

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int find(uint32_t id)
{
    for (int i = 0; i < MAXBUF; i++) if (bufs[i].map && bufs[i].id == id) return i;
    return -1;
}

/* XRGB8888 / ARGB8888 (little endian: B G R X in memory) -> PPM */
static void dump(const char *dir, const char *name, int i)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "P6 %u %u 255\n", bufs[i].w, bufs[i].h);
    const uint8_t *base = bufs[i].map + bufs[i].offset;
    for (uint32_t y = 0; y < bufs[i].h; y++)
        for (uint32_t x = 0; x < bufs[i].w; x++) {
            const uint8_t *px = base + y * bufs[i].stride + x * 4;
            uint8_t rgb[3] = { px[2], px[1], px[0] };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

/* an R8 frame: the BAND line (see the top) */
static void r8_frame(const struct glassine_epd_msg *m, int i, const char *dir, double t, double dt)
{
    struct pollfd pf = { bufs[i].fd, POLLIN, 0 };
    if (bufs[i].fd >= 0) {
        poll(&pf, 1, 500);
        struct dma_buf_sync sy = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        ioctl(bufs[i].fd, DMA_BUF_IOCTL_SYNC, &sy);
    }
    /* one copy first: a discrete GPU's buffer reads at ~0.3 MB/s byte by byte
     * (write-combined VRAM, RX 580: 2.6 MB took ~10 s, past sway's DONE timeout) */
    static uint8_t *copy;
    static size_t cap;
    size_t need = (size_t)bufs[i].stride * bufs[i].h;
    if (need > cap) { free(copy); copy = malloc(need); cap = copy ? need : 0; }
    if (!copy) return;
    memcpy(copy, bufs[i].map + bufs[i].offset, need);
    const uint8_t *base = copy;
    uint32_t h = 2166136261u;
    for (uint32_t y = 0; y < bufs[i].h; y++) {
        const uint8_t *row = base + (size_t)y * bufs[i].stride;
        for (uint32_t x = 0; x < bufs[i].w; x++) h = (h ^ row[x]) * 16777619u;
    }
    int s[5];
    const uint8_t *mid = base + (size_t)(bufs[i].h / 2) * bufs[i].stride;
    for (int k = 0; k < 5; k++) s[k] = mid[(2 * k + 1) * bufs[i].w / 10];
    long area = 0;
    for (uint32_t k = 0; k < m->nrect; k++) area += (long)m->rect[k].w * m->rect[k].h;
    printf("BAND seq=%u tag=%x true=%u rects=%u px=%ld t=%.1f dt=%.1f s=%d,%d,%d,%d,%d fnv=%08x\n", m->seq, m->format, m->offset,
           m->nrect, area, t, dt, s[0], s[1], s[2], s[3], s[4], h);
    if (dir) {
        char p[512];
        snprintf(p, sizeof p, "%s/last.pgm", dir);
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "P5 %u %u 255\n", bufs[i].w, bufs[i].h);
            for (uint32_t y = 0; y < bufs[i].h; y++) fwrite(base + (size_t)y * bufs[i].stride, 1, bufs[i].w, f);
            fclose(f);
        }
    }
    if (bufs[i].fd >= 0) {
        struct dma_buf_sync sy = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        ioctl(bufs[i].fd, DMA_BUF_IOCTL_SYNC, &sy);
    }
}

static void send_band(int fd, int t, int quiet)
{
    struct glassine_epd_msg m = { .type = GLASSINE_EPD_BAND, .seq = (uint32_t)t, .width = (uint32_t)quiet };
    send(fd, &m, offsetof(struct glassine_epd_msg, rect), MSG_NOSIGNAL);
}

int main(int argc, char **argv)
{
    double gamma = 0;
    int band_t = 0, band_q = 125, change_at = 0, change_t = 0;
    const char *sock = GLASSINE_EPD_SOCK, *dir = NULL;
    int delay = 0, maxf = 0, c;
    while ((c = getopt(argc, argv, "s:d:o:n:g:b:q:c:")) != -1) {
        if (c == 's') sock = optarg;
        else if (c == 'd') delay = atoi(optarg);
        else if (c == 'o') dir = optarg;
        else if (c == 'n') maxf = atoi(optarg);
        else if (c == 'g') gamma = atof(optarg);
        else if (c == 'b') band_t = atoi(optarg);
        else if (c == 'q') band_q = atoi(optarg);
        else if (c == 'c') sscanf(optarg, "%d:%d", &change_at, &change_t);
        else { fprintf(stderr, "usage: %s [-s SOCK] [-d DELAY_MS] [-o DIR] [-n FRAMES]\n", argv[0]); return 2; }
    }
    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    unlink(sock);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 1)) { perror("glassine-epd-sink: listen"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("glassine-epd-sink: listening on %s (DONE after %d ms)\n", sock, delay);
    int frames = 0;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        printf("glassine-epd-sink: backend connected\n");
        double last_done = now_ms();
        for (;;) {
            struct glassine_epd_msg m;
            union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } ctl;
            struct iovec iov = { &m, sizeof m };
            struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.b, .msg_controllen = sizeof ctl.b };
            ssize_t n = recvmsg(fd, &mh, 0);
            if (n <= 0) { printf("glassine-epd-sink: backend gone\n"); break; }
            int rfd = -1;
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            if (cm && cm->cmsg_type == SCM_RIGHTS) memcpy(&rfd, CMSG_DATA(cm), sizeof rfd);
            if (m.type == GLASSINE_EPD_HELLO) {
                printf("HELLO caps %x\n", m.seq);
                if (gamma > 0 && m.seq & GLASSINE_EPD_CAP_GAMMA) {
                    uint8_t t[256];
                    for (int k = 0; k < 256; k++) t[k] = (uint8_t)lround(255.0 * pow(k / 255.0, gamma));
                    struct glassine_epd_msg l = { .type = GLASSINE_EPD_LUT, .id = glassine_epd_lut_tag(t) };
                    memcpy(l.rect, t, 256);
                    send(fd, &l, offsetof(struct glassine_epd_msg, rect) + 256, MSG_NOSIGNAL);
                }
                if (band_t && m.seq & GLASSINE_EPD_CAP_BAND) send_band(fd, band_t, band_q);
            } else if (m.type == GLASSINE_EPD_BUF || m.type == GLASSINE_EPD_DMABUF) {
                int i = find(m.id);
                if (i < 0) for (i = 0; i < MAXBUF && bufs[i].map; i++) ;
                if (i == MAXBUF || rfd < 0) { printf("BUF %u: no slot/fd\n", m.id); if (rfd >= 0) close(rfd); continue; }
                if (bufs[i].map) { munmap(bufs[i].map, bufs[i].len); if (bufs[i].fd >= 0) close(bufs[i].fd); }
                size_t len = (size_t)m.offset + (size_t)m.stride * m.height;
                uint8_t *p = mmap(NULL, len, PROT_READ, MAP_SHARED, rfd, 0);
                bufs[i].fd = m.type == GLASSINE_EPD_DMABUF ? rfd : -1;
                if (m.type != GLASSINE_EPD_DMABUF) close(rfd);
                if (p == MAP_FAILED) { perror("mmap"); bufs[i].map = NULL; continue; }
                bufs[i].id = m.id; bufs[i].w = m.width; bufs[i].h = m.height; bufs[i].stride = m.stride;
                bufs[i].format = m.format; bufs[i].offset = m.offset; bufs[i].map = p; bufs[i].len = len;
                printf("BUF %u %ux%u stride %u format %.4s\n", m.id, m.width, m.height, m.stride, (char *)&m.format);
            } else if (m.type == GLASSINE_EPD_DROP) {
                int i = find(m.id);
                if (i >= 0) { munmap(bufs[i].map, bufs[i].len); if (bufs[i].fd >= 0) close(bufs[i].fd); bufs[i].map = NULL; }
                printf("DROP %u\n", m.id);
            } else if (m.type == GLASSINE_EPD_FRAME) {
                int i = find(m.id);
                long area = 0;
                for (uint32_t k = 0; k < m.nrect; k++) area += (long)m.rect[k].w * m.rect[k].h;
                printf("FRAME %u buf %u: %u rects, %ld px%s, %.0f ms since last DONE\n", m.seq, m.id, m.nrect, area,
                       m.nrect ? "" : " (whole)", now_ms() - last_done);
                for (uint32_t k = 0; k < m.nrect && k < 4; k++)
                    printf("  %dx%d+%d+%d\n", m.rect[k].w, m.rect[k].h, m.rect[k].x, m.rect[k].y);
                if (i >= 0 && bufs[i].format == FOURCC_R8) r8_frame(&m, i, dir, now_ms(), now_ms() - last_done);
                else if (i >= 0 && dir) {
                    char name[64];
                    snprintf(name, sizeof name, "frame-%04u.ppm", m.seq);
                    dump(dir, name, i);
                    dump(dir, "last.ppm", i);
                }
                if (delay) usleep(delay * 1000);
                struct glassine_epd_msg d = { .type = GLASSINE_EPD_DONE, .id = m.id, .seq = m.seq };
                send(fd, &d, offsetof(struct glassine_epd_msg, rect), MSG_NOSIGNAL);
                last_done = now_ms();
                if (maxf && ++frames >= maxf) return 0;
                if (!maxf) frames++;
                if (change_at && frames == change_at) { printf("BAND T -> %d\n", change_t); send_band(fd, change_t, band_q); }
            }
        }
        close(fd);
    }
}
