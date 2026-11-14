/*
 * xbr_game_glue.c - default implementations of the hooks a game supplies.
 *
 * [android-port] New. The generated code and the runtime call these; a game
 * project used to copy them from templates/new-game/src/recomp_manual.c. They
 * are weak here, so a game that needs its own (hand-written replacements for
 * functions the recompiler cannot translate, say) defines them and wins.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xbr_host.h"

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

#if defined(__GNUC__) || defined(__clang__)
#define XBR_WEAK __attribute__((weak))
#else
#define XBR_WEAK
#endif

/* Defaults for a project the pipeline did not configure: retail RAM, the
 * utility-drive mount skipped (see xbr_boot.c), no title environment. */
XBR_WEAK const XbrGameConfig xbr_game_config = {
    "Xbox title", 0, 0, 64, 0, NULL
};

XBR_WEAK recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}

XBR_WEAK void recomp_icall_fail_log(uint32_t va)
{
    int i;
    fprintf(stderr, "[ICALL] failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    for (i = 0; i < 16; i++) {
        uint32_t t = g_icall_trace[(g_icall_trace_idx - 16 + i) & 15];
        if (t)
            fprintf(stderr, "    recent target [%2d] 0x%08X\n", i, t);
    }
    fflush(stderr);
}

XBR_WEAK void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;
    uint64_t n;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    n = ++hits[i];
    while (n >= 10 && n % 10 == 0)
        n /= 10;
    if (n != 1)
        return;
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(at call #%llu)\n", va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

XBR_WEAK void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';
    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr, "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                        " (a no-op; RECOMP_UNIMPL_TRAP=1 stops here)\n", text, va);
        fflush(stderr);
    }
    if (stop)
        abort();
}
