/*
 * xbr_boot.c - the boot sequence of a recompiled title on a POSIX host.
 *
 * [android-port] New. See xbr_host.h for the order and why it is shared.
 */
#if defined(_WIN32)
#error "xbr_boot.c is the POSIX host layer; Windows titles use templates/new-game"
#endif

#define _GNU_SOURCE 1
#include "xbr_host.h"

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "kernel/xbox_memory_layout.h"
#include "kernel/kernel.h"
#if XBR_HAVE_VULKAN
#include "nv2a_vk.h"            /* src/vk: the NV2A render back end on Vulkan */
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_path_init(const char *game_dir, const char *save_dir);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* Hardware-register traps need an instruction decoder for the host ISA wired
 * to its signal context: x86-64 Linux and every arm64 POSIX host have one
 * (src/platform/mmio_decode_*.h, xbox_PosixMmioFault). */
#if defined(__aarch64__) || (defined(__linux__) && defined(__x86_64__))
#define XBR_HOST_MMIO 1
#else
#define XBR_HOST_MMIO 0
#endif

static const char *volatile s_stage = "not started";

const char *xbr_boot_stage(void) { return s_stage; }

static void stage(const char *name)
{
    s_stage = name;
    printf("[BOOT] %s\n", name);
    fflush(stdout);
}

void xbr_boot_options_from_config(XbrBootOptions *o)
{
    if (xbr_game_config.total_ram_mb)
        o->total_ram = (size_t)xbr_game_config.total_ram_mb * 1024u * 1024u;
    o->keep_mount_utility_drive = xbr_game_config.keep_mount_utility_drive;
}

void xbr_boot_env_defaults(void)
{
    const char *const *e;

    /* The title's own defaults first, so they take precedence over the
     * generic ones below; the environment beats both. */
    for (e = xbr_game_config.env; e && *e; e++) {
        char name[128];
        const char *eq = strchr(*e, '=');
        size_t n = eq ? (size_t)(eq - *e) : 0;
        if (!eq || n == 0 || n >= sizeof name)
            continue;
        memcpy(name, *e, n);
        name[n] = 0;
        if (!getenv(name)) {
            setenv(name, eq + 1, 0);
            printf("[BOOT] %s=%s (title default)\n", name, eq + 1);
        }
    }
    static const char *const defaults[][2] = {
        { "RECOMP_GUEST_LOCK",    "1" },
        { "RECOMP_TITLE_KEVENTS", "1" },
        { "RECOMP_VBLANK",        "paced" },
        { "RECOMP_PB_EXEC",       "1" },
    };
    size_t i;
    for (i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        const char *cur = getenv(defaults[i][0]);
        if (!cur) {
            setenv(defaults[i][0], defaults[i][1], 0);
            printf("[BOOT] %s=%s (default)\n", defaults[i][0], defaults[i][1]);
        } else {
            printf("[BOOT] %s=%s (environment)\n", defaults[i][0], cur);
        }
    }
}

/* ── crash reporting ─────────────────────────────────────────────────────── */

static volatile sig_atomic_t s_in_handler;

void (*xbr_crash_sink)(const char *line);

static void crash_write(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t r = write(2, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
        (void)r;
        if (xbr_crash_sink)
            xbr_crash_sink(buf);
    }
}

static void *fault_pc(void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
#if defined(__aarch64__) && defined(__APPLE__)
    return (void *)uc->uc_mcontext->__ss.__pc;
#elif defined(__aarch64__)
    return (void *)uc->uc_mcontext.pc;
#elif defined(__x86_64__) && defined(__APPLE__)
    return (void *)uc->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
    return (void *)uc->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
    return NULL;
#endif
}

static void restore_default(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
    void *pc;
    Dl_info info;
    uintptr_t addr = (uintptr_t)si->si_addr;

#if XBR_HOST_MMIO
    /* A trapped hardware register (APU, AC'97, USB OHCI): serviced, the
     * kernel resumes after the instruction. */
    if (xbox_PosixMmioFault(ucv, addr))
        return;
#endif
    if (s_in_handler) {
        restore_default(sig);
        return;
    }
    s_in_handler = 1;
    pc = fault_pc(ucv);
    crash_write("[CRASH] signal %d at host pc %p, fault address %p (boot stage: %s)\n",
                sig, pc, (void *)addr, s_stage);
    if (pc && dladdr(pc, &info) && info.dli_sname)
        crash_write("[CRASH]   in %s+0x%lx (%s)\n", info.dli_sname,
                    (unsigned long)((uintptr_t)pc - (uintptr_t)info.dli_saddr),
                    info.dli_fname ? info.dli_fname : "?");
    if (g_xbox_mem_offset && addr >= (uintptr_t)g_xbox_mem_offset
            && addr - (uintptr_t)g_xbox_mem_offset < 0x100000000ull)
        crash_write("[CRASH]   guest address 0x%08X (si_code %d)\n",
                    (unsigned)(addr - (uintptr_t)g_xbox_mem_offset), si->si_code);
    else
        crash_write("[CRASH]   not a guest address (guest memory at %p, si_code %d)\n",
                    (void *)(uintptr_t)g_xbox_mem_offset, si->si_code);
    crash_write("[CRASH]   guest eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int i, shown = 0;
        crash_write("[CRASH]   guest return addresses on the stack:");
        for (i = 0; i < 256 && shown < 16; i++) {
            if (sp[i] >= g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                crash_write(" %08X", sp[i]);
                shown++;
            }
        }
        crash_write("\n");
    }
    /* Returning re-executes the access with the default disposition, so the
     * process dies by the original signal and its exit status says so. */
    restore_default(sig);
}

void xbr_install_crash_handler(void)
{
    static char *altstack;
    stack_t ss;
    struct sigaction sa;
    const size_t altsize = 256 * 1024;

    if (!altstack)
        altstack = (char *)malloc(altsize);
    if (altstack) {
        memset(&ss, 0, sizeof ss);
        ss.ss_sp = altstack;
        ss.ss_size = altsize;
        if (sigaltstack(&ss, NULL) != 0)
            fprintf(stderr, "[BOOT] sigaltstack failed (%s)\n", strerror(errno));
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
}

/* ── boot ────────────────────────────────────────────────────────────────── */

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *data;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data)
        *size = (size_t)n;
    return data;
}

int xbr_boot_run(const XbrBootOptions *o)
{
    char xbe_path[4096];
    void *xbe = NULL;
    size_t xbe_size = 0;
    uint32_t entry;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[BOOT] title \"%s\" (title ID %08X), page size %ld\n",
           xbr_game_config.title ? xbr_game_config.title : "?",
           xbr_game_config.title_id, sysconf(_SC_PAGESIZE));
    stage("crash handler");
    xbr_install_crash_handler();

    stage("read XBE");
    snprintf(xbe_path, sizeof xbe_path, "%s/%s", o->game_dir,
             o->xbe_name ? o->xbe_name : "default.xbe");
    xbe = read_file(xbe_path, &xbe_size);
    if (!xbe || xbe_size < 0x178 || memcmp(xbe, "XBEH", 4) != 0) {
        fprintf(stderr, "[BOOT] FAILED: cannot read an XBE at %s (%s)\n", xbe_path,
                xbe ? "not an XBE" : strerror(errno));
        free(xbe);
        return 2;
    }
    printf("[BOOT] XBE %s: %zu bytes\n", xbe_path, xbe_size);

    stage("memory layout");
    if (o->total_ram)
        xbox_SetTotalRam(o->total_ram);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "[BOOT] FAILED: xbox_MemoryLayoutInit (the fixed guest address "
                        "range may be unavailable in this process)\n");
        free(xbe);
        return 3;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("[BOOT] guest memory at host %p (offset 0x%llx)\n", xbox_GetMemoryBase(),
           (unsigned long long)g_xbox_mem_offset);

#if XBR_HOST_MMIO
    if (getenv("RECOMP_AC97_READY")) {
        stage("emulated APU");
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        printf("[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED to initialise");
    }
#endif

    stage("kernel");
    xbox_kernel_init();
    stage("paths");
    xbox_path_init(o->game_dir, o->save_dir);
    printf("[BOOT] D: = %s, saves = %s\n", o->game_dir, o->save_dir ? o->save_dir : "(default)");
    stage("kernel bridge");
    xbox_kernel_bridge_init();

    stage("guest stack and init flags");
    g_esp = XBOX_STACK_TOP;
    {
        /* XINIT_MOUNT_UTILITY_DRIVE makes XAPI open the raw disk to read the
         * partition table; POSIX hosts back no raw disk, so the mount fails
         * and XAPI returns to the dashboard. Z: needs no mount (the path
         * layer maps it to the save cache). */
        volatile uint32_t *flags =
            (volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + 0x00010124u);
        if ((*flags & 1u) && !o->keep_mount_utility_drive) {
            *flags &= ~1u;
            printf("[BOOT] XBE init flag MOUNT_UTILITY_DRIVE cleared\n");
        }
    }
    {
        /* The entry point as the loader decoded it, for the log. */
        const uint8_t *h = (const uint8_t *)xbe;
        uint32_t raw;
        memcpy(&raw, h + 0x128, 4);
        entry = raw ^ 0xA8FC57ABu;
        if (entry < 0x10000 || entry >= 0x10000000)
            entry = raw ^ 0x94859D4Bu;
        if (entry < 0x10000 || entry >= 0x10000000)
            entry = raw;
    }

    /* The pushbuffer executor draws with a render back end when one is
     * registered, else with its CPU rasteriser. Vulkan unless RECOMP_VK=0;
     * any failure to bring it up leaves the CPU rasteriser drawing, and the
     * line below says which renderer runs and why. */
    stage("render back end");
#if XBR_HAVE_VULKAN
    {
        const char *v = getenv("RECOMP_VK");
        if (v && !strcmp(v, "0"))
            printf("[BOOT] render: cpu (RECOMP_VK=0)\n");
        else if (nv2a_vk_register() == 0)
            printf("[BOOT] render: vulkan (%s)\n", nv2a_vk_status());
        else
            printf("[BOOT] render: cpu (Vulkan %s)\n", nv2a_vk_status());
    }
#else
    printf("[BOOT] render: cpu (built without the Vulkan back end)\n");
#endif
    fflush(stdout);

    stage("dispatch table");
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; indirect calls use the "
                        "binary search\n");
    xbox_WatchdogStart();

    xbr_stats_start();
    printf("[BOOT] entering xbe_entry_point (guest 0x%08X), esp=0x%08X\n", entry, g_esp);
    s_stage = "running title";
    fflush(stdout);
    xbe_entry_point();

    printf("[BOOT] title returned from its entry point\n");
    fflush(stdout);
    free(xbe);
    return 0;
}
