/* Live on/off (live.h). The audio path touches atomics and its own instance's fields only; registration, close and
 * the block-size change take the lock. */
#define _GNU_SOURCE
#include "live.h"
#include "vst.h"
#include <dlfcn.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG(...) fprintf(stderr, "mpc-sample-rack: " __VA_ARGS__)
#define MAX_INST 64
#define RUNNING_MS 1000   /* processed audio this recently: running */

struct linst {
    AEffect *_Atomic fx;              /* NULL: free */
    atomic_int want_off;              /* set by the page; the audio thread follows */
    atomic_llong last_ms;             /* the last processReplacing */
    int off;                          /* the audio thread's */
    float *scratch;                   /* the dry signal during a switch: ch x cap */
    int ch, cap;
    char file[PATH_MAX];
    vst_dispatcher dispatcher;
    void (*replacing)(AEffect *, float **, float **, int32_t);
    void (*replacing_d)(AEffect *, double **, double **, int32_t);
};

static struct linst L[MAX_INST];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int enabled;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct linst *of(AEffect *fx)
{
    for (int k = 0; k < MAX_INST; k++)
        if (atomic_load_explicit(&L[k].fx, memory_order_acquire) == fx) return &L[k];
    return NULL;
}

/* ---- audio ---- */
/* out[c] = the input that goes there when off: the same channel, or the last input (mono into stereo), or silence */
static const float *dry_src(AEffect *fx, float **in, int c)
{
    return fx->numInputs > 0 ? in[c < fx->numInputs ? c : fx->numInputs - 1] : NULL;
}

static void dry(AEffect *fx, float **in, float **out, int32_t n)
{
    for (int c = 0; c < fx->numOutputs; c++) {
        const float *s = dry_src(fx, in, c);
        if (!s) memset(out[c], 0, (size_t)n * sizeof(float));
        else if (s != out[c]) memmove(out[c], s, (size_t)n * sizeof(float));
    }
}

static void live_replacing(AEffect *fx, float **in, float **out, int32_t n)
{
    struct linst *x = of(fx);
    if (!x) return;
    atomic_store_explicit(&x->last_ms, now_ms(), memory_order_relaxed);
    int want = atomic_load_explicit(&x->want_off, memory_order_relaxed);
    if (want == x->off) {
        if (want) dry(fx, in, out, n);
        else x->replacing(fx, in, out, n);
        return;
    }
    x->off = want;
    int ch = fx->numOutputs;
    if (!x->scratch || n > x->cap || ch > x->ch || n <= 0) {   /* no room for the dry signal: a plain switch */
        if (want) dry(fx, in, out, n);
        else x->replacing(fx, in, out, n);
        return;
    }
    for (int c = 0; c < ch; c++) {   /* keep the dry signal: the plugin may write over its input */
        const float *s = dry_src(fx, in, c);
        float *d = x->scratch + (size_t)c * (size_t)x->cap;
        if (s) memcpy(d, s, (size_t)n * sizeof(float));
        else memset(d, 0, (size_t)n * sizeof(float));
    }
    x->replacing(fx, in, out, n);
    for (int c = 0; c < ch; c++) {
        const float *d = x->scratch + (size_t)c * (size_t)x->cap;
        for (int i = 0; i < n; i++) {
            float g = (float)(i + 1) / (float)n, w = out[c][i];
            out[c][i] = want ? w + (d[i] - w) * g : d[i] + (w - d[i]) * g;
        }
    }
}

static void live_replacing_d(AEffect *fx, double **in, double **out, int32_t n)
{
    struct linst *x = of(fx);
    if (!x) return;
    atomic_store_explicit(&x->last_ms, now_ms(), memory_order_relaxed);
    if (!atomic_load_explicit(&x->want_off, memory_order_relaxed)) { x->replacing_d(fx, in, out, n); return; }
    for (int c = 0; c < fx->numOutputs; c++) {
        const double *s = fx->numInputs > 0 ? in[c < fx->numInputs ? c : fx->numInputs - 1] : NULL;
        if (!s) memset(out[c], 0, (size_t)n * sizeof(double));
        else if (s != out[c]) memmove(out[c], s, (size_t)n * sizeof(double));
    }
}

/* ---- the patched dispatcher ---- */
static intptr_t live_dispatcher(AEffect *fx, int32_t op, int32_t index, intptr_t value, void *ptr, float opt)
{
    struct linst *x = of(fx);
    if (!x) return 0;
    vst_dispatcher d = x->dispatcher;
    if (op == effClose) {
        pthread_mutex_lock(&lock);
        atomic_store_explicit(&x->fx, NULL, memory_order_release);
        free(x->scratch);
        x->scratch = NULL;
        pthread_mutex_unlock(&lock);
        return d(fx, op, index, value, ptr, opt);   /* the plugin frees itself here */
    }
    if (op == effSetBlockSize && value > 0) {   /* the host doesn't process meanwhile */
        int ch = fx->numOutputs > 0 ? fx->numOutputs : 0;
        float *s = ch ? malloc((size_t)ch * (size_t)value * sizeof(float)) : NULL;
        pthread_mutex_lock(&lock);
        free(x->scratch);
        x->scratch = s;
        x->ch = s ? ch : 0;
        x->cap = s ? (int)value : 0;
        pthread_mutex_unlock(&lock);
    }
    return d(fx, op, index, value, ptr, opt);
}

static void register_instance(AEffect *fx, const char *file)
{
    if (!fx || fx->magic != VST_MAGIC || (fx->flags & effFlagsIsSynth) || !fx->processReplacing) return;
    pthread_mutex_lock(&lock);
    struct linst *x = NULL;
    for (int k = 0; k < MAX_INST && !x; k++)
        if (!atomic_load(&L[k].fx)) x = &L[k];
    if (!x) { pthread_mutex_unlock(&lock); LOG("%d plugins already: %s has no live on/off\n", MAX_INST, file); return; }
    if (!realpath(file, x->file)) snprintf(x->file, sizeof x->file, "%s", file);
    x->dispatcher = fx->dispatcher;
    x->replacing = fx->processReplacing;
    x->replacing_d = fx->processDoubleReplacing;
    x->off = 0;
    x->scratch = NULL;
    x->ch = x->cap = 0;
    atomic_store(&x->want_off, 0);
    atomic_store(&x->last_ms, 0);
    fx->dispatcher = live_dispatcher;
    fx->processReplacing = live_replacing;
    if (x->replacing_d) fx->processDoubleReplacing = live_replacing_d;
    atomic_store_explicit(&x->fx, fx, memory_order_release);
    pthread_mutex_unlock(&lock);
}

/* ---- the entry trampolines: one per plugin library ---- */
#define MAX_MODULES 32
static struct { char path[PATH_MAX]; vst_entry _Atomic entry; } mods[MAX_MODULES];

static AEffect *main_common(int m, audioMasterCallback master)
{
    vst_entry e = atomic_load(&mods[m].entry);
    AEffect *fx = e ? e(master) : NULL;
    if (fx) register_instance(fx, mods[m].path);
    return fx;
}
#define MODULE_FN(N) static AEffect *main_##N(audioMasterCallback m) { return main_common(N, m); }
MODULE_FN(0) MODULE_FN(1) MODULE_FN(2) MODULE_FN(3) MODULE_FN(4) MODULE_FN(5) MODULE_FN(6) MODULE_FN(7)
MODULE_FN(8) MODULE_FN(9) MODULE_FN(10) MODULE_FN(11) MODULE_FN(12) MODULE_FN(13) MODULE_FN(14) MODULE_FN(15)
MODULE_FN(16) MODULE_FN(17) MODULE_FN(18) MODULE_FN(19) MODULE_FN(20) MODULE_FN(21) MODULE_FN(22) MODULE_FN(23)
MODULE_FN(24) MODULE_FN(25) MODULE_FN(26) MODULE_FN(27) MODULE_FN(28) MODULE_FN(29) MODULE_FN(30) MODULE_FN(31)
static const vst_entry module_fns[MAX_MODULES] = {
    main_0, main_1, main_2, main_3, main_4, main_5, main_6, main_7, main_8, main_9, main_10, main_11, main_12, main_13,
    main_14, main_15, main_16, main_17, main_18, main_19, main_20, main_21, main_22, main_23, main_24, main_25, main_26,
    main_27, main_28, main_29, main_30, main_31,
};

static int module_for(const char *path, vst_entry entry)
{
    pthread_mutex_lock(&lock);
    int m = -1, free_slot = -1;
    for (int k = 0; k < MAX_MODULES && m < 0; k++) {
        if (!mods[k].path[0]) { if (free_slot < 0) free_slot = k; }
        else if (!strcmp(mods[k].path, path)) m = k;
    }
    if (m < 0 && free_slot >= 0) snprintf(mods[m = free_slot].path, sizeof mods[m].path, "%s", path);
    if (m >= 0) atomic_store(&mods[m].entry, entry);
    pthread_mutex_unlock(&lock);
    return m;
}

/* ---- dlsym ----
 * The same interposition as mpc-addin-commander's (src/hook.c there): libc's dlsym found with dlvsym, RTLD_NEXT
 * answered relative to the caller. A plugin's entry is first asked of the next library that also exports dlsym (another
 * addin), so both wrap it, in either load order. */
typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn real_dlsym, next_dlsym;
static atomic_int next_known;
#define NO_SANITIZE __attribute__((no_sanitize("address", "thread", "undefined")))

static NO_SANITIZE void resolve(void)
{
    if (real_dlsym) return;
    static const char *const vers[] = { "2.34", "2.17", "2.4", "2.2.5", "2.0" };   /* assembled: no "GLIBC_2.34" text */
    for (size_t i = 0; i < sizeof vers / sizeof *vers && !real_dlsym; i++) {
        char v[16] = "GLIBC_";
        strcat(v, vers[i]);
        real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", v);
    }
    if (!real_dlsym) LOG("can't find libc's dlsym: no live on/off\n");
}

static NO_SANITIZE void *lookup_in(struct link_map *map, const char *name)
{
    if (!map->l_name || !map->l_name[0] || strstr(map->l_name, "vdso")) return NULL;
    void *h = dlopen(map->l_name, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) return NULL;
    void *sym = real_dlsym(h, name);
    dlclose(h);
    return sym;
}

static NO_SANITIZE struct link_map *map_of(const void *addr)
{
    Dl_info info;
    struct link_map *map = NULL;
    return addr && dladdr1(addr, &info, (void **)&map, RTLD_DL_LINKMAP) ? map : NULL;
}

static NO_SANITIZE void *dlsym_next(const char *name, void *caller)
{
    struct link_map *cm = map_of(caller);
    for (struct link_map *m = cm ? cm->l_next : NULL; m; m = m->l_next) {
        void *sym = lookup_in(m, name);
        if (sym && map_of(sym) == m) return sym;
    }
    void *sym = real_dlsym(RTLD_NEXT, name);
    if (!sym || !cm || map_of(sym) != cm) return sym;
    struct link_map *me = map_of((const void *)dlsym_next);
    for (struct link_map *m = me ? me->l_next : NULL; m; m = m->l_next)
        if (m != cm && (sym = lookup_in(m, name))) return sym;
    return NULL;
}

/* The next addin after this one that exports dlsym (not libc's or libdl's), found once, at the first plugin lookup. */
static NO_SANITIZE dlsym_fn find_next_dlsym(void)
{
    if (atomic_load(&next_known)) return next_dlsym;
    dlsym_fn found = NULL;
    struct link_map *me = map_of((const void *)find_next_dlsym);
    for (struct link_map *m = me ? me->l_next : NULL; m && !found; m = m->l_next) {
        if (!m->l_name || strstr(m->l_name, "/libc.so") || strstr(m->l_name, "/libdl.so") || strstr(m->l_name, "/ld-linux")) continue;
        void *sym = lookup_in(m, "dlsym");
        if (sym && sym != (void *)real_dlsym && map_of(sym) == m) found = (dlsym_fn)sym;
    }
    next_dlsym = found;
    atomic_store(&next_known, 1);
    if (found) LOG("plugin lookups go on to %s too\n", map_of((void *)found) ? map_of((void *)found)->l_name : "?");
    return found;
}

#ifndef HOOK_DLSYM
#define HOOK_DLSYM dlsym
#endif
__attribute__((visibility("default"))) NO_SANITIZE void *HOOK_DLSYM(void *handle, const char *name)
{
    resolve();
    if (!real_dlsym) return NULL;
    if (handle == RTLD_NEXT) return dlsym_next(name, __builtin_return_address(0));
    if (strcmp(name, "VSTPluginMain") && strcmp(name, "main")) return real_dlsym(handle, name);
    dlsym_fn next = find_next_dlsym();
    void *sym = next ? next(handle, name) : real_dlsym(handle, name);
    if (!sym || !atomic_load(&enabled)) return sym;
    Dl_info info;
    /* the library the entry came from: the handle's, as another addin's trampoline lives in that addin */
    void *own = real_dlsym(handle, name);
    if (!own || !dladdr(own, &info) || !info.dli_fname || !strstr(info.dli_fname, ".so")) return sym;
    int m = module_for(info.dli_fname, (vst_entry)sym);
    if (m < 0) { LOG("%d plugin files already: %s has no live on/off\n", MAX_MODULES, info.dli_fname); return sym; }
    return (void *)module_fns[m];
}

void live_enable(void)
{
    resolve();
    atomic_store(&enabled, 1);
}

/* ---- the page's side ---- */
static struct linst *running(const char *file)
{
    char want[PATH_MAX];
    if (!file || !file[0]) return NULL;
    if (!realpath(file, want)) snprintf(want, sizeof want, "%s", file);
    long long t = now_ms();
    struct linst *hit = NULL;
    for (int k = 0; k < MAX_INST; k++) {
        struct linst *x = &L[k];
        if (!atomic_load(&x->fx) || strcmp(x->file, want)) continue;
        if (t - atomic_load(&x->last_ms) > RUNNING_MS) continue;
        if (hit) return NULL;   /* two of them: can't tell which insert is which */
        hit = x;
    }
    return hit;
}

int live_state(const char *file)
{
    pthread_mutex_lock(&lock);
    struct linst *x = running(file);
    int r = x ? (atomic_load(&x->want_off) ? LIVE_OFF : LIVE_ON) : LIVE_NONE;
    pthread_mutex_unlock(&lock);
    return r;
}

int live_set(const char *file, int off)
{
    pthread_mutex_lock(&lock);
    struct linst *x = running(file);
    if (x) atomic_store(&x->want_off, off != 0);
    pthread_mutex_unlock(&lock);
    return x ? 0 : -1;
}
