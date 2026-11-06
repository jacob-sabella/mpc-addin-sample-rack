/* The live on/off test: a stand-in "MPC Sample" with the real addin preloaded loads tests/fake_fx.so the way MPC's
 * host does, processes blocks, and turns it off and on through the bridge (mpc_sample_rack_live), as Commander would.
 * Usage: "MPC Sample" <fake_fx.so> [expect-other-hook] */
#define _GNU_SOURCE
#include "../src/vst.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (c) printf("ok   %s\n", m); else { printf("FAIL %s\n", m); fails++; } } while (0)
#define N 64

static intptr_t master(AEffect *fx, int32_t op, int32_t i, intptr_t v, void *p, float o)
{
    (void)fx; (void)i; (void)v; (void)p; (void)o;
    return op == audioMasterVersion ? 2400 : 0;
}

static float a[2][N], b[2][N];
static float *in[2] = { a[0], a[1] }, *out[2] = { b[0], b[1] };

static void block(AEffect *fx, float **i, float **o)
{
    for (int c = 0; c < 2; c++) for (int k = 0; k < N; k++) i[c][k] = 1.0f;
    fx->processReplacing(fx, i, o, N);
}
static int all(float **o, float v) { for (int c = 0; c < 2; c++) for (int k = 0; k < N; k++) if (fabsf(o[c][k] - v) > 1e-6f) return 0; return 1; }
static int ramp(float **o, float from, float to)
{
    for (int c = 0; c < 2; c++) for (int k = 0; k < N; k++) {
        float g = (float)(k + 1) / N;
        if (fabsf(o[c][k] - (from + (to - from) * g)) > 1e-5f) return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    vst_entry entry = h ? (vst_entry)dlsym(h, "VSTPluginMain") : NULL;
    CHECK(entry != NULL, "the plugin's entry");
    if (!entry) return 1;
    Dl_info di;
    CHECK(dladdr((void *)entry, &di) && strstr(di.dli_fname, "mpc_sample_rack"), "is the rack's trampoline");
    AEffect *fx = entry(master);
    CHECK(fx && fx->magic == VST_MAGIC, "an instance");
    if (argc > 2) {
        int *n = (int *)dlsym(RTLD_DEFAULT, "other_hook_instances");
        CHECK(n && *n == 1, "the other addin that hooks dlsym wrapped it too");
    }
    fx->dispatcher(fx, effOpen, 0, 0, NULL, 0);
    fx->dispatcher(fx, effSetSampleRate, 0, 0, NULL, 44100);
    fx->dispatcher(fx, effSetBlockSize, 0, N, NULL, 0);
    fx->dispatcher(fx, effMainsChanged, 0, 1, NULL, 0);

    int (*live)(const char *, int, const char *, int, char *, size_t) =
        (int (*)(const char *, int, const char *, int, char *, size_t))dlsym(RTLD_DEFAULT, "mpc_sample_rack_live");
    char *(*state)(void) = (char *(*)(void))dlsym(RTLD_DEFAULT, "mpc_sample_rack_state");
    int (*api)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "mpc_sample_rack_api");
    CHECK(live && state && api && api() == 2, "the bridge, interface 2");
    if (!live || !state) return 1;
    char why[200] = "";
    CHECK(live("input", 1, "NAM", 0, why, sizeof why) == -1 && strstr(why, "isn't running"), "not processing yet: refused");

    block(fx, in, out);
    CHECK(all(out, 0.5f), "on: the plugin's output");
    char *st = state();
    CHECK(st && strstr(st, "\"name\":\"NAM\",\"live\":\"on\""), "state: NAM live on");
    CHECK(st && strstr(st, "\"name\":\"LP Filter\",\"live\":null"), "state: MPC's own effect has no live state");
    free(st);
    CHECK(live("input", 1, "NAM", 0, why, sizeof why) == 0, "turned off");
    block(fx, in, out);
    CHECK(ramp(out, 0.5f, 1.0f), "the switch: a crossfade over one block");
    block(fx, in, out);
    CHECK(all(out, 1.0f), "off: the input as it is");
    block(fx, in, in);
    CHECK(all(in, 1.0f), "off, in place: the input as it is");
    st = state();
    CHECK(st && strstr(st, "\"name\":\"NAM\",\"live\":\"off\""), "state: NAM live off");
    free(st);
    CHECK(live("input", 1, "Not NAM", 1, why, sizeof why) == -1 && strstr(why, "isn't at insert"), "a stale name: refused");
    CHECK(live("output:0", 0, "Pad-Controlled FXs", 1, why, sizeof why) == -1 && strstr(why, "MPC's own"), "MPC's own effect: refused");
    CHECK(live("input", 1, "NAM", 1, why, sizeof why) == 0, "turned on");
    block(fx, in, out);
    CHECK(ramp(out, 1.0f, 0.5f), "back: a crossfade over one block");
    block(fx, in, out);
    CHECK(all(out, 0.5f), "on again: the plugin's output");

    fx->dispatcher(fx, effMainsChanged, 0, 0, NULL, 0);
    fx->dispatcher(fx, effClose, 0, 0, NULL, 0);
    CHECK(live("input", 1, "NAM", 0, why, sizeof why) == -1, "closed: no live on/off");
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails != 0;
}
