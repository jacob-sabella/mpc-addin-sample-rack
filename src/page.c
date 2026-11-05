/* mpc-addin-sample-rack, the RACK page on the MPC Sample's screen (through mpc-addin-sample-draw-handler).
 *
 * The list: every channel (input, outputs, submixes, tracks) with its inserts, and "+ ADD PLUGIN" where an insert slot
 * is free. The dial moves; the keys above the screen act on the selected row (their labels say what they do). MPC's own
 * effects are shown and left alone. Changes queue up (pink) until APPLY, which restarts MPC with them in the project.
 * Keys: plugin row  ON/OFF  REMOVE  APPLY;  "+ ADD" row  ADD  -  APPLY;  queued add  -  UNDO  APPLY.
 * ADD opens the list of effect plugins MPC knows; APPLY asks first. Pressing the dial in on a plugin opens that plugin's
 * own page if it has one (by its name; a long hold of the page button comes back here); on "+ ADD" or in the list of
 * plugins it does what the middle-key ADD does. */
#define _GNU_SOURCE
#include "rack_addin.h"
#include "sample_draw.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { M_LIST, M_BROWSE, M_CONFIRM, M_APPLYING, M_MESSAGE };
enum { R_HEAD, R_FX, R_NEW, R_ADD };
typedef struct { int kind, slot, fx, edit; } row;

static struct {
    pthread_mutex_t mu;
    rack_model m;
    char err[160];
    rack_edit e[RACK_MAX_EDITS]; int ne;
    rack_plugin pl[RACK_MAX_PLUGINS]; int np;
    int mode, sel, top, bsel, btop, add_slot;
    char msg[160];
    long long last_frame;
} P = { .mu = PTHREAD_MUTEX_INITIALIZER };
static sdc_page page;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---- the queued edits ---- */
static int find(int op, const char *slot, int index, const char *name)
{
    for (int i = 0; i < P.ne; i++)
        if (P.e[i].op == op && !strcmp(P.e[i].slot, slot) && (op == RACK_ADD || (P.e[i].index == index && !strcmp(P.e[i].name, name))))
            return i;
    return -1;
}
static void drop(int i) { if (i >= 0 && i < P.ne) { memmove(&P.e[i], &P.e[i + 1], (size_t)(P.ne - i - 1) * sizeof *P.e); P.ne--; } }
static int queue(rack_edit e) { if (P.ne >= RACK_MAX_EDITS) return -1; P.e[P.ne++] = e; return 0; }
static int fx_removed(const rack_slot *s, const rack_fx *f) { return find(RACK_REMOVE, s->id, f->index, f->name) >= 0; }
static int fx_on(const rack_slot *s, const rack_fx *f)
{
    int i = find(RACK_ENABLE, s->id, f->index, f->name);
    return i >= 0 ? P.e[i].value : f->enabled;
}
static int slot_count(const rack_slot *s)   /* inserts once the queue is applied */
{
    int n = 0;
    for (int i = 0; i < s->nfx; i++) n += !fx_removed(s, &s->fx[i]);
    for (int i = 0; i < P.ne; i++) n += P.e[i].op == RACK_ADD && !strcmp(P.e[i].slot, s->id);
    return n;
}

/* Where plugin name already is once the queue is applied (its channel's label), or NULL. One of each plugin per project:
 * a page belongs to one plugin, and the dial press opens it by name. */
static const char *in_use(const char *name)
{
    for (int s = 0; s < P.m.nslots; s++) {
        const rack_slot *sl = &P.m.slot[s];
        for (int f = 0; f < sl->nfx; f++)
            if (sl->fx[f].vst && !strcmp(sl->fx[f].name, name) && !fx_removed(sl, &sl->fx[f])) return sl->label;
    }
    for (int i = 0; i < P.ne; i++)
        if (P.e[i].op == RACK_ADD && !strcmp(P.e[i].plugin.name, name))
            for (int s = 0; s < P.m.nslots; s++) if (!strcmp(P.m.slot[s].id, P.e[i].slot)) return P.m.slot[s].label;
    return NULL;
}

/* ---- the rows: headers aren't selectable ---- */
#define MAX_ROWS (RACK_MAX_SLOTS * (RACK_MAX_FX + 2) + RACK_MAX_EDITS)
static int rows(row *r)
{
    int n = 0;
    for (int s = 0; s < P.m.nslots; s++) {
        rack_slot *sl = &P.m.slot[s];
        r[n++] = (row){ R_HEAD, s, -1, -1 };
        for (int f = 0; f < sl->nfx; f++) r[n++] = (row){ R_FX, s, f, -1 };
        for (int i = 0; i < P.ne; i++) if (P.e[i].op == RACK_ADD && !strcmp(P.e[i].slot, sl->id)) r[n++] = (row){ R_NEW, s, -1, i };
        if (slot_count(sl) < RACK_FX_SLOTS) r[n++] = (row){ R_ADD, s, -1, -1 };
    }
    return n;
}
/* the selected row (selectable rows counted), or -1 */
static int selected(const row *r, int n)
{
    for (int i = 0, k = 0; i < n; i++) if (r[i].kind != R_HEAD && k++ == P.sel) return i;
    return -1;
}
static int selectable(const row *r, int n) { int k = 0; for (int i = 0; i < n; i++) k += r[i].kind != R_HEAD; return k; }

/* ---- input (the draw handler's thread) ---- */
static void key(int k)
{
    row r[MAX_ROWS];
    int n = rows(r), s = selected(r, n);
    if (P.mode == M_MESSAGE) { P.mode = M_LIST; return; }
    if (k == 3) {   /* the dial pressed in */
        if (P.mode == M_BROWSE) k = 2;
        else if (P.mode != M_LIST || s < 0) return;
        else if (r[s].kind == R_ADD) k = 0;
        else if (r[s].kind == R_FX) {
            rack_fx *f = &P.m.slot[r[s].slot].fx[r[s].fx];
            if (sdc_page_show(&page, f->name) != 0) {
                snprintf(P.msg, sizeof P.msg, f->vst ? "%s HAS NO PAGE ON THIS SCREEN" : "%s IS ONE OF MPC'S OWN EFFECTS", f->name);
                P.mode = M_MESSAGE;
            }
            return;
        } else return;
    }
    if (P.mode == M_BROWSE) {
        if (k == 0) P.mode = M_LIST;
        else if (k == 2 && P.bsel < P.np && !in_use(P.pl[P.bsel].name)) {
            rack_edit e = { .op = RACK_ADD, .plugin = P.pl[P.bsel] };
            snprintf(e.slot, sizeof e.slot, "%s", P.m.slot[P.add_slot].id);
            queue(e);
            P.mode = M_LIST;
        }
        return;
    }
    if (P.mode == M_CONFIRM) {
        if (k == 0) P.mode = M_LIST;
        else if (k == 2) {
            char why[160];
            if (rack_apply_start(P.e, P.ne, why, sizeof why) == 0) P.mode = M_APPLYING;
            else { snprintf(P.msg, sizeof P.msg, "%s", why); P.mode = M_MESSAGE; }
        }
        return;
    }
    if (P.mode != M_LIST) return;
    if (k == 2) { if (P.ne) P.mode = M_CONFIRM; return; }
    if (s < 0) return;
    rack_slot *sl = &P.m.slot[r[s].slot];
    if (r[s].kind == R_FX) {
        rack_fx *f = &sl->fx[r[s].fx];
        if (!f->vst) return;   /* MPC's own effects stay as they are */
        int en = find(RACK_ENABLE, sl->id, f->index, f->name), rm = find(RACK_REMOVE, sl->id, f->index, f->name);
        rack_edit e = { .slot = "", .index = f->index };
        snprintf(e.slot, sizeof e.slot, "%s", sl->id);
        snprintf(e.name, sizeof e.name, "%s", f->name);
        if (k == 0 && rm < 0) {   /* on/off: queue the opposite, or take back a queued one */
            if (en >= 0) drop(en);
            else { e.op = RACK_ENABLE; e.value = !f->enabled; queue(e); }
        } else if (k == 1) {
            if (rm >= 0) drop(rm);
            else { if (en >= 0) drop(en); e.op = RACK_REMOVE; queue(e); }
        }
    } else if (r[s].kind == R_NEW && k == 1) {
        drop(r[s].edit);
    } else if (r[s].kind == R_ADD && k == 0) {
        P.np = rack_plugins(rack_conf()->settings, P.pl, RACK_MAX_PLUGINS);
        P.add_slot = r[s].slot; P.bsel = 0; P.btop = 0;
        P.mode = M_BROWSE;
    }
    n = rows(r);
    int k2 = selectable(r, n);
    if (P.sel >= k2) P.sel = k2 ? k2 - 1 : 0;
}

static void on_input(void *user, const mpc_sdraw_event *ev)
{
    (void)user;
    pthread_mutex_lock(&P.mu);
    if (ev->type == MPC_SDRAW_EV_KEY && ev->value) key(ev->id);
    else if (ev->type == MPC_SDRAW_EV_DIAL) {
        if (P.mode == M_BROWSE) { P.bsel += ev->value; if (P.bsel >= P.np) P.bsel = P.np - 1; if (P.bsel < 0) P.bsel = 0; }
        else if (P.mode == M_LIST) {
            row r[MAX_ROWS]; int k = selectable(r, rows(r));
            P.sel += ev->value; if (P.sel >= k) P.sel = k - 1; if (P.sel < 0) P.sel = 0;
        }
    }
    pthread_mutex_unlock(&P.mu);
}

/* ---- drawing (the page's thread) ---- */
static const uint32_t BG = 0x0b0820, PINK = 0xff2fa0, TEXT = 0xf0e8ff, DIM = 0x8a7ab8, CYAN = 0x22e6ff, PANEL = 0x1a1040,
                      SEL = 0x2e1e60, LOCK = 0x5a4a88;

static void fit(char *out, size_t n, const char *s, int maxc)
{
    if ((int)strlen(s) <= maxc) { snprintf(out, n, "%s", s); return; }
    snprintf(out, n, "%.*s..", maxc - 2 > 0 ? maxc - 2 : 0, s);
}

static void keys(sdc_canvas *cv, const char *a, const char *b, const char *c, int c_on)
{
    const char *k[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        int x = i * 107, w = i == 2 ? cv->w - x : 106;
        sdc_fill(cv, x, 0, w, 20, PANEL);
        if (k[i][0]) sdc_text(cv, x + (w - sdc_text_width(k[i], 2)) / 2, 3, 2, i == 2 ? (c_on ? PINK : LOCK) : TEXT, k[i]);
    }
    sdc_fill(cv, 0, 20, cv->w, 2, PINK);
}

static void list_view(sdc_canvas *cv)
{
    row r[MAX_ROWS];
    int n = rows(r), s = selected(r, n);
    char t[64], lab[16];
    const char *k0 = "", *k1 = "";
    if (s >= 0) {
        rack_slot *sl = &P.m.slot[r[s].slot];
        if (r[s].kind == R_FX && sl->fx[r[s].fx].vst) {
            rack_fx *f = &sl->fx[r[s].fx];
            int rm = fx_removed(sl, f);
            k0 = rm ? "" : fx_on(sl, f) ? "OFF" : "ON";
            k1 = rm ? "UNDO" : "REMOVE";
        } else if (r[s].kind == R_NEW) k1 = "UNDO";
        else if (r[s].kind == R_ADD) k0 = "ADD";
    }
    snprintf(lab, sizeof lab, P.ne ? "APPLY %d" : "APPLY", P.ne);
    keys(cv, k0, k1, lab, P.ne > 0);
    sdc_text(cv, 8, 27, 1, PINK, "RACK");
    fit(t, sizeof t, P.m.project[0] ? P.m.project : "", 30);
    sdc_text(cv, 40, 27, 1, DIM, t);
    if (P.err[0]) { sdc_text(cv, 8, 60, 1, PINK, P.err); return; }

    const int y0 = 38, rh = 19, vis = 10;
    if (s >= 0) {   /* keep the selection on screen, with its channel's header when it fits */
        if (s < P.top) P.top = s > 0 && r[s - 1].kind == R_HEAD ? s - 1 : s;
        if (s >= P.top + vis) P.top = s - vis + 1;
    }
    if (P.top > n - 1) P.top = 0;
    for (int i = P.top; i < n && i < P.top + vis; i++) {
        int y = y0 + (i - P.top) * rh;
        rack_slot *sl = &P.m.slot[r[i].slot];
        if (i == s) { sdc_fill(cv, 0, y - 2, cv->w, rh, SEL); sdc_fill(cv, 0, y - 2, 3, rh, PINK); }
        if (r[i].kind == R_HEAD) {
            fit(t, sizeof t, sl->label, 24);
            sdc_text(cv, 8, y, 2, CYAN, t);
        } else if (r[i].kind == R_FX) {
            rack_fx *f = &sl->fx[r[i].fx];
            int rm = fx_removed(sl, f), on = fx_on(sl, f), changed = rm || on != f->enabled;
            char nm[64];
            snprintf(nm, sizeof nm, "%d %s", f->index + 1, f->name);
            fit(t, sizeof t, nm, 19);
            sdc_text(cv, 20, y, 2, !f->vst ? LOCK : changed ? PINK : TEXT, t);
            const char *st = !f->vst ? "MPC" : rm ? "REMOVE" : on ? "ON" : "OFF";
            sdc_text(cv, cv->w - 8 - sdc_text_width(st, f->vst ? 2 : 1), f->vst ? y : y + 4, f->vst ? 2 : 1,
                     !f->vst ? LOCK : changed ? PINK : on ? CYAN : DIM, st);
            if (rm) sdc_fill(cv, 20, y + 6, sdc_text_width(t, 2), 2, PINK);
        } else if (r[i].kind == R_NEW) {
            char nm[64];
            snprintf(nm, sizeof nm, "+ %s", P.e[r[i].edit].plugin.name);
            fit(t, sizeof t, nm, 19);
            sdc_text(cv, 20, y, 2, PINK, t);
            sdc_text(cv, cv->w - 8 - sdc_text_width("NEW", 2), y, 2, PINK, "NEW");
        } else {
            sdc_text(cv, 20, y, 2, DIM, "+ ADD PLUGIN");
        }
    }
    sdc_text(cv, 8, 230, 1, LOCK, "DIAL: MOVE  PRESS: OPEN  HOLD PAD FX: CLOSE");
}

static void browse_view(sdc_canvas *cv)
{
    keys(cv, "BACK", "", "ADD", P.np > 0 && P.bsel < P.np && !in_use(P.pl[P.bsel].name));
    char t[64], h[80];
    snprintf(h, sizeof h, "ADD TO %s", P.m.slot[P.add_slot].label);
    fit(t, sizeof t, h, 50);
    sdc_text(cv, 8, 27, 1, PINK, t);
    if (!P.np) { sdc_text(cv, 8, 50, 2, DIM, "NO PLUGINS FOUND"); sdc_text(cv, 8, 72, 1, DIM, "INSTALL ONE, THEN COME BACK"); return; }
    const int y0 = 40, rh = 22, vis = 8;
    if (P.bsel < P.btop) P.btop = P.bsel;
    if (P.bsel >= P.btop + vis) P.btop = P.bsel - vis + 1;
    for (int i = P.btop; i < P.np && i < P.btop + vis; i++) {
        int y = y0 + (i - P.btop) * rh;
        if (i == P.bsel) { sdc_fill(cv, 0, y - 3, cv->w, rh, SEL); sdc_fill(cv, 0, y - 3, 3, rh, PINK); }
        const char *used = in_use(P.pl[i].name);
        fit(t, sizeof t, P.pl[i].name, 24);
        sdc_text(cv, 12, y, 2, used ? LOCK : TEXT, t);
        if (used) snprintf(h, sizeof h, "ON %s", used);
        fit(t, sizeof t, used ? h : P.pl[i].manufacturer, 40);
        sdc_text(cv, cv->w - 8 - sdc_text_width(t, 1), y + 4, 1, DIM, t);
    }
    sdc_text(cv, 8, 230, 1, LOCK, "DIAL: CHOOSE");
}

static void confirm_view(sdc_canvas *cv)
{
    keys(cv, "CANCEL", "", "RESTART", 1);
    char t[64];
    snprintf(t, sizeof t, "APPLY %d CHANGE%s?", P.ne, P.ne == 1 ? "" : "S");
    sdc_text(cv, 8, 40, 2, TEXT, t);
    sdc_text(cv, 8, 66, 2, PINK, "MPC RESTARTS");
    sdc_text(cv, 8, 86, 1, DIM, "ABOUT 20 SECONDS, AUDIO STOPS MEANWHILE.");
    sdc_text(cv, 8, 98, 1, DIM, "THE PROJECT COMES BACK WITH THE CHANGES.");
    for (int i = 0, y = 118; i < P.ne && y < 220; i++, y += 12) {
        const rack_edit *e = &P.e[i];
        char line[96];
        const char *slot = e->slot;
        for (int s = 0; s < P.m.nslots; s++) if (!strcmp(P.m.slot[s].id, e->slot)) slot = P.m.slot[s].label;
        if (e->op == RACK_ADD) snprintf(line, sizeof line, "+ %s ON %s", e->plugin.name, slot);
        else if (e->op == RACK_REMOVE) snprintf(line, sizeof line, "- %s ON %s", e->name, slot);
        else snprintf(line, sizeof line, "%s %s ON %s", e->value ? "ON " : "OFF", e->name, slot);
        fit(t, sizeof t, line, 50);
        sdc_text(cv, 8, y, 1, TEXT, t);
    }
}

static void render(void *user, sdc_canvas *cv)
{
    (void)user;
    long long t = now_ms();
    int fresh = t - P.last_frame > 400;   /* the page just came up: read the project again (MPC saves as you go) */
    P.last_frame = t;
    if (fresh && P.mode != M_APPLYING) {
        static rack_model m;
        char err[160];
        int r = rack_load(rack_conf()->project, &m, err, sizeof err);
        pthread_mutex_lock(&P.mu);
        if (r == 0) { P.m = m; P.err[0] = 0; } else snprintf(P.err, sizeof P.err, "%s", err);
        pthread_mutex_unlock(&P.mu);
    }
    pthread_mutex_lock(&P.mu);
    sdc_fill(cv, 0, 0, cv->w, cv->h, BG);
    if (P.mode == M_BROWSE) browse_view(cv);
    else if (P.mode == M_CONFIRM) confirm_view(cv);
    else if (P.mode == M_APPLYING) {
        keys(cv, "", "", "", 0);
        sdc_text(cv, 8, 90, 3, PINK, "RESTARTING");
        sdc_text(cv, 8, 124, 2, TEXT, "MPC COMES BACK IN");
        sdc_text(cv, 8, 144, 2, TEXT, "ABOUT 20 SECONDS");
    } else if (P.mode == M_MESSAGE) {
        keys(cv, "OK", "", "", 0);
        sdc_text(cv, 8, 40, 2, PINK, "COULDN'T APPLY");
        char t[64]; size_t off = 0, L = strlen(P.msg);
        for (int y = 66; off < L && y < 220; y += 12, off += 50) { snprintf(t, sizeof t, "%.50s", P.msg + off); sdc_text(cv, 8, y, 1, TEXT, t); }
    } else list_view(cv);
    pthread_mutex_unlock(&P.mu);
}

/* Look the draw handler up from this thread (preloaded addins start in no set order), then open the page. */
static void *page_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120; i++) {
        if (sdc_page_open(&page, "RACK", 12, render, NULL) == 0) {
            if (sdc_page_set_input(&page, on_input, NULL)) RLOG("the draw handler is too old for input (needs 0.3): the page only shows\n");
            else RLOG("RACK page open\n");
            return NULL;
        }
        struct timespec ts = { 0, 500 * 1000000L };
        nanosleep(&ts, NULL);
    }
    RLOG("no draw handler (mpc-addin-sample-draw-handler): no RACK page\n");
    return NULL;
}

void rack_page_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, page_thread, NULL) == 0) pthread_detach(t);
}
