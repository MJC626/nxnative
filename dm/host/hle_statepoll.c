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
static void sp_hit(uint64_t obj, uint32_t res) {
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
    }
    pthread_mutex_unlock(&g_sp_lock);
}
static void sp_child(const uint64_t *r) { uint64_t obj = 0; if (rd64(r[21], &obj)) sp_hit(obj, (uint32_t)r[0]); }
static void sp_cur(const uint64_t *r) { uint64_t end = 0, obj = 0; if (rd64(r[19] + 0x20, &end) && rd64(end - 0x10, &obj)) sp_hit(obj, (uint32_t)r[0]); }
__attribute__((constructor)) static void statepoll_gates(void) {
    const char *e = getenv("DM_STATEPOLL"); if (!e || !*e || *e == '0') return;
    gate_obs_set(OBS_STATE_CHILD, sp_child); gate_obs_set(OBS_STATE_CUR, sp_cur);
}
