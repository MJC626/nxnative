// Xenoblade Chronicles 3 1.0.0's large-block allocator, address region used up (observation gate 31, the stub of dm/games/010074F013262000/patches.py at main+0xf56234).
// The allocator (main+0xf55fb4, object x20 = G + 0xa80) keeps per size bucket a list of free chunks and takes a new chunk from its address region (object [x20 + 0x28] = G + 8,
// main+0x1864a08: +0 base, +8 offset in use, +0x10 chunks, +0x18 size) by moving the offset forward; a freed chunk (main+0xf54efc) goes back to its bucket with its memory unmapped
// and its addresses kept. Built for a 60 GB region, at 4 GB the offset reached the end after the buckets' peaks, and every later new chunk failed (the opening movie's 65 / 128 MB,
// a new map's 193 MB terrain, 16 MB character data); 2.5 GB of the 3.2 GB pool were mapped then.
// Past the end the callback first reuses addresses: a run of free chunks lying side by side (descriptors in a row, in buckets, gaps under 64 KB) becomes one chunk of the size
// asked for. Its first descriptor takes the size and class, the others leave their buckets and stay in the array unused, so no descriptor moves: the allocator finds a chunk by a
// binary search over descriptors in address order (an orphaned descriptor out of that order, in a version that took chunks back to make new ones, sent a later free to the wrong
// chunk: a crash in main+0xfa19cc), and the bucket lists point at descriptors. A whole run of the region played through to the second map with 4.75 GB of addresses used and 2.25 GB
// mapped; 256 MB chunks then failed 2,721 times.
// Without such a run, it takes an address region of its own (nn::os::AllocateAddressRegion, 512 MB or the chunk's size) above the region and grows the region over it: the offset
// moves to its start, then the size to its end, and the allocator bumps again. Whatever lies between (the SDK's reserve region) is never handed out. The region has to stay one
// range: operator delete (main+0xf9e3bc) frees a block through the large-block allocator only when main+0xf54320 finds it in [base, base + size), and the chunk queries at
// main+0xfa1ca4.. check the same (chunks served from outside it played the opening movie, and its end crashed in the movie heap's free, main+0xf9bad8). The profile's alias region
// has room for one or two (mem_alias). Counters in the log (DM_STATEPOLL=1 reports them with the state)
#include "hle.h"
#include <sched.h>
#include <stdatomic.h>

#define XC3_MAIN_V0 "60887a25b66529910275a1066003a002"
enum { EXTRA = 512 << 20, NBUCKET = 16, DESC = 0x28 };
static int g_ready;   // 1 = XC3 1.0.0 with the sdk functions, -1 = not
static uint32_t (*g_alloc_region)(uint64_t *, uint64_t);
static int (*g_trylock)(uint64_t);
static void (*g_unlock)(uint64_t);
static _Atomic uint64_t g_regions, g_bytes, g_failed, g_joins, g_joined, g_join_bytes;
static uint64_t g_ext[16]; static int g_next_ext;   // bases of the extra regions (a run of free chunks never crosses one)

// Descriptor (array [alloc + 0x30], count [alloc + 0x38]): +0 base, +8 bytes mapped, +0x10 size, +0x18 class, +0x1c, +0x20 next in a bucket's list.
// Bucket b (class table entry +0x18): its lock at alloc + 0x58 + b * 0x30, list head +0x78, count +0x80 (u32), chunks made +0x84
#define D64(d, o) (*(volatile uint64_t *)(uintptr_t)((d) + (o)))
static uint64_t bucket(uint64_t alloc, int b) { return alloc + 0x58 + (uint64_t)b * 0x30; }

// A run of free chunks covering size bytes, joined into one; returns its descriptor, 0 = none (or the buckets could not all be locked)
static uint64_t join_free(uint64_t alloc, uint64_t size, unsigned cls) {
    uint64_t D = D64(alloc, 0x30); uint32_t N = *(volatile uint32_t *)(uintptr_t)(alloc + 0x38);
    if (!D || N < 2) return 0;
    int locked = 0;
    for (; locked < NBUCKET; locked++) {
        int k = 0; while (!g_trylock(bucket(alloc, locked)) && ++k < 20000) sched_yield();
        if (k == 20000) break;
    }
    uint64_t got = 0; uint8_t *fb = NULL;
    if (locked < NBUCKET) { LOG("[RED] monolib allocator: bucket %d stayed locked; no free chunks joined this time", locked); goto out; }
    fb = calloc(N, 1);   // bucket + 1 of each free descriptor
    if (!fb) goto out;
    for (int b = 0; b < NBUCKET; b++) {
        uint32_t n = 0;
        for (uint64_t e = D64(bucket(alloc, b), 0x20); e; e = D64(e, 0x20)) {
            if (e < D || e >= D + (uint64_t)N * DESC || (e - D) % DESC || ++n > N) { LOG("[RED] monolib allocator: bucket %d's list leaves the descriptor array (%#llx); no free chunks joined", b, (unsigned long long)e); goto out; }
            if (!D64(e, 8)) fb[(e - D) / DESC] = (uint8_t)(b + 1);   // a free chunk has nothing mapped (main+0xf54f50)
        }
    }
    // the run wasting the least: two pointers over each stretch of free chunks side by side. Descriptors a join left unused (size 0) are skipped; the addresses they had
    // lie between their neighbours and belong to no chunk, so free chunks on both sides still count as side by side. A stretch never crosses the base of an extra region
    // (the gap the region grew over) or a live chunk
    uint32_t best_lo = 0, best_hi = 0, lo = 0, prev = 0; uint64_t best_waste = ~0ull; int in = 0, zgap = 0;
    for (uint32_t k = 0; k < N; k++) {
        uint64_t dk = D + (uint64_t)k * DESC, base = D64(dk, 0), sz = D64(dk, 0x10);
        if (!sz && !D64(dk, 8)) { zgap = 1; continue; }   // left unused by a join
        if (!fb[k]) { in = 0; continue; }                   // a live chunk
        if (in) {
            uint64_t dp = D + (uint64_t)prev * DESC, pe = D64(dp, 0) + D64(dp, 0x10);
            int side = base >= pe && (base - pe < 0x10000 || zgap);
            for (int x = 0; x < g_next_ext && side; x++) if (g_ext[x] > D64(dp, 0) && g_ext[x] <= base) side = 0;
            if (!side) in = 0;
        }
        if (!in) { in = 1; lo = k; }
        prev = k; zgap = 0;
        uint64_t end = base + sz;
        while (lo < k) {   // advance lo to the next free descriptor while the rest still covers the size
            uint32_t nx = lo + 1; while (nx < k && !fb[nx]) nx++;
            if (end - D64(D + (uint64_t)nx * DESC, 0) < size) break;
            lo = nx;
        }
        uint64_t lb = D64(D + (uint64_t)lo * DESC, 0);
        if (end - lb >= size && end - lb - size < best_waste) { best_waste = end - lb - size; best_lo = lo; best_hi = k; }
    }
    if (best_waste == ~0ull) goto out;
    for (int b = 0; b < NBUCKET; b++) {   // the run leaves its buckets
        uint64_t *link = (uint64_t *)(uintptr_t)(bucket(alloc, b) + 0x20), e = *link; uint32_t gone = 0;
        while (e) {
            uint32_t k = (uint32_t)((e - D) / DESC); uint64_t nx = D64(e, 0x20);
            if (k >= best_lo && k <= best_hi) { *link = nx; gone++; } else link = (uint64_t *)(uintptr_t)(e + 0x20);
            e = nx;
        }
        if (gone) *(volatile uint32_t *)(uintptr_t)(bucket(alloc, b) + 0x28) -= gone;
    }
    for (uint32_t k = best_lo + 1; k <= best_hi; k++) { uint64_t dk = D + (uint64_t)k * DESC; D64(dk, 0x20) = 0; D64(dk, 8) = 0; D64(dk, 0x10) = 0; }   // left unused: size 0
    got = D + (uint64_t)best_lo * DESC;
    D64(got, 8) = 0; D64(got, 0x10) = size; *(volatile uint8_t *)(uintptr_t)(got + 0x18) = (uint8_t)cls; D64(got, 0x20) = 0;
    uint64_t n = atomic_fetch_add(&g_joins, 1); atomic_fetch_add(&g_joined, best_hi - best_lo + 1); atomic_fetch_add(&g_join_bytes, size);
    if (n < 20 || !(n & 255)) LOG("monolib allocator: %u free chunks joined at %#llx into a %.1f MB chunk (class %u), %.1f MB left unused at its end (%llu joins so far)",
                                  best_hi - best_lo + 1, (unsigned long long)D64(got, 0), size / 1048576.0, cls, best_waste / 1048576.0, (unsigned long long)n + 1);
out:
    free(fb);
    while (locked > 0) g_unlock(bucket(alloc, --locked));
    return got;
}

// The region grows over an extra address region above it; 0 = no room
static int grow(uint64_t region, uint64_t alloc, uint64_t size, unsigned cls) {
    _Atomic uint64_t *off = (_Atomic uint64_t *)(uintptr_t)(region + 8), *rsize = (_Atomic uint64_t *)(uintptr_t)(region + 0x18);
    uint64_t rbase = D64(region, 0), rend = rbase + atomic_load(rsize);
    uint64_t need = ((size + 0xffff) & ~0xffffull) + 0x10000, n = need < EXTRA ? EXTRA : need, base = 0;   // the bump needs its end strictly below the size (main+0xf56144)
    uint32_t rs = g_alloc_region(&base, n);
    if ((rs || !base) && n > need) { n = need; base = 0; rs = g_alloc_region(&base, n); }   // what is left of the alias region
    if (rs || !base) {
        if (atomic_fetch_add(&g_failed, 1) < 40) LOG("[RED] monolib allocator: no free chunks to join and nn::os::AllocateAddressRegion of %.1f MB failed (result %#x): the alias region is full (profile mem_alias), a %.1f MB chunk (class %u) fails",
                                                     n / 1048576.0, rs, size / 1048576.0, cls);
        return 0;
    }
    if (base < rend) {   // the region can only grow upwards (the extra region stays reserved, unused)
        if (atomic_fetch_add(&g_failed, 1) < 40) LOG("[RED] monolib allocator: an extra region at %#llx lies below the allocator's region end %#llx: a %.1f MB chunk (class %u) fails",
                                                     (unsigned long long)base, (unsigned long long)rend, size / 1048576.0, cls);
        return 0;
    }
    atomic_store(off, base - rbase);       // first the offset past the gap, then the size: a bump in between sees an offset beyond the old end and fails
    atomic_store(rsize, base + n - rbase);
    atomic_fetch_add(&g_regions, 1); atomic_fetch_add(&g_bytes, n);
    if (g_next_ext < 16) g_ext[g_next_ext++] = base;
    LOG("monolib allocator %#llx: its region (%#llx..%#llx) is used up; grown over an extra region of %.1f MB at %#llx for a %.1f MB chunk (class %u)", (unsigned long long)alloc,
        (unsigned long long)rbase, (unsigned long long)rend, n / 1048576.0, (unsigned long long)base, size / 1048576.0, cls);
    return 1;
}

static void region_full(const uint64_t *rc) {
    uint64_t *r = (uint64_t *)rc;   // _dm_gate reads x0..x29 back from here (patch_nso.py "Stubs")
    r[0] = 0;
    if (!g_ready) {
        int xc3 = 0; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && !strcmp(g_mod[i].build_id, XC3_MAIN_V0)) xc3 = 1;
        g_alloc_region = (uint32_t (*)(uint64_t *, uint64_t))guest_sym("_ZN2nn2os21AllocateAddressRegionEPmm");
        g_trylock = (int (*)(uint64_t))guest_sym("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE");
        g_unlock = (void (*)(uint64_t))guest_sym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
        g_ready = xc3 && g_alloc_region && g_trylock && g_unlock ? 1 : -1;
        if (g_ready < 0) LOG("[RED] monolib allocator: not Xenoblade Chronicles 3 1.0.0's main, or nn::os functions missing: the region stays used up");
    }
    if (g_ready < 0) return;
    uint64_t size = (uint32_t)r[9], region = r[10], alloc = r[20], b = r[26];
    unsigned cls = (uint8_t)r[24];
    if (D64(alloc, 0x28) != region) return;   // the failure path is also reached from main+0xf5618c, where x10 holds the region's base, not the region
    // the allocator's lock is held (x25); the asked class's bucket lock is not (x19, released at main+0xf560fc)
    if (b < NBUCKET && r[19] == bucket(alloc, (int)b) && r[27] == bucket(alloc, (int)b) + 0x20) {
        uint64_t d = join_free(alloc, size, cls);
        if (d) { r[28] = d; g_unlock(r[25]); r[0] = 2; return; }   // main+0xf56060 goes on without the allocator's lock
    } else LOG("[RED] monolib allocator: unexpected registers at the region's end (bucket %llu, x19 %#llx, x27 %#llx): no free chunks joined", (unsigned long long)b, (unsigned long long)r[19], (unsigned long long)r[27]);
    // only this allocator bumps the region after start-up (the small-object arena took its part at main+0xf54048)
    if (grow(region, alloc, size, cls)) r[0] = 1;
}
void monolib_alloc_report(void) {   // with the state poll's report
    if (g_joins || g_regions || g_failed)
        LOG("monolib allocator: %llu chunks made of %llu free ones (%.1f MB), region grown by %llu extra regions (%.1f MB), %llu chunks failed", (unsigned long long)g_joins,
            (unsigned long long)g_joined, g_join_bytes / 1048576.0, (unsigned long long)g_regions, g_bytes / 1048576.0, (unsigned long long)g_failed);
}
__attribute__((constructor)) static void monolib_alloc_gate(void) { gate_obs_set(OBS_REGION_FULL, region_full); }
