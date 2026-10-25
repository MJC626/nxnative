// Xenoblade Chronicles 3 1.0.0's large-block allocator, address region used up (observation gate 31, the stub of dm/games/010074F013262000/patches.py at main+0xf56234).
// The allocator (main+0xf55fb4, object x20) keeps per size bucket a list of free chunks and takes a new chunk from its address region (object [x20 + 0x28]: +0 base, +8 offset in
// use, +0x10 chunks, +0x18 end) by moving the offset forward; a freed chunk (main+0xf54efc) goes back to its bucket with its memory unmapped and its addresses kept. Built for a
// 60 GB region, at 4 GB the offset reached the end after the buckets' peaks, and every later new chunk failed (the opening movie's 65 / 128 MB, a new map's 193 MB terrain,
// 16 MB character data); 2.5 GB of the 3.2 GB pool were mapped then.
// Past the end, the callback goes on in address regions of its own (nn::os::AllocateAddressRegion, 512 MB or the chunk's size) in the same way: forward, 64 KB aligned. Chunks
// are not taken back from the buckets: the allocator finds a block's chunk by its descriptor, and a descriptor array whose bases stop rising (an orphaned descriptor beside a new
// one at its addresses) sent a later free to the wrong descriptor (a crash in main+0xfa19cc). Every extra region lies above the allocator's own, so new chunks keep rising
// as the original offset did. The profile's alias region has room for them (mem_alias). Counters in the log (DM_STATEPOLL=1 reports them with the state)
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>

#define XC3_MAIN_V0 "60887a25b66529910275a1066003a002"
enum { EXTRA = 512 << 20 };
static struct { uint64_t region, lo, hi; } g_ext[8];   // per allocator region: the extra region in use, [lo, hi) left of it
static int g_next;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;   // 1 = XC3 1.0.0 with the sdk function, -1 = not
static uint32_t (*g_alloc_region)(uint64_t *, uint64_t);
static _Atomic uint64_t g_served, g_failed, g_regions, g_bytes;

static void region_full(const uint64_t *rc) {
    uint64_t *r = (uint64_t *)rc;   // _dm_gate reads x0..x29 back from here (patch_nso.py "Stubs")
    r[0] = 0;
    if (!g_ready) {
        int xc3 = 0; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && !strcmp(g_mod[i].build_id, XC3_MAIN_V0)) xc3 = 1;
        g_alloc_region = (uint32_t (*)(uint64_t *, uint64_t))guest_sym("_ZN2nn2os21AllocateAddressRegionEPmm");
        g_ready = xc3 && g_alloc_region ? 1 : -1;
        if (g_ready < 0) LOG("[RED] monolib allocator: not Xenoblade Chronicles 3 1.0.0's main, or no nn::os::AllocateAddressRegion: the region stays used up");
    }
    if (g_ready < 0) return;
    uint64_t size = (uint32_t)r[9], region = r[10], alloc = r[20];
    if (*(volatile uint64_t *)(uintptr_t)(alloc + 0x28) != region) return;   // the failure path is also reached from main+0xf5618c, where x10 holds the region's base, not the region
    uint64_t rbase = *(volatile uint64_t *)(uintptr_t)region, rend = rbase + *(volatile uint64_t *)(uintptr_t)(region + 0x18);
    pthread_mutex_lock(&g_mu);
    int k = 0; while (k < g_next && g_ext[k].region != region) k++;
    if (k == g_next && g_next < 8) { g_ext[k].region = region; g_ext[k].lo = g_ext[k].hi = 0; g_next++; }
    uint64_t a = 0;
    if (k < g_next) {
        uint64_t at = (g_ext[k].lo + 0xffff) & ~0xffffull;
        if (!g_ext[k].hi || at + size > g_ext[k].hi) {   // a new extra region (the rest of the last one stays unused: chunks only rise)
            uint64_t n = size > EXTRA ? (size + 0xffff) & ~0xffffull : EXTRA, base = 0;
            uint32_t rs = g_alloc_region(&base, n);
            if (rs || !base) LOG("[RED] monolib allocator: nn::os::AllocateAddressRegion of %.1f MB failed (result %#x): the alias region is full (profile mem_alias)", n / 1048576.0, rs);
            else if (base < rend || base < g_ext[k].hi) LOG("[RED] monolib allocator: an extra region at %#llx lies below the allocator's chunks (region end %#llx): not used", (unsigned long long)base, (unsigned long long)rend);
            else { g_ext[k].lo = base; g_ext[k].hi = base + n; at = base; atomic_fetch_add(&g_regions, 1); atomic_fetch_add(&g_bytes, n);
                   LOG("monolib allocator %#llx: its region (%#llx..%#llx) is used up; an extra region of %.1f MB at %#llx", (unsigned long long)alloc, (unsigned long long)rbase, (unsigned long long)rend, n / 1048576.0, (unsigned long long)base); }
        }
        if (g_ext[k].hi && at + size <= g_ext[k].hi) { a = at; g_ext[k].lo = at + size; }
    }
    pthread_mutex_unlock(&g_mu);
    if (a) {
        r[11] = a; r[0] = 1; atomic_fetch_add((_Atomic uint64_t *)(uintptr_t)(region + 0x10), 1);
        uint64_t n = atomic_fetch_add(&g_served, 1); if (n < 20 || !(n & 255)) LOG("monolib allocator: a %.1f MB chunk (class %u) at %#llx in an extra region (%llu so far)", size / 1048576.0, (unsigned)(uint8_t)r[24], (unsigned long long)a, (unsigned long long)n + 1);
    } else { uint64_t n = atomic_fetch_add(&g_failed, 1); if (n < 40) LOG("[RED] monolib allocator: no room for a %.1f MB chunk (class %u)", size / 1048576.0, (unsigned)(uint8_t)r[24]); }
}
void monolib_alloc_report(void) {   // with the state poll's report
    if (g_served || g_failed) LOG("monolib allocator: %llu chunks in %llu extra regions (%.1f MB), %llu with no room", (unsigned long long)g_served, (unsigned long long)g_regions, g_bytes / 1048576.0, (unsigned long long)g_failed);
}
__attribute__((constructor)) static void monolib_alloc_gate(void) { gate_obs_set(OBS_REGION_FULL, region_full); }
