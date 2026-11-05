/* sample-rack-apply: applies the rack's queued edits to MPC's project file with MPC stopped, then starts MPC again.
 *   sample-rack-apply <edits file> <project file> [service]
 * The addin starts it with systemd-run as a unit of its own: MPC's service is stopped with everything in its cgroup, so
 * a child of MPC would be stopped with it. MPC is started again whatever happens (a failed edit leaves the file as it
 * was). Without a service (or RACK_NO_SERVICE set: the tests) it only edits the file. Log: stdout (the journal). */
#define _GNU_SOURCE
#include "rack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int systemctl(const char *verb, const char *service)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) { execlp("systemctl", "systemctl", verb, service, (char *)NULL); _exit(127); }
    int st;
    while (waitpid(p, &st, 0) < 0) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <edits> <project> [service]\n", argv[0]); return 2; }
    const char *edits = argv[1], *project = argv[2];
    const char *service = argc > 3 && !getenv("RACK_NO_SERVICE") ? argv[3] : NULL;
    static rack_edit e[RACK_MAX_EDITS];
    int n = rack_edits_load(edits, e, RACK_MAX_EDITS);
    if (n <= 0) { printf("sample-rack-apply: no edits in %s\n", edits); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (service) {
        printf("sample-rack-apply: stopping %s for %d change(s)\n", service, n);
        if (systemctl("stop", service)) printf("sample-rack-apply: stopping %s failed; editing anyway\n", service);
    }
    char err[256];
    size_t len, js;
    int applied = 0, rc = 0;
    char *text = rack_read(project, &len, &js, err, sizeof err);
    if (!text) { printf("sample-rack-apply: %s\n", err); rc = 1; }
    for (int i = 0; text && i < n; i++) {
        int r = rack_apply(&text, &len, js, &e[i], err, sizeof err);
        const char *what = e[i].op == RACK_ADD ? "add" : e[i].op == RACK_REMOVE ? "remove" : e[i].value ? "on" : "off";
        const char *who = e[i].op == RACK_ADD ? e[i].plugin.name : e[i].name;
        if (r == 0) { applied++; printf("sample-rack-apply: %s %s on %s\n", what, who, e[i].slot); }
        else printf("sample-rack-apply: %s %s on %s skipped: %s\n", what, who, e[i].slot, err);
        if (r < 0) { rc = 1; break; }
    }
    if (text && applied && rc == 0) {
        char bak[1024];
        snprintf(bak, sizeof bak, "%s.rack-backup", project);   /* the file as it was, once per apply */
        size_t blen, bjs;
        char *orig = rack_read(project, &blen, &bjs, err, sizeof err);
        if (orig) { rack_write(bak, orig, blen, err, sizeof err); free(orig); }
        if (rack_write(project, text, len, err, sizeof err)) { printf("sample-rack-apply: %s\n", err); rc = 1; }
        else printf("sample-rack-apply: %d of %d change(s) written to %s (previous copy: %s)\n", applied, n, project, bak);
    }
    free(text);
    unlink(edits);
    if (service) {
        printf("sample-rack-apply: starting %s\n", service);
        if (systemctl("start", service)) { printf("sample-rack-apply: starting %s failed\n", service); rc = 1; }
    }
    return rc;
}
