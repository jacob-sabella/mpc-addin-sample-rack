/* Host test of the bridge: state JSON from a project, and apply's checks and the edits it hands to rack_apply_start. */
#include "../src/rack_addin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rack_settings S;
const rack_settings *rack_conf(void) { return &S; }
static rack_edit got[RACK_MAX_EDITS];
static int ngot, starts;
int rack_apply_start(const rack_edit *e, int n, char *why, size_t wn)
{
    (void)why; (void)wn;
    memcpy(got, e, (size_t)n * sizeof *e);
    ngot = n;
    starts++;
    return 0;
}

#include "../src/live.h"
static int nam_live = LIVE_NONE;   /* the stand-in for live.c: NAM's live state */
int live_state(const char *file) { return !strcmp(file, "NAM") ? nam_live : LIVE_NONE; }
int live_set(const char *file, int off) { if (strcmp(file, "NAM") || nam_live == LIVE_NONE) return -1; nam_live = off != 0; return 0; }

#include "../src/bridge.c"   /* in this file, to reset its applying flag between cases */

static int fails;
#define CHECK(c, m) do { if (c) printf("ok   %s\n", m); else { printf("FAIL %s\n", m); fails++; } } while (0)

static int apply(const char *j, char *why) { atomic_store(&applying, 0); why[0] = 0; return mpc_sample_rack_apply(j, why, 200); }

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    snprintf(S.project, sizeof S.project, "%s", argv[1]);
    snprintf(S.settings, sizeof S.settings, "%s", argv[2]);
    char *st = mpc_sample_rack_state();
    CHECK(st && strstr(st, "\"ok\":true") && strstr(st, "\"project\":\"Riff Ideas\""), "state: the project by name");
    CHECK(st && strstr(st, "\"id\":\"input\"") && strstr(st, "\"name\":\"NAM\"") && strstr(st, "\"vst\":true"), "state: the input's inserts");
    CHECK(st && strstr(st, "\"plugins\":[{\"name\":\"Tuner\"") && strstr(st, "Gain \\\"PoC\\\""), "state: the plugins MPC knows, escaped");
    CHECK(st && strstr(st, "\"max_inserts\":4"), "state: insert slots per channel");
    free(st);
    char why[200];
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"track:0\",\"plugin\":\"Tuner\"}]}", why) == 0 &&
          mpc_sample_rack_apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"track:0\",\"plugin\":\"Tuner\"}]}", why, sizeof why) == -1 && strstr(why, "already running"),
          "a second apply while one runs is refused");
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"track:0\",\"plugin\":\"Tuner\"}]}", why) == 0 && ngot == 1 && got[0].op == RACK_ADD &&
          !strcmp(got[0].slot, "track:0") && !strcmp(got[0].plugin.name, "Tuner") && got[0].plugin.uid == 0x54756e72,
          "add: queued with the plugin's file and uid");
    CHECK(apply("{\"edits\":[{\"op\":\"enable\",\"slot\":\"input\",\"index\":1,\"name\":\"NAM\",\"value\":false},"
                "{\"op\":\"remove\",\"slot\":\"input\",\"index\":1,\"name\":\"NAM\"}]}", why) == 0 && ngot == 2 &&
          got[0].op == RACK_ENABLE && got[0].value == 0 && got[1].op == RACK_REMOVE && got[1].index == 1,
          "enable (a boolean) and remove");
    CHECK(apply("{\"edits\":[{\"op\":\"enable\",\"slot\":\"input\",\"index\":1,\"name\":\"NAM\",\"value\":1}]}", why) == 0 && got[0].value == 1,
          "enable with a number");
    int s0 = starts;
    CHECK(apply("{\"edits\":[{\"op\":\"remove\",\"slot\":\"input\",\"index\":0,\"name\":\"LP Filter\"}]}", why) == -1 && strstr(why, "MPC's own") &&
          starts == s0, "an MPC effect is refused and nothing starts");
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"output:1\",\"plugin\":\"NAM\"}]}", why) == -1, "NAM is in the project already: refused");
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"track:0\",\"plugin\":\"Tuner\"},{\"op\":\"add\",\"slot\":\"output:1\",\"plugin\":\"Tuner\"}]}", why) == -1 &&
          strstr(why, "already"), "one of each, also within one request");
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"nowhere\",\"plugin\":\"Tuner\"}]}", why) == -1 && strstr(why, "no channel"), "unknown channel");
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"track:0\",\"plugin\":\"Nope\"}]}", why) == -1 && strstr(why, "doesn't know"), "unknown plugin");
    CHECK(apply("{\"edits\":[{\"op\":\"remove\",\"slot\":\"input\",\"index\":1,\"name\":\"Not NAM\"}]}", why) == -1 && strstr(why, "any more"), "a changed insert");
    CHECK(apply("{\"edits\":[]}", why) == -1 && apply("not json", why) == -1 && apply("{\"edits\":[{\"op\":\"zap\",\"slot\":\"input\"}]}", why) == -1,
          "empty, bad JSON, bad op");
    st = mpc_sample_rack_state();
    CHECK(st && strstr(st, "\"name\":\"NAM\",\"live\":null"), "state: NAM not running, no live state");
    free(st);
    CHECK(mpc_sample_rack_live("input", 1, "NAM", 0, why, sizeof why) == -1 && strstr(why, "isn't running"), "live: not running, refused");
    nam_live = LIVE_ON;
    CHECK(mpc_sample_rack_live("input", 1, "NAM", 0, why, sizeof why) == 0 && nam_live == LIVE_OFF, "live: turned off");
    st = mpc_sample_rack_state();
    CHECK(st && strstr(st, "\"name\":\"NAM\",\"live\":\"off\""), "state: NAM live off");
    free(st);
    CHECK(apply("{\"edits\":[{\"op\":\"add\",\"slot\":\"track:0\",\"plugin\":\"Tuner\"}]}", why) == 0 && ngot == 2 &&
          got[1].op == RACK_ENABLE && !strcmp(got[1].slot, "input") && got[1].index == 1 && !strcmp(got[1].name, "NAM") && got[1].value == 0,
          "apply: the live OFF goes into the project too");
    CHECK(apply("{\"edits\":[{\"op\":\"remove\",\"slot\":\"input\",\"index\":1,\"name\":\"NAM\"}]}", why) == 0 && ngot == 1 &&
          got[0].op == RACK_REMOVE, "apply: not when the edits already say what happens to it");
    nam_live = LIVE_NONE;
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails != 0;
}
