// Xenoblade Chronicles 3 1.0.0's large-block allocator, address region used up (observation gate 31, the stub of dm/games/010074F013262000/patches.py at main+0xf56234).
// The allocator (main+0xf55fb4, object x20) keeps per size bucket b a list of free chunk descriptors (head at x20 + 0x30 b + 0x78, count +0x80, guarded by the nn::os mutex at
// x20 + 0x30 b + 0x58) and takes a new chunk from its address region (object [x20 + 0x28]: +0 base, +8 offset in use, +0x10 chunks, +0x18 end) by moving the offset forward. A freed
// chunk (main+0xf54efc) has its memory unmapped and goes back to its bucket's list: its addresses are never returned, so the region is spent by the sum of the buckets' peaks. Built
// for a 60 GB region, at 4 GB the offset reached the end and every later new chunk failed (the opening movie's 65 MB, a new map's 193 MB terrain, 16 MB character data).
// When the offset would pass the end, the callback hands out addresses from a list of free ranges of that region; when no range is large enough it takes every free chunk off every
// bucket's list (unmapped already: descriptor +8, the bytes mapped, is 0), under that bucket's own mutex, plus the region's unused end, joins neighbouring ranges and tries again.
// The allocator's own mutex is held around the call and is the only lock taken around a bucket's (a free takes the bucket's alone, a reuse lets it go before the allocator's), so the
// order cannot deadlock. A descriptor taken off a list stays in the allocator's descriptor array unused. Counters in the log (DM_STATEPOLL=1 reports them with the state)
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>

#define XC3_MAIN_V0 "60887a25b66529910275a1066003a002"
enum { CLASS_TABLE = 0x1791b98, CLASSES = 64, REGIONS = 8 };
typedef struct { uint64_t lo, hi; } va_range;
static struct { uint64_t region, alloc; va_range *r; int n, cap; } g_rg[REGIONS];
static int g_nrg;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_main;
static void (*g_lock)(void *), (*g_unlock)(void *);
static _Atomic uint64_t g_served, g_failed, g_reclaimed;

static void rg_add(int k, uint64_t lo, uint64_t hi) {   // insert [lo, hi) sorted, joined with its neighbours
    if (hi <= lo) return;
    int i = 0; while (i < g_rg[k].n && g_rg[k].r[i].lo < lo) i++;
    if (i > 0 && g_rg[k].r[i - 1].hi >= lo) { if (hi > g_rg[k].r[i - 1].hi) g_rg[k].r[i - 1].hi = hi; i--; }
    else {
        if (g_rg[k].n == g_rg[k].cap) { g_rg[k].cap = g_rg[k].cap ? g_rg[k].cap * 2 : 256; g_rg[k].r = realloc(g_rg[k].r, sizeof(va_range) * g_rg[k].cap); }
        memmove(&g_rg[k].r[i + 1], &g_rg[k].r[i], sizeof(va_range) * (g_rg[k].n - i)); g_rg[k].r[i] = (va_range){lo, hi}; g_rg[k].n++;
    }
    while (i + 1 < g_rg[k].n && g_rg[k].r[i + 1].lo <= g_rg[k].r[i].hi) {   // swallow the ranges it now reaches
        if (g_rg[k].r[i + 1].hi > g_rg[k].r[i].hi) g_rg[k].r[i].hi = g_rg[k].r[i + 1].hi;
        memmove(&g_rg[k].r[i + 1], &g_rg[k].r[i + 2], sizeof(va_range) * (g_rg[k].n - i - 2)); g_rg[k].n--;
    }
}
static uint64_t rg_take(int k, uint64_t n) {   // first fit
    for (int i = 0; i < g_rg[k].n; i++) if (g_rg[k].r[i].hi - g_rg[k].r[i].lo >= n) {
        uint64_t a = g_rg[k].r[i].lo; g_rg[k].r[i].lo += n;
        if (g_rg[k].r[i].lo == g_rg[k].r[i].hi) { memmove(&g_rg[k].r[i], &g_rg[k].r[i + 1], sizeof(va_range) * (g_rg[k].n - i - 1)); g_rg[k].n--; }
        return a;
    }
    return 0;
}
static uint64_t rg_largest(int k) { uint64_t m = 0; for (int i = 0; i < g_rg[k].n; i++) if (g_rg[k].r[i].hi - g_rg[k].r[i].lo > m) m = g_rg[k].r[i].hi - g_rg[k].r[i].lo; return m; }
// Every free chunk off every bucket's list, and the region's unused end, into the free ranges
static void harvest(int k) {
    uint64_t alloc = g_rg[k].alloc, region = g_rg[k].region, table = *(volatile uint64_t *)(uintptr_t)(g_main + CLASS_TABLE), got = 0; int chunks = 0;
    uint8_t seen[256] = {0};
    for (int c = 0; table && c < CLASSES; c++) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)(table + 32 * c);
        uint32_t size; memcpy(&size, e + 0x1c, 4); uint8_t b = e[0x18];
        if (!size || seen[b]) continue;
        seen[b] = 1;
        void *mtx = (void *)(uintptr_t)(alloc + 0x30ull * b + 0x58);
        volatile uint64_t *head = (volatile uint64_t *)(uintptr_t)(alloc + 0x30ull * b + 0x78); volatile uint32_t *cnt = (volatile uint32_t *)(uintptr_t)(alloc + 0x30ull * b + 0x80);
        g_lock(mtx);
        uint64_t prev = 0, d = *head;
        while (d) {
            const volatile uint64_t *dd = (const volatile uint64_t *)(uintptr_t)d; uint64_t next = dd[4], base = dd[0], mapped = dd[1], csize = dd[2];
            if (!mapped && base && csize) {
                if (prev) ((volatile uint64_t *)(uintptr_t)prev)[4] = next; else *head = next;
                if (*cnt) (*cnt)--;
                rg_add(k, base, base + csize); got += csize; chunks++;
            } else prev = d;
            d = next;
        }
        g_unlock(mtx);
    }
    _Atomic uint64_t *cur = (_Atomic uint64_t *)(uintptr_t)(region + 8); uint64_t base = *(volatile uint64_t *)(uintptr_t)region, end = *(volatile uint64_t *)(uintptr_t)(region + 0x18);
    uint64_t c = atomic_load(cur);
    while (c < end && !atomic_compare_exchange_weak(cur, &c, end)) {}
    if (c < end) { rg_add(k, base + c, base + end); got += end - c; }   // the end no new chunk could fit in
    atomic_fetch_add(&g_reclaimed, got);
    LOG("monolib allocator %#llx: %d free chunks and the region's end taken back, %.1f MB; free ranges %d, the largest %.1f MB", (unsigned long long)alloc, chunks, got / 1048576.0,
        g_rg[k].n, rg_largest(k) / 1048576.0);
}
static void region_full(const uint64_t *rc) {
    uint64_t *r = (uint64_t *)rc;   // _dm_gate reads x0..x29 back from here (patch_nso.py "Stubs")
    r[0] = 0;
    if (!g_main) {
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && !strcmp(g_mod[i].build_id, XC3_MAIN_V0)) g_main = (uint64_t)(uintptr_t)g_mod[i].base;
        g_lock = (void (*)(void *))guest_sym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"); g_unlock = (void (*)(void *))guest_sym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
        if (!g_main || !g_lock || !g_unlock) { LOG("[RED] monolib allocator: not Xenoblade Chronicles 3 1.0.0's main, or no nn::os mutex functions: the region stays used up"); g_main = 1; }
    }
    if (g_main == 1) return;
    uint64_t size = (uint32_t)r[9], region = r[10], alloc = r[20];
    if (*(volatile uint64_t *)(uintptr_t)(alloc + 0x28) != region) return;   // the failure path is also reached from main+0xf5618c, where x10 holds the region's base, not the region
    pthread_mutex_lock(&g_mu);
    int k = 0; while (k < g_nrg && g_rg[k].region != region) k++;
    if (k == g_nrg && g_nrg < REGIONS) { g_rg[k].region = region; g_rg[k].alloc = alloc; g_nrg++; }
    uint64_t a = 0;
    if (k < g_nrg) { a = rg_take(k, size); if (!a) { harvest(k); a = rg_take(k, size); } }
    uint64_t largest = k < g_nrg ? rg_largest(k) : 0;
    pthread_mutex_unlock(&g_mu);
    if (a) {
        r[11] = a; r[0] = 1; atomic_fetch_add((_Atomic uint64_t *)(uintptr_t)(region + 0x10), 1);
        uint64_t n = atomic_fetch_add(&g_served, 1); if (n < 20 || !(n & 255)) LOG("monolib allocator: a %.1f MB chunk (class %u) from taken-back addresses at %#llx (%llu so far)", size / 1048576.0, (unsigned)(uint8_t)r[24], (unsigned long long)a, (unsigned long long)n + 1);
    } else { uint64_t n = atomic_fetch_add(&g_failed, 1); if (n < 40) LOG("[RED] monolib allocator: no free range of %.1f MB (class %u) even after taking back the free chunks (the largest %.1f MB)", size / 1048576.0, (unsigned)(uint8_t)r[24], largest / 1048576.0); }
}
void monolib_alloc_report(void) {   // with the state poll's report
    if (g_served || g_failed) LOG("monolib allocator: %llu chunks from taken-back addresses, %llu with no range large enough, %.1f MB taken back", (unsigned long long)g_served, (unsigned long long)g_failed, g_reclaimed / 1048576.0);
}
__attribute__((constructor)) static void monolib_alloc_gate(void) { gate_obs_set(OBS_REGION_FULL, region_full); }
