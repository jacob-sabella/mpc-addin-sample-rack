/* mpc-addin-sample-rack: the MPC Sample's plugin rack, on its own screen. Preloaded into MPC; starts only in the
 * "MPC Sample" process. Settings: mpc_sample_rack.conf next to the library (or $MPC_SAMPLE_RACK_CONF). */
#define _GNU_SOURCE
#include "rack_addin.h"
#include <ctype.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ADDIN_VERSION
#define ADDIN_VERSION "dev"
#endif

static rack_settings C = {
    .enabled = 1,
    .project = "/data/TempProjects/Temp-Project.xpj",
    .settings = "/data/Settings/Akai/MPC Sample/MPC Sample.settings",
    .service = "ac50.service",
};
const rack_settings *rack_conf(void) { return &C; }

static void addin_dir(char *out, size_t n)
{
    Dl_info di;
    out[0] = 0;
    if (dladdr((void *)addin_dir, &di) && di.dli_fname) {
        snprintf(out, n, "%s", di.dli_fname);
        char *s = strrchr(out, '/');
        if (s) *s = 0; else out[0] = 0;
    }
}

/* key=value lines; the value is the rest of the line (paths have spaces) */
static void conf_load(const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) { RLOG("%s: not understood: %s", path, p); continue; }
        char *k = p, *v = eq + 1;
        char *ke = eq; while (ke > k && isspace((unsigned char)ke[-1])) ke--; *ke = 0;
        while (isspace((unsigned char)*v)) v++;
        size_t vl = strlen(v); while (vl && isspace((unsigned char)v[vl - 1])) v[--vl] = 0;
        if (!strcmp(k, "enabled")) C.enabled = atoi(v) != 0;
        else if (!strcmp(k, "project")) snprintf(C.project, sizeof C.project, "%s", v);
        else if (!strcmp(k, "settings")) snprintf(C.settings, sizeof C.settings, "%s", v);
        else if (!strcmp(k, "service")) snprintf(C.service, sizeof C.service, "%s", v);
        else RLOG("%s: unknown setting %s\n", path, k);
    }
    fclose(f);
}

int rack_apply_start(const rack_edit *e, int n, char *why, size_t wn)
{
    char edits[600], helper[600], unit[64];
    snprintf(edits, sizeof edits, "%s/pending.edits", C.dir);
    snprintf(helper, sizeof helper, "%s/sample-rack-apply", C.dir);
    snprintf(unit, sizeof unit, "sample-rack-apply-%ld", (long)time(NULL));
    if (access(helper, X_OK)) chmod(helper, 0755);   /* the shared installer leaves extra files 644 */
    if (access(helper, X_OK)) { snprintf(why, wn, "%s is missing or not executable: reinstall the addin", helper); return -1; }
    if (rack_edits_save(edits, e, n)) { snprintf(why, wn, "can't write %s", edits); return -1; }
    RLOG("applying %d change(s): %s (unit %s)\n", n, helper, unit);
    pid_t p = fork();
    if (p < 0) { snprintf(why, wn, "can't start the helper"); return -1; }
    if (p == 0) {
        unsetenv("LD_PRELOAD");   /* not MPC's addins in systemd-run */
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        execlp("systemd-run", "systemd-run", "--quiet", "--collect", "--unit", unit, "--", helper, edits, C.project, C.service,
               (char *)NULL);
        _exit(127);
    }
    int st;
    while (waitpid(p, &st, 0) < 0) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { snprintf(why, wn, "systemd-run failed (%d): see journalctl", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return -1; }
    return 0;
}

static int is_sample(void)
{
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return 0;
    exe[k] = 0;
    const char *b = strrchr(exe, '/');
    return !strcmp(b ? b + 1 : exe, "MPC Sample");
}

__attribute__((visibility("default"))) int mpc_sample_rack_start(void)
{
    static atomic_int started;
    if (atomic_exchange(&started, 1)) return 0;
    addin_dir(C.dir, sizeof C.dir);
    char path[700];
    const char *env = getenv("MPC_SAMPLE_RACK_CONF");
    if (env) snprintf(path, sizeof path, "%s", env);
    else snprintf(path, sizeof path, "%s/mpc_sample_rack.conf", C.dir[0] ? C.dir : ".");
    conf_load(path);
    if (!C.enabled) { RLOG("disabled in %s\n", path); return -1; }
    rack_page_start();
    RLOG("%s: project %s, plugins from %s\n", ADDIN_VERSION, C.project, C.settings);
    return 0;
}

__attribute__((constructor)) static void addin_init(void)
{
    if (getenv("MPC_SAMPLE_RACK_DISABLE") || !is_sample()) return;
    mpc_sample_rack_start();
}
