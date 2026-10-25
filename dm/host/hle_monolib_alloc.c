// Xenoblade Chronicles 3 1.0.0's large-block allocator, address region used up (observation gate 31, the stub of dm/games/010074F013262000/patches.py at main+0xf56234).
// The allocator (main+0xf55fb4, object x20 = G + 0xa80) keeps per size bucket a list of free chunks and takes a new chunk from its address region (object [x20 + 0x28] = G + 8,
// main+0x1864a08: +0 base, +8 offset in use, +0x10 chunks, +0x18 size) by moving the offset forward; a freed chunk (main+0xf54efc) goes back to its bucket with its memory unmapped
// and its addresses kept. Built for a 60 GB region, at 4 GB the offset reached the end after the buckets' peaks, and every later new chunk failed (the opening movie's 65 / 128 MB,
// a new map's 193 MB terrain, 16 MB character data); 2.5 GB of the 3.2 GB pool were mapped then.
// Past the end, the callback takes an address region of its own (nn::os::AllocateAddressRegion, 512 MB or the chunk's size) above the region and grows the region over it: the
// offset moves to its start, then the size to its end, and the allocator bumps again. Whatever lies between (the SDK's reserve region) is never handed out. The region has to
// stay one range: operator delete (main+0xf9e3bc) frees a block through the large-block allocator only when main+0xf54320 finds it in [base, base + size), and the chunk
// queries at main+0xfa1ca4.. check the same. Chunks served from outside it (the previous version) played the opening movie in a 128 MB chunk, and its end crashed in the movie
// heap's free (main+0xf9bad8, the chunk's first page unmapped). Chunks are not taken back from the buckets: the allocator finds a block's chunk by a binary search over descriptors
// in address order, and an orphaned descriptor beside a new one at its addresses sent a later free to the wrong one (a crash in main+0xfa19cc). The profile's alias region has
// room for the extra regions (mem_alias). Counters in the log (DM_STATEPOLL=1 reports them with the state)
#include "hle.h"
#include <stdatomic.h>

#define XC3_MAIN_V0 "60887a25b66529910275a1066003a002"
enum { EXTRA = 512 << 20 };
static int g_ready;   // 1 = XC3 1.0.0 with the sdk function, -1 = not
static uint32_t (*g_alloc_region)(uint64_t *, uint64_t);
static _Atomic uint64_t g_regions, g_bytes, g_failed;

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
    // the allocator's lock is held (x25): only this allocator bumps the region after start-up (the small-object arena took its part at main+0xf54048)
    _Atomic uint64_t *off = (_Atomic uint64_t *)(uintptr_t)(region + 8), *rsize = (_Atomic uint64_t *)(uintptr_t)(region + 0x18);
    uint64_t rbase = *(volatile uint64_t *)(uintptr_t)region, rend = rbase + atomic_load(rsize);
    uint64_t n = ((size + 0xffff) & ~0xffffull) + 0x10000, base = 0;   // the bump needs its end strictly below the size (main+0xf56144)
    if (n < EXTRA) n = EXTRA;
    uint32_t rs = g_alloc_region(&base, n);
    if (rs || !base) {
        if (atomic_fetch_add(&g_failed, 1) < 40) LOG("[RED] monolib allocator: nn::os::AllocateAddressRegion of %.1f MB failed (result %#x): the alias region is full (profile mem_alias), a %.1f MB chunk (class %u) fails",
                                                     n / 1048576.0, rs, size / 1048576.0, (unsigned)(uint8_t)r[24]);
        return;
    }
    if (base < rend) {   // the region can only grow upwards (the extra region stays reserved, unused)
        if (atomic_fetch_add(&g_failed, 1) < 40) LOG("[RED] monolib allocator: an extra region at %#llx lies below the allocator's region end %#llx: a %.1f MB chunk (class %u) fails",
                                                     (unsigned long long)base, (unsigned long long)rend, size / 1048576.0, (unsigned)(uint8_t)r[24]);
        return;
    }
    atomic_store(off, base - rbase);       // first the offset past the gap, then the size: a bump in between sees an offset beyond the old end and fails
    atomic_store(rsize, base + n - rbase);
    atomic_fetch_add(&g_regions, 1); atomic_fetch_add(&g_bytes, n);
    LOG("monolib allocator %#llx: its region (%#llx..%#llx) is used up; grown over an extra region of %.1f MB at %#llx for a %.1f MB chunk (class %u)", (unsigned long long)alloc,
        (unsigned long long)rbase, (unsigned long long)rend, n / 1048576.0, (unsigned long long)base, size / 1048576.0, (unsigned)(uint8_t)r[24]);
    r[0] = 1;
}
void monolib_alloc_report(void) {   // with the state poll's report
    if (g_regions || g_failed) LOG("monolib allocator: region grown by %llu extra regions (%.1f MB), %llu chunks failed", (unsigned long long)g_regions, g_bytes / 1048576.0, (unsigned long long)g_failed);
}
__attribute__((constructor)) static void monolib_alloc_gate(void) { gate_obs_set(OBS_REGION_FULL, region_full); }
