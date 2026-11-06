/* Internal to the addin (inside MPC): its settings, the page and starting an apply. */
#ifndef RACK_ADDIN_H
#define RACK_ADDIN_H
#include "rack.h"
#include <stdio.h>

#define RLOG(...) fprintf(stderr, "mpc-sample-rack: " __VA_ARGS__)

typedef struct {
    int enabled;
    char project[512], settings[512], service[64];
    char dir[512];   /* the addin's folder: sample-rack-apply and the queued edits */
} rack_settings;

const rack_settings *rack_conf(void);
void rack_page_start(void);
/* Save the edits and start sample-rack-apply as a systemd unit of its own (it stops MPC, edits, starts MPC). 0, or -1
 * with the reason. */
int rack_apply_start(const rack_edit *e, int n, char *why, size_t wn);
/* Edits e[0..n) with an ENABLE 0 added for each running plugin that is off live and not already in them. New count. */
int rack_fold_live(const rack_model *m, rack_edit *e, int n, int max);

#endif
