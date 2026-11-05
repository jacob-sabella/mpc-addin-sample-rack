/* Host tests of the core: the slots and inserts read from a project, the plugin list, and edits applied to the text. */
#include "../src/rack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (c) printf("ok   %s\n", m); else { printf("FAIL %s\n", m); fails++; } } while (0)

static const rack_slot *slot(const rack_model *m, const char *id)
{
    for (int i = 0; i < m->nslots; i++) if (!strcmp(m->slot[i].id, id)) return &m->slot[i];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    const char *proj = argv[1], *settings = argv[2];
    char err[256];
    rack_model m;
    CHECK(rack_load(proj, &m, err, sizeof err) == 0, "the project loads");
    CHECK(!strcmp(m.project, "Riff Ideas"), "its name from original_project_data.txt");
    CHECK(m.nslots == 5, "input, two outputs, a submix and the drum track (not the mirror programs)");
    const rack_slot *in = slot(&m, "input"), *main_out = slot(&m, "output:0"), *drum = slot(&m, "track:0");
    CHECK(in && in->nfx == 2 && in->fx[1].vst && !in->fx[0].vst && !strcmp(in->fx[1].name, "NAM"), "the input's inserts, NAM a plugin");
    CHECK(main_out && !strcmp(main_out->label, "MAIN OUT") && main_out->nfx == 3, "the main out and MPC's own effects on it");
    CHECK(slot(&m, "output:1") && !strcmp(slot(&m, "output:1")->label, "OUT 3/4"), "the second output");
    CHECK(drum && !strcmp(drum->label, "Drum 001") && drum->nfx == 0, "the drum track by name, empty");
    CHECK(!slot(&m, "track:1") && !slot(&m, "track:2"), "submix and output programs left out");

    rack_plugin pl[8];
    int np = rack_plugins(settings, pl, 8);
    CHECK(np == 2, "two effect plugins (not the instrument, not another device's list)");
    CHECK(np == 2 && !strcmp(pl[0].name, "Tuner") && pl[0].uid == 0x54756e72, "Tuner with its uid");
    CHECK(np == 2 && !strcmp(pl[1].name, "Gain \"PoC\"") && !strcmp(pl[1].manufacturer, "j & s") && !strcmp(pl[1].file, "/g g/gain.so"),
          "XML entities and spaces in the path");

    rack_edit e[8];
    memset(e, 0, sizeof e);
    e[0] = (rack_edit){ .op = RACK_ADD, .slot = "track:0", .plugin = pl[1] };                          /* into an empty list */
    e[1] = (rack_edit){ .op = RACK_ADD, .slot = "input", .plugin = pl[0] };                             /* after two */
    e[2] = (rack_edit){ .op = RACK_ENABLE, .slot = "input", .index = 1, .value = 0, .name = "NAM" };
    e[3] = (rack_edit){ .op = RACK_REMOVE, .slot = "output:0", .index = 1, .name = "Flex Beat Player" };   /* middle */
    e[4] = (rack_edit){ .op = RACK_REMOVE, .slot = "submix:0", .index = 0, .name = "LP Filter" };          /* the only one */
    e[5] = (rack_edit){ .op = RACK_REMOVE, .slot = "input", .index = 1, .name = "Not NAM" };               /* changed: skipped */
    e[6] = (rack_edit){ .op = RACK_ADD, .slot = "input", .plugin = pl[1] };                             /* fills index 3 */
    e[7] = (rack_edit){ .op = RACK_ADD, .slot = "input", .plugin = pl[1] };                             /* full: skipped */
    CHECK(rack_edits_save(argv[3], e, 8) == 0, "edits saved");
    rack_edit back[8];
    CHECK(rack_edits_load(argv[3], back, 8) == 8 && !strcmp(back[0].plugin.file, "/g g/gain.so") && back[0].plugin.uid == 0x46476e50 &&
          !strcmp(back[0].plugin.name, "Gain \"PoC\"") && back[5].op == RACK_REMOVE && !strcmp(back[5].name, "Not NAM"),
          "and read back the same");
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails != 0;
}
