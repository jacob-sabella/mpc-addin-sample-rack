/* mpc-addin-sample-rack: the rack (the plugins on the MPC Sample's input, outputs, submixes and tracks), read from
 * MPC's working copy of the project and changed by edits applied to that file while MPC is stopped.
 * Shared by the addin (inside MPC: reads, shows, queues edits) and sample-rack-apply (outside MPC: applies them). */
#ifndef RACK_H
#define RACK_H
#include "json.h"
#include <stddef.h>

#define RACK_MAX_SLOTS 40
#define RACK_MAX_FX 8
#define RACK_FX_SLOTS 4        /* insert slots per channel */
#define RACK_MAX_PLUGINS 64
#define RACK_MAX_EDITS 32

/* One insert: index is its slot on the channel (0-3), not its place in the list. vst: a plugin (else MPC's own effect,
 * which the rack shows but never changes). */
typedef struct { int index, enabled, vst; char name[64]; } rack_fx;
/* A channel: id is "input", "output:N", "submix:N" or "track:N"; label is for the screen. */
typedef struct { char id[24], label[48]; int nfx; rack_fx fx[RACK_MAX_FX]; } rack_slot;
typedef struct { int nslots; rack_slot slot[RACK_MAX_SLOTS]; char project[64]; } rack_model;

/* A plugin MPC knows (its settings' plugin list for this device). */
typedef struct { char name[64], manufacturer[64], file[512]; unsigned uid; } rack_plugin;

enum { RACK_ADD = 1, RACK_REMOVE = 2, RACK_ENABLE = 3 };
/* An edit. ADD: plugin on slot (at the lowest free index). REMOVE / ENABLE (value 0/1): the insert at index, only if it
 * is still the one named (the project may have changed since the edit was queued). */
typedef struct { int op; char slot[24]; int index, value; char name[64]; rack_plugin plugin; } rack_edit;

/* The project file: gzip over five header lines and a JSON document. Returns the whole text (malloc, NUL-terminated)
 * with *len and *json (the offset of the JSON), or NULL with the reason in err. zlib is the device's, loaded at run time. */
char *rack_read(const char *path, size_t *len, size_t *json, char *err, size_t en);
/* Write text as path (gzip): to path.new, then renamed over it. 0, or -1 with the reason. */
int rack_write(const char *path, const char *text, size_t len, char *err, size_t en);
/* Tokens for a JSON document (malloc'd into *t). Count, or -1. */
int rack_tokens(const char *s, size_t n, struct jtok **t);
/* The effects array of a slot, or -1. */
int rack_effects(const char *s, const struct jtok *t, const char *slot);
/* The slots and their inserts. 0, or -1. */
int rack_model_of(const char *s, const struct jtok *t, rack_model *m);
/* A whole project file into m (name from original_project_data.txt beside it). 0, or -1 with the reason. */
int rack_load(const char *path, rack_model *m, char *err, size_t en);
/* The effect plugins MPC knows (VST, not instruments), from its settings file. Count. */
int rack_plugins(const char *settings, rack_plugin *out, int max);

/* Apply one edit to the project text (*text grows if needed; json is the JSON's offset). 0 applied, 1 skipped (the
 * insert is gone or changed, or the slot is full), -1 error; why in err. */
int rack_apply(char **text, size_t *len, size_t json, const rack_edit *e, char *err, size_t en);
/* Edits as a file (one per line, tab-separated) and back. */
int rack_edits_save(const char *path, const rack_edit *e, int n);
int rack_edits_load(const char *path, rack_edit *e, int max);

#endif
