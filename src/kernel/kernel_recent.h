/**
 * kernel_recent.h -- the last kernel calls, kept for the crash report.
 * [android-port] (new file; see docs/android/MODIFICATIONS.md)
 *
 * The bridge's call log (RECOMP_KERNEL_LOG_BUDGET) goes quiet after its
 * first few hundred calls, so a title that crashes later in its boot leaves
 * no record of what the kernel last told it -- and that is often the cause:
 * a file that did not open, a call that failed, a wait that never returned.
 * kernel_bridge.c keeps the last XBOX_KERNEL_RECENT calls in a ring --
 * ordinal, guest call site, the first three stack arguments, the result
 * and, for the calls that take an object name (the file and symbolic-link
 * calls), that name -- and a host's crash handler prints them
 * (src/host/xbr_boot.c). RECOMP_KERNEL_RECENT=0 turns the ring off.
 *
 * A call that repeats one of the last two entries -- same ordinal, call site
 * and thread: a polling loop, a critical section entered and left -- counts
 * on that entry instead of taking a slot, so a loop cannot push everything
 * else out of the ring.
 *
 * Lock-free: every new entry takes a slot from an atomic counter. A reader in
 * a signal handler can see an entry half written, and repeat counts can be
 * off under contention; for a crash report that is acceptable.
 */
#ifndef XBOX_KERNEL_RECENT_H
#define XBOX_KERNEL_RECENT_H

#include <stdint.h>

#define XBOX_KERNEL_RECENT 32

typedef struct {
    long long n;            /* entry number, 1, 2, ...; 0 = unused */
    unsigned repeat;        /* further calls folded into this entry */
    uint32_t ordinal;
    uint32_t caller;        /* guest return address of the call */
    uint32_t arg[3];        /* first three stack arguments */
    uint32_t result;        /* eax after the call, when done */
    uint32_t thread;        /* 1, 2, ...: the host thread's order of first call */
    volatile int done;      /* 0 while the call is still running (a wait...) */
    char name[56];          /* object name of a file/link call, else "" */
} xbox_kernel_recent_t;

/* Copies entry i (0 = the oldest kept, XBOX_KERNEL_RECENT - 1 = the newest)
 * into *out; returns 0 when that slot has not been used. Safe to call from a
 * signal handler: it only reads memory. */
int xbox_KernelRecentGet(int i, xbox_kernel_recent_t *out);

/* The calling host thread's number as recorded in the entries (0 when it has
 * made no kernel call). */
uint32_t xbox_KernelRecentThread(void);

#endif /* XBOX_KERNEL_RECENT_H */
