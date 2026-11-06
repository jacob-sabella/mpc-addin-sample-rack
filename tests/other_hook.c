/* Another addin that interposes dlsym for plugin entries, as mpc-addin-commander does: preloaded after the rack, it must
 * still see the plugin's entry. Counts the instances it wrapped (other_hook_instances). */
#define _GNU_SOURCE
#include "../src/vst.h"
#include <dlfcn.h>
#include <string.h>

__attribute__((visibility("default"))) int other_hook_instances;
static vst_entry real_entry;

static AEffect *wrapped(audioMasterCallback m)
{
    AEffect *fx = real_entry(m);
    if (fx) other_hook_instances++;
    return fx;
}

__attribute__((visibility("default"))) void *dlsym(void *h, const char *name)
{
    static void *(*real)(void *, const char *);
    if (!real) real = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real) real = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    void *s = real(h, name);
    if (s && h != RTLD_NEXT && !strcmp(name, "VSTPluginMain")) { real_entry = (vst_entry)s; return (void *)wrapped; }
    return s;
}
