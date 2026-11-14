/*
 * xbrtest -- a self-built Original Xbox test title for the Android pipeline.
 *
 * Built from this source with clang (zig cc, i386) into a PE, converted to an
 * XBE by pe2xbe.py and packed into an XDVDFS image by tools/android/xiso_build.py.
 * It is original code with no game content, so CI can push it through the
 * whole ISO -> APK path. It is NOT evidence that any commercial title works.
 *
 * What it exercises, each reported on its own line through DbgPrint and in
 * T:\xbrtest_results.txt:
 *
 *   TIMER    KeQueryPerformanceFrequency/Counter and KeDelayExecutionThread
 *   MEMORY   MmAllocateContiguousMemory + MmGetPhysicalAddress
 *   DISPLAY  AvSetDisplayMode on a contiguous framebuffer, then pixels
 *   FILE     NtCreateFile/NtReadFile of D:\data\xbrtest.bin (game files)
 *   CASE     the same file opened with different letter case
 *   SAVE     NtWriteFile + NtReadFile round trip on T:\ (title save area)
 *   THREAD   PsCreateSystemThreadEx worker with interlocked increments
 *   MATH     x87 float/double, 64-bit integer multiply, 32-bit division
 *
 * Result lines read "XBRTEST <NAME> PASS|FAIL <detail>", then
 * "XBRTEST DONE pass=<n> fail=<n>". The title then animates the framebuffer
 * forever (colour bars, a moving block and a frame counter) so a device shows
 * that the recompiled code keeps running and frames keep being presented.
 */
#include "xboxkrnl_min.h"

#define FB_W 640
#define FB_H 480
#define FB_PITCH (FB_W * 4)

/* ── freestanding helpers (clang may call these for struct copies) ── */

/* The MSVC ABI marks every object that uses floating point with a reference
 * to _fltused; the CRT normally defines it. */
int _fltused;

void *memset(void *d, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned int n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static unsigned int str_len(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

/* A line buffer and a tiny formatter: the runtime's DbgPrint prints the
 * format string verbatim, so lines are built here first. */
static char g_line[160];
static unsigned int g_pos;

static void line_reset(void) { g_pos = 0; g_line[0] = 0; }

static void put_str(const char *s)
{
    while (*s && g_pos + 1 < sizeof g_line) g_line[g_pos++] = *s++;
    g_line[g_pos] = 0;
}

static void put_uint(unsigned int v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n--) { if (g_pos + 1 < sizeof g_line) g_line[g_pos++] = tmp[n]; }
    g_line[g_pos] = 0;
}

static void put_hex(unsigned int v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    put_str("0x");
    for (i = 28; i >= 0; i -= 4)
        if (g_pos + 1 < sizeof g_line) g_line[g_pos++] = hx[(v >> i) & 15];
    g_line[g_pos] = 0;
}

/* ── results ── */

static char g_results[2048];
static unsigned int g_results_len;
static unsigned int g_pass, g_fail;

static void emit_line(void)
{
    unsigned int i;
    DbgPrint(g_line);
    for (i = 0; g_line[i] && g_results_len + 2 < sizeof g_results; i++)
        g_results[g_results_len++] = g_line[i];
    if (g_results_len + 1 < sizeof g_results) g_results[g_results_len++] = '\n';
}

static void report(const char *name, int ok, const char *what, unsigned int value)
{
    line_reset();
    put_str("XBRTEST ");
    put_str(name);
    put_str(ok ? " PASS " : " FAIL ");
    put_str(what);
    put_str("=");
    put_uint(value);
    emit_line();
    if (ok) g_pass++; else g_fail++;
}

/* ── kernel wrappers ── */

static void sleep_ms(unsigned int ms)
{
    LARGE_INTEGER iv;
    iv.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(1, 0, &iv);
}

static NTSTATUS open_file(HANDLE *h, const char *path, ULONG access, ULONG disposition)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    name.Length = (USHORT)str_len(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = 0;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    return NtCreateFile(h, access | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa, &iosb, 0,
                        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, disposition,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

static int read_all(HANDLE h, void *buf, ULONG len, ULONG *got)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS st;
    off.QuadPart = 0;
    iosb.Information = 0;
    st = NtReadFile(h, 0, 0, 0, &iosb, buf, len, &off);
    *got = iosb.Information;
    return NT_SUCCESS(st);
}

/* ── tests ── */

static unsigned int g_fb_va;      /* framebuffer, CPU view */

static void test_timer(void)
{
    ULONGLONG freq = KeQueryPerformanceFrequency();
    ULONGLONG t0 = KeQueryPerformanceCounter();
    ULONGLONG t1;
    unsigned int per_ms, ms;
    sleep_ms(20);
    t1 = KeQueryPerformanceCounter();
    per_ms = (unsigned int)freq / 1000u;
    ms = per_ms ? (unsigned int)(t1 - t0) / per_ms : 0;
    /* 20 ms asked; accept 15..500 ms (CI machines stall). */
    report("TIMER", freq > 1000 && t1 > t0 && ms >= 15 && ms <= 500, "slept_ms", ms);
}

static void test_memory_display(void)
{
    unsigned int phys, y, x, sum = 0;
    volatile unsigned int *fb;
    PVOID p = MmAllocateContiguousMemory(FB_PITCH * FB_H);
    report("MEMORY", p != 0, "contig_va_hi", (unsigned int)p >> 24);
    if (!p) return;
    phys = MmGetPhysicalAddress(p);
    g_fb_va = (unsigned int)p;
    /* Mode 640x480 NTSC, format LIN_X8R8G8B8 (0x1E), pitch 2560. */
    AvSetDisplayMode((PVOID)0xFD000000u, 0, 0x04010101u, 0x1Eu, FB_PITCH, phys);
    fb = (volatile unsigned int *)p;
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++)
            fb[y * FB_W + x] = 0xFF000000u | ((x * 255 / FB_W) << 16)
                             | ((y * 255 / FB_H) << 8) | 0x40u;
    for (y = 0; y < FB_H; y += 61)
        sum += fb[y * FB_W + y];
    report("DISPLAY", fb[FB_W * 240 + 320] == (0xFF000000u | (127u << 16) | (127u << 8) | 0x40u),
           "probe_sum", sum & 0xFFFF);
}

static unsigned char g_buf[4096];

static void test_file(void)
{
    HANDLE h = 0;
    ULONG got = 0, i, bad = 0;
    NTSTATUS st = open_file(&h, "\\??\\D:\\data\\xbrtest.bin", GENERIC_READ, FILE_OPEN);
    if (!NT_SUCCESS(st)) { report("FILE", 0, "open_status", (unsigned int)st); return; }
    if (!read_all(h, g_buf, sizeof g_buf, &got)) bad = 0xFFFF;
    NtClose(h);
    for (i = 0; i < got; i++)
        if (g_buf[i] != (unsigned char)((i * 7 + 3) & 0xFF)) bad++;
    report("FILE", got == sizeof g_buf && bad == 0, "bytes", got);
}

static void test_case(void)
{
    HANDLE h = 0;
    ULONG got = 0;
    NTSTATUS st = open_file(&h, "d:\\MEDIA\\Sub\\HELLO.TXT", GENERIC_READ, FILE_OPEN);
    if (!NT_SUCCESS(st)) { report("CASE", 0, "open_status", (unsigned int)st); return; }
    read_all(h, g_buf, 64, &got);
    NtClose(h);
    report("CASE", got >= 5 && g_buf[0] == 'h' && g_buf[4] == 'o', "bytes", got);
}

static void test_save(void)
{
    static unsigned char out[64], in[64];
    HANDLE h = 0;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    ULONG got = 0, i, bad = 0;
    NTSTATUS st;
    for (i = 0; i < sizeof out; i++) out[i] = (unsigned char)(0xA5 ^ i);
    st = open_file(&h, "\\??\\T:\\xbrsave.dat", GENERIC_WRITE | GENERIC_READ, FILE_OVERWRITE_IF);
    if (!NT_SUCCESS(st)) { report("SAVE", 0, "create_status", (unsigned int)st); return; }
    off.QuadPart = 0;
    st = NtWriteFile(h, 0, 0, 0, &iosb, out, sizeof out, &off);
    NtClose(h);
    if (!NT_SUCCESS(st)) { report("SAVE", 0, "write_status", (unsigned int)st); return; }
    st = open_file(&h, "\\??\\T:\\xbrsave.dat", GENERIC_READ, FILE_OPEN);
    if (!NT_SUCCESS(st)) { report("SAVE", 0, "reopen_status", (unsigned int)st); return; }
    read_all(h, in, sizeof in, &got);
    NtClose(h);
    for (i = 0; i < sizeof in; i++) if (in[i] != out[i]) bad++;
    report("SAVE", got == sizeof in && bad == 0, "bytes", got);
}

static volatile long g_counter;
static volatile long g_worker_done;

static void __stdcall worker(PVOID ctx1, PVOID ctx2)
{
    int i;
    long add = (long)ctx1;
    (void)ctx2;
    for (i = 0; i < 1000; i++)
        __sync_fetch_and_add(&g_counter, add);
    g_worker_done = 1;
    PsTerminateSystemThread(0);
}

static void test_thread(void)
{
    HANDLE th = 0;
    int i, j;
    NTSTATUS st = PsCreateSystemThreadEx(&th, 0, 0x4000, 0, 0, (PVOID)2, 0, 0, 0, worker);
    for (j = 0; j < 1000; j++)
        __sync_fetch_and_add(&g_counter, 1);
    for (i = 0; i < 200 && !g_worker_done; i++)
        sleep_ms(10);
    report("THREAD", NT_SUCCESS(st) && g_worker_done && g_counter == 3000,
           "counter", (unsigned int)g_counter);
}

static volatile float g_fquarter = 0.25f;
static volatile double g_dthree = 3.0;
static volatile unsigned int g_div = 7;
static volatile unsigned int g_mul = 0x10001u;

static void test_math(void)
{
    float f = 0.0f;
    double d, r;
    unsigned long long big;
    unsigned int q, ok = 1;
    int i;
    for (i = 1; i <= 100; i++)
        f += (float)i * g_fquarter;           /* 1262.5 exactly */
    if (f != 1262.5f) ok = 0;
    d = 1.0 / g_dthree;
    r = d * g_dthree;
    if (r < 0.9999999 || r > 1.0000001) ok = 0;
    big = (unsigned long long)0x12345678u * (unsigned long long)g_mul;
    if (big != 0x123468AC5678ull) ok = 0;
    q = 1000000u / g_div;
    if (q != 142857u) ok = 0;
    report("MATH", ok, "f_x10", (unsigned int)(f * 10.0f));
}

/* ── the animation loop ── */

static const unsigned char g_digits[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
};

static void draw_digit(volatile unsigned int *fb, int x0, int y0, int d, unsigned int c)
{
    int r, col, sy, sx;
    for (r = 0; r < 5; r++)
        for (col = 0; col < 3; col++)
            if (g_digits[d][r] & (4 >> col))
                for (sy = 0; sy < 8; sy++)
                    for (sx = 0; sx < 8; sx++)
                        fb[(y0 + r * 8 + sy) * FB_W + x0 + col * 8 + sx] = c;
}

static void animate_forever(void)
{
    static const unsigned int bars[8] = {
        0xFFFFFFFFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFF00FF00u,
        0xFFFF00FFu, 0xFFFF0000u, 0xFF0000FFu, 0xFF000000u };
    volatile unsigned int *fb = (volatile unsigned int *)g_fb_va;
    unsigned int frame = 0;
    if (!fb) for (;;) sleep_ms(1000);
    for (;;) {
        unsigned int x, y, bx = (frame * 4) % (FB_W - 64), n, k;
        for (y = 0; y < 320; y++)
            for (x = 0; x < FB_W; x++)
                fb[y * FB_W + x] = bars[(x / 80 + frame / 30) & 7];
        for (y = 320; y < FB_H; y++)
            for (x = 0; x < FB_W; x++)
                fb[y * FB_W + x] = (g_fail ? 0xFF400000u : 0xFF003000u);
        for (y = 340; y < 404; y++)
            for (x = bx; x < bx + 64; x++)
                fb[y * FB_W + x] = 0xFFFFFFFFu;
        n = frame;
        for (k = 0; k < 6; k++) {
            draw_digit(fb, 600 - (int)k * 32, 420, (int)(n % 10), 0xFFFFFF80u);
            n /= 10;
        }
        frame++;
        sleep_ms(16);
    }
}

/* ── entry ── */

static void write_results(void)
{
    HANDLE h = 0;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    if (NT_SUCCESS(open_file(&h, "\\??\\T:\\xbrtest_results.txt",
                             GENERIC_WRITE, FILE_OVERWRITE_IF))) {
        off.QuadPart = 0;
        NtWriteFile(h, 0, 0, 0, &iosb, g_results, g_results_len, &off);
        NtClose(h);
    }
}

static void __stdcall title_main(PVOID ctx1, PVOID ctx2)
{
    (void)ctx1; (void)ctx2;
    line_reset();
    put_str("XBRTEST BOOT build=");
    put_hex(0x20261114u);
    emit_line();

    test_timer();
    test_memory_display();
    test_file();
    test_case();
    test_save();
    test_thread();
    test_math();

    line_reset();
    put_str("XBRTEST DONE pass=");
    put_uint(g_pass);
    put_str(" fail=");
    put_uint(g_fail);
    emit_line();
    write_results();
    animate_forever();
}

/* The XBE entry point. As with XAPI's own startup, the title's main thread is
 * created with PsCreateSystemThreadEx (the runtime runs the first such thread
 * inline, as the game). */
void __cdecl XboxEntry(void)
{
    HANDLE th = 0;
    PsCreateSystemThreadEx(&th, 0, 0x10000, 0, 0, 0, 0, 0, 0, title_main);
    for (;;) sleep_ms(1000);
}
