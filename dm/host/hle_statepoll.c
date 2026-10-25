// State poll tracer (DM_STATEPOLL=1): observation gates the rewriter puts after a game's per-state update call (patch_nso.py OBS 21 / 22; Xenoblade Chronicles 3 1.0.0: the scene framework's
// vtable +0x40 update, 1 = done). Counts updates per state class (vtable, with the C++ class name from RTTI) and how many returned done; every 10 s logs the classes updated since the last report.
// A loading screen that never ends shows the state that keeps answering "not done"
#include "dm.h"
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define SP_N 512
typedef struct { uint64_t vt; uint32_t n, done, last_res, reported; } sp_ent_t;
static sp_ent_t g_sp[SP_N];
static pthread_mutex_t g_sp_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_sp_next_ns;

static int rd64(uint64_t a, uint64_t *v) { vm_size_t g = 0; return a > 0x100000000ull && !vm_read_overwrite(mach_task_self(), a, 8, (vm_address_t)v, &g) && g == 8; }
static void class_name(uint64_t vt, char *out, size_t n) {   // Itanium ABI: vtable[-1] = type_info, type_info +8 = mangled name
    uint64_t ti = 0, nm = 0; char buf[96] = {0}; vm_size_t g = 0;
    if (rd64(vt - 8, &ti) && rd64(ti + 8, &nm) && nm > 0x100000000ull) vm_read_overwrite(mach_task_self(), nm, sizeof buf - 1, (vm_address_t)buf, &g);
    buf[sizeof buf - 1] = 0; snprintf(out, n, "%s", buf[0] ? buf : "?");
}
static void snd_report(void);
static _Atomic uint64_t g_al_ok[2], g_al_fail[2];   // guest allocator results (al_hit)
static int sp_on(void) {   // read on the first hit, not in the constructor: on iOS Documents/env.txt is applied in main(), after constructors run
    static int on = -1; if (on < 0) { const char *e = getenv("DM_STATEPOLL"); on = e && *e && *e != '0'; if (on) LOG("state poll: on"); } return on;
}
static void sp_hit(uint64_t obj, uint32_t res) {
    if (!sp_on()) return;
    uint64_t vt = 0; if (!rd64(obj, &vt)) return;
    pthread_mutex_lock(&g_sp_lock);
    unsigned s = (unsigned)((vt * 0x9E3779B97F4A7C15ull) >> 55) % SP_N, k = 0;
    while (g_sp[s].vt && g_sp[s].vt != vt && k++ < SP_N) s = (s + 1) % SP_N;
    if (k < SP_N) { g_sp[s].vt = vt; g_sp[s].n++; g_sp[s].done += res == 1; g_sp[s].last_res = res; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    if (!g_sp_next_ns) g_sp_next_ns = now + 10000000000ull;
    if (now >= g_sp_next_ns) {
        g_sp_next_ns = now + 10000000000ull;
        LOG("state poll: classes updated in the last 10 s (updates since the last report / total, done total, last result):");
        for (int i = 0; i < SP_N; i++) if (g_sp[i].vt && g_sp[i].n != g_sp[i].reported) {
            char nm[96], vs[64]; class_name(g_sp[i].vt, nm, sizeof nm); module_t *m = module_of((void *)g_sp[i].vt);
            if (m) snprintf(vs, sizeof vs, "%s+%#llx", m->name, (unsigned long long)(g_sp[i].vt - (uint64_t)m->base)); else snprintf(vs, sizeof vs, "%#llx", (unsigned long long)g_sp[i].vt);
            LOG("state poll: %-22s %6u / %7u done %6u last %u  %s", vs, g_sp[i].n - g_sp[i].reported, g_sp[i].n, g_sp[i].done, g_sp[i].last_res, nm);
            g_sp[i].reported = g_sp[i].n;
        }
        snd_report();
        LOG("state poll: guest allocations so far: small %llu ok / %llu failed, large %llu ok / %llu failed", (unsigned long long)g_al_ok[0], (unsigned long long)g_al_fail[0], (unsigned long long)g_al_ok[1], (unsigned long long)g_al_fail[1]);
    }
    pthread_mutex_unlock(&g_sp_lock);
}
// Sound waits (OBS 23, after main+0x502f4c "is this sound still playing"): per call site, how often it asked and how often the answer was still playing, with the last sound id.
// A state that waits for a sound to end (play then poll, main+0x6f8784) shows here with every answer "still playing"
typedef struct { uint64_t caller; uint32_t n, playing, last_id, reported; } snd_ent_t;
static snd_ent_t g_snd[256];
static void snd_report(void) {   // holding g_sp_lock
    int any = 0;
    for (int i = 0; i < 256; i++) if (g_snd[i].caller && g_snd[i].n != g_snd[i].reported) {
        if (!any++) LOG("state poll: sound waits in the last 10 s (call site, queries since the last report / total, still playing total, last sound id):");
        module_t *m = module_of((void *)g_snd[i].caller); char cs[64];
        if (m) snprintf(cs, sizeof cs, "%s+%#llx", m->name, (unsigned long long)(g_snd[i].caller - (uint64_t)m->base)); else snprintf(cs, sizeof cs, "%#llx", (unsigned long long)g_snd[i].caller);
        LOG("state poll: sound %-18s %6u / %7u playing %7u id %#x", cs, g_snd[i].n - g_snd[i].reported, g_snd[i].n, g_snd[i].playing, g_snd[i].last_id);
        g_snd[i].reported = g_snd[i].n;
    }
}
static void sp_sound(const uint64_t *r) {
    if (!sp_on()) return;
    uint64_t caller = 0, id = 0; if (!rd64(r[29] + 8, &caller)) return; rd64(r[19] + 8, &id);
    pthread_mutex_lock(&g_sp_lock);
    unsigned s = (unsigned)((caller * 0x9E3779B97F4A7C15ull) >> 56), k = 0;
    while (g_snd[s].caller && g_snd[s].caller != caller && k++ < 256) s = (s + 1) & 255;
    if (k < 256) { g_snd[s].caller = caller; g_snd[s].n++; g_snd[s].playing += (r[0] & 1); g_snd[s].last_id = (uint32_t)id; }
    pthread_mutex_unlock(&g_sp_lock);
}
// Opening movie chain (OBS 24-26, Xenoblade Chronicles 3: New Game starts event ev01005100, whose movie is movie/mv01005100.webm): the first 40 hits of each gate are logged, then one line
// per change of what they see, so the log shows how far the chain gets
static void rdstr(uint64_t a, char *out, size_t n) { vm_size_t g = 0; out[0] = 0; if (a > 0x100000000ull && !vm_read_overwrite(mach_task_self(), a, n - 1, (vm_address_t)out, &g)) out[n - 1] = 0; else out[0] = 0;
    for (size_t i = 0; i < n && out[i]; i++) if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) { out[i] = 0; break; } }
static uint32_t rd32(uint64_t a) { uint32_t v = 0; vm_size_t g = 0; if (a > 0x100000000ull) vm_read_overwrite(mach_task_self(), a, 4, (vm_address_t)&v, &g); return v; }
static void mv_update(const uint64_t *r) {
    if (!sp_on()) return;
    static int n; static uint64_t last_key; uint64_t h = 0; rd64(r[0] + 0x218, &h); uint32_t st = rd32(r[0] + 0x80);
    uint64_t key = r[0] ^ ((uint64_t)st << 48) ^ (h ? 1ull << 63 : 0);
    if (n < 40 || key != last_key) { n++; last_key = key; char nm[48]; uint64_t np = 0; rd64(r[0] + 0x220, &np); rdstr(np, nm, sizeof nm);
        LOG("movie chain: player update %#llx state %u handle(+0x218) %#llx name(+0x220) '%s'", (unsigned long long)r[0], st, (unsigned long long)h, nm); }
}
static void mv_request(const uint64_t *r) {
    if (!sp_on()) return;
    static int n; if (n++ >= 40) return;
    char nm[48]; rdstr(r[1] + 0x10, nm, sizeof nm);
    LOG("movie chain: request %#llx name '%s' flags +0x34 %u +0x38 %u", (unsigned long long)r[1], nm, rd32(r[1] + 0x34), rd32(r[1] + 0x38));
}
static void mv_open(const uint64_t *r) {
    if (!sp_on()) return;
    static int n; if (n++ >= 40) return;
    char p[96]; rdstr(r[2], p, sizeof p); uint64_t h = 0; rd64(r[0] + 0x40, &h);
    LOG("movie chain: open player %#llx path '%s' (+0x40 %#llx: %s)", (unsigned long long)r[0], p, (unsigned long long)h, h ? "set" : "null, the open returns false");
}
// Guest allocator results (OBS 27 / 28, Xenoblade Chronicles 3: the large-block and small-object paths of main+0xf55764): every failed allocation (first 200) with its size class, size and the
// code that asked; every 10 s with the state report, counts of successes and failures. A loader stuck after reading a map's headers may be waiting for memory that never comes
// Successful large blocks per size class (count, bytes, lowest / highest block address; frees are not seen, so bytes are what was handed out, not what is held), printed at the first
// large failure: the 65 MB blocks (class 42) the opening movie asks for fail in a 4 GB region (docs/xenoblade3.md, memory layout), and this tells which classes took the space
enum { AL_CLASSES = 64 };
static _Atomic uint64_t g_al_cls_n[AL_CLASSES], g_al_cls_bytes[AL_CLASSES], g_al_cls_lo[AL_CLASSES], g_al_cls_hi[AL_CLASSES];
static void al_note(const uint64_t *r) {
    unsigned c = (unsigned)r[27]; if (c >= AL_CLASSES) return;
    atomic_fetch_add(&g_al_cls_n[c], 1); atomic_fetch_add(&g_al_cls_bytes[c], r[25]);
    uint64_t lo = atomic_load(&g_al_cls_lo[c]); while ((!lo || r[0] < lo) && !atomic_compare_exchange_weak(&g_al_cls_lo[c], &lo, r[0])) {}
    uint64_t hi = atomic_load(&g_al_cls_hi[c]); while (r[0] > hi && !atomic_compare_exchange_weak(&g_al_cls_hi[c], &hi, r[0])) {}
}
static void al_table(void) {
    uint64_t total = 0;
    for (unsigned c = 0; c < AL_CLASSES; c++) if (g_al_cls_n[c]) {
        total += g_al_cls_bytes[c];
        LOG("state poll: large blocks of class %u so far: %llu, %.1f MB handed out, addresses %#llx..%#llx", c, (unsigned long long)g_al_cls_n[c], g_al_cls_bytes[c] / 1048576.0, (unsigned long long)g_al_cls_lo[c], (unsigned long long)g_al_cls_hi[c]);
    }
    LOG("state poll: large blocks handed out so far: %.1f MB in all", total / 1048576.0);
}
static void al_hit(const uint64_t *r, int large) {
    if (!sp_on()) return;
    if (r[0]) { atomic_fetch_add(&g_al_ok[large], 1); if (large) al_note(r); return; }
    uint64_t n = atomic_fetch_add(&g_al_fail[large], 1);
    if (large && !n) al_table();
    if (n < 200) { uint64_t caller = 0; rd64(r[29] + 8, &caller); module_t *m = module_of((void *)caller); char cs[64];
        if (m) snprintf(cs, sizeof cs, "%s+%#llx", m->name, (unsigned long long)(caller - (uint64_t)m->base)); else snprintf(cs, sizeof cs, "%#llx", (unsigned long long)caller);
        LOG("[RED] guest allocation failed (%s): size class %llu, size %#llx, asked from %s", large ? "large block" : "small object", (unsigned long long)r[27], (unsigned long long)r[25], cs); }
}
static void al_large(const uint64_t *r) { al_hit(r, 1); }
static void al_small(const uint64_t *r) { al_hit(r, 0); }
static void sp_child(const uint64_t *r) { uint64_t obj = 0; if (rd64(r[21], &obj)) sp_hit(obj, (uint32_t)r[0]); }
static void sp_cur(const uint64_t *r) { uint64_t end = 0, obj = 0; if (rd64(r[19] + 0x20, &end) && rd64(end - 0x10, &obj)) sp_hit(obj, (uint32_t)r[0]); }
__attribute__((constructor)) static void statepoll_gates(void) {
    gate_obs_set(OBS_STATE_CHILD, sp_child); gate_obs_set(OBS_STATE_CUR, sp_cur); gate_obs_set(OBS_SOUND_PLAYING, sp_sound); gate_obs_set(OBS_MOVIE_UPDATE, mv_update); gate_obs_set(OBS_MOVIE_REQUEST, mv_request); gate_obs_set(OBS_MOVIE_OPEN, mv_open); gate_obs_set(OBS_ALLOC_LARGE, al_large); gate_obs_set(OBS_ALLOC_SMALL, al_small);
}
