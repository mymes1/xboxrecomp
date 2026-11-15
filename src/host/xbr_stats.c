/*
 * xbr_stats.c - what the title is showing, for boot evidence and status.
 *
 * [android-port] New. The pushbuffer executor reports each flip through
 * xbox_FramebufferWindowFrameStats, and AvSetDisplayMode and the flip name
 * the scanout surface through xbox_FramebufferWindowSet; both are defined
 * by the presenter. The game link wraps the two (ld --wrap, see
 * src/host/CMakeLists.txt) so the host layer sees them too, without a change
 * to the presenter or the executor:
 *
 *   - xbr_stats_get(): flips, draws in the last frame, scanout address/pitch
 *   - a status line every RECOMP_STATUS_SECS seconds (default 5, 0 = off):
 *       [STATUS] t=10s flips=0 draws=0 display=0x83E00000 pitch=2560 fbsum=1A2B3C4D
 *     fbsum is a sampled checksum of the scanout surface: it moves when the
 *     title draws, whatever path the pixels took (rasteriser or CPU).
 *   - RECOMP_STATUS_BMP=<file>: the scanout surface as a 24-bit BMP at each
 *     status tick, so the last one written is what was on screen.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xbr_host.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static uint64_t s_flips;
static uint32_t s_draws, s_fb_va, s_fb_pitch, s_last_sum;
static uint64_t s_sum_changes;

#if !defined(XBR_NO_WRAP)
void __real_xbox_FramebufferWindowFrameStats(uint32_t draws);
void __real_xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);

void __wrap_xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    /* The app is in the background: the guest waits here (xbr_window.c). */
    xbr_hold_point();
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_draws, draws, __ATOMIC_RELAXED);
    __real_xbox_FramebufferWindowFrameStats(draws);
}

void __wrap_xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    __atomic_store_n(&s_fb_va, fb_va, __ATOMIC_RELAXED);
    __atomic_store_n(&s_fb_pitch, pitch, __ATOMIC_RELAXED);
    __real_xbox_FramebufferWindowSet(fb_va, pitch);
}
#endif

#define FB_W 640u
#define FB_H 480u

static const uint8_t *surface(uint32_t *bpp, uint32_t *pitch)
{
    uint32_t va = __atomic_load_n(&s_fb_va, __ATOMIC_RELAXED);
    uint32_t p = __atomic_load_n(&s_fb_pitch, __ATOMIC_RELAXED);
    uint32_t b = p / FB_W;
    if (!va || !p || (b != 2 && b != 4))
        return NULL;
    *bpp = b;
    *pitch = p;
    return (const uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static uint32_t surface_sum(void)
{
    uint32_t bpp, pitch, y, x, h = 2166136261u;
    const uint8_t *px = surface(&bpp, &pitch);
    if (!px)
        return 0;
    for (y = 0; y < FB_H; y += 4)
        for (x = 0; x < FB_W * bpp; x += 64) {
            uint32_t v;
            memcpy(&v, px + (size_t)y * pitch + x, 4);
            h = (h ^ v) * 16777619u;
        }
    return h;
}

static int write_bmp(const char *path)
{
    uint32_t bpp, pitch, y, x;
    const uint8_t *px = surface(&bpp, &pitch);
    uint32_t row = FB_W * 3u, img = row * FB_H, total = 54u + img;
    uint8_t hdr[54], *line;
    char tmp[1100];
    FILE *f;

    if (!px)
        return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    { uint32_t w = FB_W, h = FB_H; memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); }
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    line = (uint8_t *)malloc(row);
    for (y = 0; line && y < FB_H; y++) {
        const uint8_t *src = px + (size_t)(FB_H - 1 - y) * pitch;
        for (x = 0; x < FB_W; x++) {
            uint8_t r, g, b;
            if (bpp == 4) {
                b = src[x * 4 + 0]; g = src[x * 4 + 1]; r = src[x * 4 + 2];
            } else {
                uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
                r = (uint8_t)(((v >> 11) & 31) * 255 / 31);
                g = (uint8_t)(((v >> 5) & 63) * 255 / 63);
                b = (uint8_t)((v & 31) * 255 / 31);
            }
            line[x * 3 + 0] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return rename(tmp, path);
}

void xbr_stats_get(XbrStats *out)
{
    out->flips = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
    out->draws = __atomic_load_n(&s_draws, __ATOMIC_RELAXED);
    out->display_va = __atomic_load_n(&s_fb_va, __ATOMIC_RELAXED);
    out->display_pitch = __atomic_load_n(&s_fb_pitch, __ATOMIC_RELAXED);
    out->display_sum = s_last_sum;
    out->display_changes = s_sum_changes;
}

static void *status_main(void *arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg, t = 0;
    const char *bmp = getenv("RECOMP_STATUS_BMP");
    for (;;) {
        uint32_t sum;
        sleep(secs);
        t += secs;
        sum = surface_sum();
        if (sum != s_last_sum) {
            if (s_last_sum || sum)
                s_sum_changes++;
            s_last_sum = sum;
        }
        printf("[STATUS] t=%us flips=%llu draws=%u display=0x%08X pitch=%u fbsum=%08X "
               "fbchanges=%llu\n", t, (unsigned long long)__atomic_load_n(&s_flips, __ATOMIC_RELAXED),
               __atomic_load_n(&s_draws, __ATOMIC_RELAXED),
               __atomic_load_n(&s_fb_va, __ATOMIC_RELAXED),
               __atomic_load_n(&s_fb_pitch, __ATOMIC_RELAXED), sum,
               (unsigned long long)s_sum_changes);
        fflush(stdout);
        if (bmp && *bmp)
            write_bmp(bmp);
    }
    return NULL;
}

void xbr_stats_start(void)
{
    const char *e = getenv("RECOMP_STATUS_SECS");
    unsigned secs = e ? (unsigned)atoi(e) : 5u;
    pthread_t th;
    if (!secs)
        return;
    if (pthread_create(&th, NULL, status_main, (void *)(uintptr_t)secs) == 0)
        pthread_detach(th);
}
