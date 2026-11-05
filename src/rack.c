/* mpc-addin-sample-rack, the core: the project file's rack, and edits to it (see rack.h). */
#define _GNU_SOURCE
#include "rack.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- the device's zlib, loaded at run time (never linked: one less thing to match on the device) ---- */
static struct {
    void *(*open)(const char *, const char *);
    int (*read)(void *, void *, unsigned);
    int (*write)(void *, const void *, unsigned);
    int (*close)(void *);
} Z;
static pthread_mutex_t z_mu = PTHREAD_MUTEX_INITIALIZER;

static int zlib(void)
{
    pthread_mutex_lock(&z_mu);
    if (!Z.close) {
        void *lib = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            *(void **)&Z.open = dlsym(lib, "gzopen");
            *(void **)&Z.read = dlsym(lib, "gzread");
            *(void **)&Z.write = dlsym(lib, "gzwrite");
            *(void **)&Z.close = dlsym(lib, "gzclose");
            if (!Z.open || !Z.read || !Z.write) Z.close = NULL;
        }
    }
    int ok = Z.close != NULL;
    pthread_mutex_unlock(&z_mu);
    return ok ? 0 : -1;
}

char *rack_read(const char *path, size_t *len, size_t *json, char *err, size_t en)
{
    if (zlib()) { snprintf(err, en, "zlib isn't available"); return NULL; }
    void *g = Z.open(path, "rb");
    if (!g) { snprintf(err, en, "can't open %s", path); return NULL; }
    size_t cap = 1 << 20, n = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        if (!b) { Z.close(g); snprintf(err, en, "out of memory"); return NULL; }
        int r = Z.read(g, b + n, (unsigned)(cap - n));
        if (r < 0) { Z.close(g); free(b); snprintf(err, en, "can't read %s", path); return NULL; }
        n += (size_t)r;
        if (r == 0) break;
        if (n == cap) { cap *= 2; char *nb = realloc(b, cap + 1); if (!nb) free(b); b = nb; }
    }
    Z.close(g);
    b[n] = 0;
    size_t i = 0;
    for (int line = 0; line < 5; line++) {   /* product, version, class, "json", platform */
        char *nl = memchr(b + i, '\n', n - i);
        if (!nl) { free(b); snprintf(err, en, "%s: not a project file", path); return NULL; }
        i = (size_t)(nl - b) + 1;
    }
    if (b[i] != '{') { free(b); snprintf(err, en, "%s: no JSON after the header", path); return NULL; }
    *len = n; *json = i;
    return b;
}

int rack_write(const char *path, const char *text, size_t len, char *err, size_t en)
{
    if (zlib()) { snprintf(err, en, "zlib isn't available"); return -1; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    void *g = Z.open(tmp, "wb");
    if (!g) { snprintf(err, en, "can't write %s", tmp); return -1; }
    size_t off = 0;
    while (off < len) {
        unsigned k = len - off > (1u << 30) ? (1u << 30) : (unsigned)(len - off);
        if (Z.write(g, text + off, k) != (int)k) { Z.close(g); unlink(tmp); snprintf(err, en, "can't write %s", tmp); return -1; }
        off += k;
    }
    if (Z.close(g) != 0) { unlink(tmp); snprintf(err, en, "can't finish %s", tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); snprintf(err, en, "can't replace %s", path); return -1; }
    return 0;
}

int rack_tokens(const char *s, size_t n, struct jtok **t)
{
    int max = (int)(n / 2) + 64;
    *t = malloc((size_t)max * sizeof **t);
    if (!*t) return -1;
    int r = json_parse(s, n, *t, max);
    if (r < 0) { free(*t); *t = NULL; }
    return r;
}

/* item i of array token a, or -1 */
static int item(const struct jtok *t, int a, int i)
{
    if (a < 0 || t[a].type != J_ARR || i < 0 || i >= t[a].size) return -1;
    int j = a + 1;
    while (i--) j = t[j].skip;
    return j;
}

/* a path of object keys from token o: "a.b.c" */
static int path(const char *s, const struct jtok *t, int o, const char *p)
{
    char k[64];
    while (o >= 0 && *p) {
        const char *d = strchr(p, '.');
        size_t l = d ? (size_t)(d - p) : strlen(p);
        if (l >= sizeof k) return -1;
        memcpy(k, p, l); k[l] = 0;
        o = t[o].type == J_OBJ ? json_get(s, t, o, k) : -1;
        p += l + (d ? 1 : 0);
    }
    return o;
}

/* the mixable object of a slot id */
static int mixable(const char *s, const struct jtok *t, const char *slot)
{
    int data = path(s, t, 0, "data");
    int n;
    if (!strcmp(slot, "input")) return path(s, t, data, "mixer.input.mixable");
    if (sscanf(slot, "output:%d", &n) == 1) return path(s, t, item(t, path(s, t, data, "mixer.outputs"), n), "mixable");
    if (sscanf(slot, "submix:%d", &n) == 1) return path(s, t, item(t, path(s, t, data, "mixer.submixes"), n), "mixable");
    if (sscanf(slot, "track:%d", &n) == 1) return path(s, t, item(t, path(s, t, data, "tracks"), n), "program.mixable");
    return -1;
}

int rack_effects(const char *s, const struct jtok *t, const char *slot)
{
    int e = path(s, t, mixable(s, t, slot), "inserts.effects");
    return e >= 0 && t[e].type == J_ARR ? e : -1;
}

static int num(const char *s, const struct jtok *t, int i, int def)
{
    double d;
    return i >= 0 && !json_num(s, t, i, &d) ? (int)d : def;
}

static void fx_of(const char *s, const struct jtok *t, int o, rack_fx *f)
{
    memset(f, 0, sizeof *f);
    f->index = num(s, t, json_get(s, t, o, "index"), -1);
    int en = 1, e = json_get(s, t, o, "enable");
    if (e >= 0) json_bool(s, t, e, &en);
    f->enabled = en;
    int d = path(s, t, o, "plugin.description");
    char fmt[16] = "";
    if (json_str(s, t, json_get(s, t, d, "name"), f->name, sizeof f->name)) snprintf(f->name, sizeof f->name, "?");
    json_str(s, t, json_get(s, t, d, "pluginFormatName"), fmt, sizeof fmt);
    f->vst = !strcmp(fmt, "VST");
}

static void add_slot(const char *s, const struct jtok *t, rack_model *m, const char *id, const char *label)
{
    int e = rack_effects(s, t, id);
    if (e < 0 || m->nslots >= RACK_MAX_SLOTS) return;
    rack_slot *sl = &m->slot[m->nslots++];
    memset(sl, 0, sizeof *sl);
    snprintf(sl->id, sizeof sl->id, "%s", id);
    snprintf(sl->label, sizeof sl->label, "%s", label);
    for (int i = 0; i < t[e].size && sl->nfx < RACK_MAX_FX; i++) fx_of(s, t, item(t, e, i), &sl->fx[sl->nfx++]);
}

int rack_model_of(const char *s, const struct jtok *t, rack_model *m)
{
    m->nslots = 0;
    int data = path(s, t, 0, "data");
    if (data < 0) return -1;
    char id[24], label[48];
    add_slot(s, t, m, "input", "INPUT");
    int outs = path(s, t, data, "mixer.outputs");
    for (int i = 0; outs >= 0 && i < t[outs].size; i++) {
        snprintf(id, sizeof id, "output:%d", i);
        if (i == 0) snprintf(label, sizeof label, "MAIN OUT");
        else snprintf(label, sizeof label, "OUT %d/%d", 2 * i + 1, 2 * i + 2);
        add_slot(s, t, m, id, label);
    }
    int subs = path(s, t, data, "mixer.submixes");
    for (int i = 0; subs >= 0 && i < t[subs].size; i++) {
        snprintf(id, sizeof id, "submix:%d", i); snprintf(label, sizeof label, "SUBMIX %d", i + 1);
        add_slot(s, t, m, id, label);
    }
    /* tracks: their program's channel. Submix (8) and output (9) programs mirror the mixer's channels above, and MPC
     * loads those from the mixer (2026-11-05, device), so they're left out. */
    int tracks = path(s, t, data, "tracks");
    for (int i = 0; tracks >= 0 && i < t[tracks].size; i++) {
        int tr = item(t, tracks, i), type = num(s, t, path(s, t, tr, "program.type"), -1);
        if (type == 8 || type == 9) continue;
        char name[40] = "";
        if (json_str(s, t, json_get(s, t, tr, "name"), name, sizeof name)) snprintf(name, sizeof name, "TRACK %d", i + 1);
        snprintf(id, sizeof id, "track:%d", i); snprintf(label, sizeof label, "%s", name);
        add_slot(s, t, m, id, label);
    }
    return 0;
}

int rack_load(const char *file, rack_model *m, char *err, size_t en)
{
    size_t len, js;
    char *b = rack_read(file, &len, &js, err, en);
    if (!b) return -1;
    struct jtok *t;
    int r = rack_tokens(b + js, len - js, &t);
    if (r < 0) { free(b); snprintf(err, en, "%s: can't read its JSON", file); return -1; }
    r = rack_model_of(b + js, t, m);
    free(t); free(b);
    if (r) { snprintf(err, en, "%s: no project data", file); return -1; }
    /* the project's name: the first line of original_project_data.txt beside the file */
    m->project[0] = 0;
    char p[1024];
    snprintf(p, sizeof p, "%s", file);
    char *sl = strrchr(p, '/');
    if (sl) {
        snprintf(sl + 1, sizeof p - (size_t)(sl + 1 - p), "original_project_data.txt");
        FILE *f = fopen(p, "re");
        if (f) {
            if (fgets(m->project, sizeof m->project, f)) m->project[strcspn(m->project, "\r\n")] = 0;
            fclose(f);
        }
    }
    return 0;
}

/* ---- MPC's settings: <PLUGIN .../> lines under pluginList-arm-64bit ---- */
static int attr(const char *el, const char *end, const char *name, char *out, size_t n)
{
    char k[40];
    snprintf(k, sizeof k, " %s=\"", name);
    const char *p = strstr(el, k);
    if (!p || p > end) return -1;
    p += strlen(k);
    size_t o = 0;
    while (p < end && *p != '"' && o + 1 < n) {
        static const struct { const char *e; char c; } ent[] = { { "&amp;", '&' }, { "&quot;", '"' }, { "&apos;", '\'' },
                                                                 { "&lt;", '<' }, { "&gt;", '>' } };
        int hit = 0;
        if (*p == '&')
            for (size_t i = 0; i < sizeof ent / sizeof *ent; i++)
                if (!strncmp(p, ent[i].e, strlen(ent[i].e))) { out[o++] = ent[i].c; p += strlen(ent[i].e); hit = 1; break; }
        if (!hit) out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

int rack_plugins(const char *settings, rack_plugin *out, int max)
{
    FILE *f = fopen(settings, "re");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = sz > 0 ? malloc((size_t)sz + 1) : NULL;
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return 0; }
    fclose(f);
    b[sz] = 0;
    int n = 0;
    char *list = strstr(b, "name=\"pluginList-arm-64bit\"");
    char *stop = list ? strstr(list, "</KNOWNPLUGINS>") : NULL;
    for (char *p = list; p && stop && n < max && (p = strstr(p, "<PLUGIN ")) && p < stop; p++) {
        char *end = strstr(p, "/>");
        if (!end || end > stop) break;
        char fmt[16], inst[8], uid[16];
        rack_plugin *pl = &out[n];
        if (attr(p, end, "format", fmt, sizeof fmt) || strcmp(fmt, "VST")) continue;
        if (!attr(p, end, "isInstrument", inst, sizeof inst) && strcmp(inst, "0")) continue;
        if (attr(p, end, "name", pl->name, sizeof pl->name) || attr(p, end, "file", pl->file, sizeof pl->file) ||
            attr(p, end, "uid", uid, sizeof uid)) continue;
        if (attr(p, end, "manufacturer", pl->manufacturer, sizeof pl->manufacturer)) pl->manufacturer[0] = 0;
        pl->uid = (unsigned)strtoul(uid, NULL, 16);
        n++;
    }
    free(b);
    return n;
}

/* ---- edits ---- */
/* replace [a, b) of the text with r */
static int splice(char **text, size_t *len, size_t a, size_t b, const char *r, size_t rn)
{
    size_t nl = *len - (b - a) + rn;
    if (nl > *len) { char *nt = realloc(*text, nl + 1); if (!nt) return -1; *text = nt; }
    memmove(*text + a + rn, *text + b, *len - b + 1);   /* with the NUL */
    memcpy(*text + a, r, rn);
    *len = nl;
    return 0;
}

int rack_apply(char **text, size_t *len, size_t js, const rack_edit *e, char *err, size_t en)
{
    struct jtok *t;
    const char *s = *text + js;
    if (rack_tokens(s, *len - js, &t) < 0) { snprintf(err, en, "can't read the project's JSON"); return -1; }
    int r = 1, arr = rack_effects(s, t, e->slot);
    if (arr < 0) { snprintf(err, en, "%s: no such channel", e->slot); goto out; }
    if (e->op == RACK_ADD) {
        int used[RACK_FX_SLOTS] = { 0 }, idx = -1;
        for (int i = 0; i < t[arr].size; i++) {
            int x = num(s, t, json_get(s, t, item(t, arr, i), "index"), -1);
            if (x >= 0 && x < RACK_FX_SLOTS) used[x] = 1;
        }
        for (int i = 0; i < RACK_FX_SLOTS && idx < 0; i++) if (!used[i]) idx = i;
        if (idx < 0) { snprintf(err, en, "%s: no free insert slot for %s", e->slot, e->plugin.name); goto out; }
        struct sb b = { 0 };
        if (t[arr].size) sb_puts(&b, ",");
        sb_printf(&b, "{\"index\":%d,\"plugin\":{\"version\":2,\"is64Bits\":true,\"description\":{\"version\":1,\"name\":", idx);
        sb_jstr(&b, e->plugin.name);
        sb_puts(&b, ",\"descriptiveName\":"); sb_jstr(&b, e->plugin.name);
        sb_puts(&b, ",\"pluginFormatName\":\"VST\",\"category\":\"Effect\",\"manufacturerName\":"); sb_jstr(&b, e->plugin.manufacturer);
        sb_puts(&b, ",\"fileOrIdentifier\":"); sb_jstr(&b, e->plugin.file);
        sb_printf(&b, ",\"uid\":%d,\"isInstrument\":false,\"numInputChannels\":2,\"numOutputChannels\":2},\"state\":\"\","
                      "\"programNumber\":0,\"presetName\":\"\",\"presetEdited\":false},\"enable\":true}", (int)e->plugin.uid);
        if (b.oom) { sb_free(&b); snprintf(err, en, "out of memory"); r = -1; goto out; }
        size_t at = js + (size_t)t[arr].end - 1;   /* before the closing bracket */
        r = splice(text, len, at, at, b.p, b.n) ? -1 : 0;
        sb_free(&b);
        if (r) snprintf(err, en, "out of memory");
        goto out;
    }
    int k = -1;   /* the item with that index and name */
    for (int i = 0; i < t[arr].size && k < 0; i++) {
        int o = item(t, arr, i);
        rack_fx f;
        fx_of(s, t, o, &f);
        if (f.index == e->index && !strcmp(f.name, e->name)) k = i;
    }
    if (k < 0) { snprintf(err, en, "%s: %s isn't at insert %d any more", e->slot, e->name, e->index + 1); goto out; }
    int o = item(t, arr, k);
    if (e->op == RACK_ENABLE) {
        int v = json_get(s, t, o, "enable");
        if (v < 0 || (t[v].type != J_TRUE && t[v].type != J_FALSE)) { snprintf(err, en, "%s: %s has no on/off", e->slot, e->name); goto out; }
        const char *w = e->value ? "true" : "false";
        r = splice(text, len, js + (size_t)t[v].start, js + (size_t)t[v].end, w, strlen(w)) ? -1 : 0;
    } else if (e->op == RACK_REMOVE) {
        size_t a, b;
        if (t[arr].size == 1) { a = (size_t)t[o].start; b = (size_t)t[o].end; }
        else if (k + 1 < t[arr].size) { a = (size_t)t[o].start; b = (size_t)t[item(t, arr, k + 1)].start; }   /* with the comma after */
        else { a = (size_t)t[item(t, arr, k - 1)].end; b = (size_t)t[o].end; }                                 /* with the comma before */
        r = splice(text, len, js + a, js + b, "", 0) ? -1 : 0;
    } else { snprintf(err, en, "unknown edit"); r = -1; }
    if (r < 0) snprintf(err, en, "out of memory");
out:
    free(t);
    return r;
}

int rack_edits_save(const char *file, const rack_edit *e, int n)
{
    FILE *f = fopen(file, "we");
    if (!f) return -1;
    for (int i = 0; i < n; i++)
        fprintf(f, "%d\t%s\t%d\t%d\t%s\t%s\t%s\t%x\t%s\n", e[i].op, e[i].slot, e[i].index, e[i].value, e[i].name,
                e[i].plugin.name, e[i].plugin.manufacturer, e[i].plugin.uid, e[i].plugin.file);
    return fclose(f) ? -1 : 0;
}

/* the next tab-separated field into out */
static char *field(char *p, char *out, size_t n)
{
    if (!p) { out[0] = 0; return NULL; }
    char *tab = strchr(p, '\t');
    size_t l = tab ? (size_t)(tab - p) : strcspn(p, "\r\n");
    snprintf(out, n, "%.*s", (int)l, p);
    return tab ? tab + 1 : NULL;
}

int rack_edits_load(const char *file, rack_edit *e, int max)
{
    FILE *f = fopen(file, "re");
    if (!f) return -1;
    char line[1024], v[600];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        rack_edit *x = &e[n];
        memset(x, 0, sizeof *x);
        char *p = line;
        p = field(p, v, sizeof v); x->op = atoi(v);
        p = field(p, x->slot, sizeof x->slot);
        p = field(p, v, sizeof v); x->index = atoi(v);
        p = field(p, v, sizeof v); x->value = atoi(v);
        p = field(p, x->name, sizeof x->name);
        p = field(p, x->plugin.name, sizeof x->plugin.name);
        p = field(p, x->plugin.manufacturer, sizeof x->plugin.manufacturer);
        p = field(p, v, sizeof v); x->plugin.uid = (unsigned)strtoul(v, NULL, 16);
        field(p, x->plugin.file, sizeof x->plugin.file);
        if (x->op >= RACK_ADD && x->op <= RACK_ENABLE && x->slot[0]) n++;
    }
    fclose(f);
    return n;
}
