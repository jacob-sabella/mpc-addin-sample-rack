/* A stand-in effect plugin for the live test: two in, two out, output = half the input. */
#include "../src/vst.h"
#include <stdlib.h>

static intptr_t dispatch(AEffect *fx, int32_t op, int32_t i, intptr_t v, void *p, float o)
{
    (void)i; (void)v; (void)p; (void)o;
    if (op == effClose) free(fx);
    return 0;
}

static void replacing(AEffect *fx, float **in, float **out, int32_t n)
{
    (void)fx;
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < n; k++) out[c][k] = in[c][k] * 0.5f;
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master)
{
    (void)master;
    AEffect *fx = calloc(1, sizeof *fx);
    fx->magic = VST_MAGIC;
    fx->dispatcher = dispatch;
    fx->processReplacing = replacing;
    fx->numInputs = fx->numOutputs = 2;
    fx->flags = effFlagsCanReplacing;
    fx->uniqueID = 0x46616b65;
    return fx;
}
