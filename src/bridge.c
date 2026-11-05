/* The rack for other addins in the MPC Sample's process: Commander finds these by dlsym (RTLD_DEFAULT) and offers
 * add / remove / on-off in the browser and the app. There is no socket here: the callers are inside MPC, and an apply
 * is the same one the RACK page starts (the edits saved, MPC stopped, the project edited, MPC started again).
 *   int   mpc_sample_rack_api(void)                       the interface version (1)
 *   char *mpc_sample_rack_state(void)                     a JSON document (malloc'd; free it): the channels with their
 *                                                         inserts, the plugins MPC knows, whether an apply is running
 *   int   mpc_sample_rack_apply(const char *json, char *why, size_t wn)
 *                                                         {"edits":[{"op":"add","slot":"track:0","plugin":"Tuner"},
 *                                                         {"op":"remove","slot":"input","index":1,"name":"NAM"},
 *                                                         {"op":"enable","slot":"input","index":1,"name":"NAM","value":0}]}
 *                                                         0 when MPC is going down for it, else -1 with the reason */
#define _GNU_SOURCE
#include "rack_addin.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
#define MAX_TOK 640

static atomic_int applying;

EXPORT int mpc_sample_rack_api(void) { return 1; }

static char *take(struct sb *b)
{
    if (b->oom) { sb_free(b); return NULL; }
    return b->p;   /* NUL-terminated by sb */
}

EXPORT char *mpc_sample_rack_state(void)
{
    struct sb b = { 0 };
    rack_model *m = malloc(sizeof *m);
    rack_plugin *pl = malloc(RACK_MAX_PLUGINS * sizeof *pl);
    if (!m || !pl) { free(m); free(pl); return NULL; }
    char err[200];
    const rack_settings *C = rack_conf();
    if (rack_load(C->project, m, err, sizeof err)) {
        sb_puts(&b, "{\"ok\":false,\"error\":");
        sb_jstr(&b, err);
        sb_puts(&b, "}");
    } else {
        int np = rack_plugins(C->settings, pl, RACK_MAX_PLUGINS);
        sb_puts(&b, "{\"ok\":true,\"project\":");
        sb_jstr(&b, m->project);
        sb_printf(&b, ",\"applying\":%s,\"max_inserts\":%d,\"slots\":[", atomic_load(&applying) ? "true" : "false", RACK_FX_SLOTS);
        for (int s = 0; s < m->nslots; s++) {
            const rack_slot *sl = &m->slot[s];
            sb_puts(&b, s ? ",{\"id\":" : "{\"id\":");
            sb_jstr(&b, sl->id);
            sb_puts(&b, ",\"label\":");
            sb_jstr(&b, sl->label);
            sb_puts(&b, ",\"fx\":[");
            for (int f = 0; f < sl->nfx; f++) {
                const rack_fx *x = &sl->fx[f];
                sb_printf(&b, "%s{\"index\":%d,\"enabled\":%s,\"vst\":%s,\"name\":", f ? "," : "", x->index, x->enabled ? "true" : "false",
                          x->vst ? "true" : "false");
                sb_jstr(&b, x->name);
                sb_puts(&b, "}");
            }
            sb_puts(&b, "]}");
        }
        sb_puts(&b, "],\"plugins\":[");
        for (int i = 0; i < np; i++) {
            sb_puts(&b, i ? ",{\"name\":" : "{\"name\":");
            sb_jstr(&b, pl[i].name);
            sb_puts(&b, ",\"vendor\":");
            sb_jstr(&b, pl[i].manufacturer);
            sb_puts(&b, "}");
        }
        sb_puts(&b, "]}");
    }
    free(m);
    free(pl);
    return take(&b);
}

static const rack_slot *slot_by_id(const rack_model *m, const char *id)
{
    for (int i = 0; i < m->nslots; i++) if (!strcmp(m->slot[i].id, id)) return &m->slot[i];
    return NULL;
}

static const rack_fx *fx_at(const rack_slot *s, int index)
{
    for (int i = 0; i < s->nfx; i++) if (s->fx[i].index == index) return &s->fx[i];
    return NULL;
}

/* inserts on a slot once the edits so far are applied */
static int count_after(const rack_slot *s, const rack_edit *e, int n)
{
    int c = s->nfx;
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].slot, s->id)) c += e[i].op == RACK_ADD ? 1 : e[i].op == RACK_REMOVE ? -1 : 0;
    return c;
}

/* one of each plugin per project: the page belongs to a plugin and the dial opens it by name */
static int used_after(const rack_model *m, const rack_edit *e, int n, const char *name)
{
    int used = 0;
    for (int s = 0; s < m->nslots; s++)
        for (int f = 0; f < m->slot[s].nfx; f++)
            if (m->slot[s].fx[f].vst && !strcmp(m->slot[s].fx[f].name, name)) used++;
    for (int i = 0; i < n; i++) {
        if (e[i].op == RACK_ADD && !strcmp(e[i].plugin.name, name)) used++;
        if (e[i].op == RACK_REMOVE && !strcmp(e[i].name, name)) used--;
    }
    return used;
}

EXPORT int mpc_sample_rack_apply(const char *json, char *why, size_t wn)
{
    int rc = -1;
    struct jtok *t = malloc(MAX_TOK * sizeof *t);
    rack_model *m = malloc(sizeof *m);
    rack_plugin *pl = malloc(RACK_MAX_PLUGINS * sizeof *pl);
    rack_edit *e = calloc(RACK_MAX_EDITS, sizeof *e);
    if (!t || !m || !pl || !e) { snprintf(why, wn, "out of memory"); goto out; }
    if (atomic_load(&applying)) { snprintf(why, wn, "an apply is already running"); goto out; }
    int nt = json_parse(json, strlen(json), t, MAX_TOK);
    int ka = nt > 0 && t[0].type == J_OBJ ? json_get(json, t, 0, "edits") : -1;
    if (ka < 0 || t[ka].type != J_ARR || t[ka].size < 1) { snprintf(why, wn, "edits must be a non-empty array"); goto out; }
    if (t[ka].size > RACK_MAX_EDITS) { snprintf(why, wn, "at most %d edits at once", RACK_MAX_EDITS); goto out; }
    char err[200];
    const rack_settings *C = rack_conf();
    if (rack_load(C->project, m, err, sizeof err)) { snprintf(why, wn, "%s", err); goto out; }
    int np = rack_plugins(C->settings, pl, RACK_MAX_PLUGINS);
    int n = 0, k = ka + 1;
    for (int i = 0; i < t[ka].size; i++, k = t[k].skip) {
        char op[16], name[64];
        rack_edit *x = &e[n];
        int ko = json_get(json, t, k, "op"), ks = json_get(json, t, k, "slot");
        if (t[k].type != J_OBJ || ko < 0 || ks < 0 || json_str(json, t, ko, op, sizeof op) || json_str(json, t, ks, x->slot, sizeof x->slot)) {
            snprintf(why, wn, "edit %d needs op and slot", i + 1); goto out;
        }
        const rack_slot *sl = slot_by_id(m, x->slot);
        if (!sl) { snprintf(why, wn, "edit %d: no channel %s", i + 1, x->slot); goto out; }
        if (!strcmp(op, "add")) {
            int kp = json_get(json, t, k, "plugin");
            if (kp < 0 || json_str(json, t, kp, name, sizeof name)) { snprintf(why, wn, "edit %d: add needs plugin", i + 1); goto out; }
            int p = 0;
            while (p < np && strcmp(pl[p].name, name)) p++;
            if (p == np) { snprintf(why, wn, "edit %d: MPC doesn't know the plugin %s", i + 1, name); goto out; }
            if (used_after(m, e, n, name)) { snprintf(why, wn, "%s is already in this project (one of each)", name); goto out; }
            if (count_after(sl, e, n) >= RACK_FX_SLOTS) { snprintf(why, wn, "%s has no free insert", sl->label); goto out; }
            x->op = RACK_ADD;
            x->plugin = pl[p];
        } else if (!strcmp(op, "remove") || !strcmp(op, "enable")) {
            double idx = -1;
            int ki = json_get(json, t, k, "index"), kn = json_get(json, t, k, "name"), kv = json_get(json, t, k, "value");
            if (ki < 0 || kn < 0 || json_num(json, t, ki, &idx) || json_str(json, t, kn, name, sizeof name) || idx < 0 || idx >= RACK_FX_SLOTS) {
                snprintf(why, wn, "edit %d: %s needs index and name", i + 1, op); goto out;
            }
            const rack_fx *f = fx_at(sl, (int)idx);
            if (!f || strcmp(f->name, name)) { snprintf(why, wn, "edit %d: %s isn't at insert %d of %s any more", i + 1, name, (int)idx + 1, sl->label); goto out; }
            if (!f->vst) { snprintf(why, wn, "%s is one of MPC's own effects: it stays as it is", name); goto out; }
            x->op = op[0] == 'r' ? RACK_REMOVE : RACK_ENABLE;
            x->index = (int)idx;
            snprintf(x->name, sizeof x->name, "%s", name);
            if (x->op == RACK_ENABLE) {
                int bv;
                double dv;
                if (kv >= 0 && json_bool(json, t, kv, &bv) == 0) x->value = bv;
                else if (kv >= 0 && json_num(json, t, kv, &dv) == 0) x->value = dv != 0;
                else { snprintf(why, wn, "edit %d: enable needs value", i + 1); goto out; }
            }
        } else { snprintf(why, wn, "edit %d: op must be add, remove or enable", i + 1); goto out; }
        n++;
    }
    if (rack_apply_start(e, n, why, wn)) goto out;
    atomic_store(&applying, 1);
    rc = 0;
out:
    free(t); free(m); free(pl); free(e);
    return rc;
}
