#!/usr/bin/env bash
# Host tests (x86, ASan+UBSan): the core reads a project and the plugin list and saves edits (core_test), then
# sample-rack-apply applies them to the file (no service), checked with Python: everything else byte-for-byte the same.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/host; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$B"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cc -std=gnu11 -O1 -Wall -Wextra -Werror $SAN -o "$B/core_test" tests/core_test.c src/rack.c src/json.c -ldl -lpthread
cc -std=gnu11 -O1 -Wall -Wextra -Werror $SAN -o "$B/bridge_test" tests/bridge_test.c src/rack.c src/json.c -ldl -lpthread
cc -std=gnu11 -O1 -Wall -Wextra -Werror $SAN -o "$B/sample-rack-apply" src/apply.c src/rack.c src/json.c -ldl -lpthread
mkdir -p "$T/p"
python3 tests/fixture.py "$T/p/Temp-Project.xpj" "$T/settings"
cp "$T/p/Temp-Project.xpj" "$T/orig.xpj"
ASAN_OPTIONS=detect_leaks=1 "$B/core_test" "$T/p/Temp-Project.xpj" "$T/settings" "$T/edits"
ASAN_OPTIONS=detect_leaks=1 "$B/bridge_test" "$T/p/Temp-Project.xpj" "$T/settings"
RACK_NO_SERVICE=1 ASAN_OPTIONS=detect_leaks=1 "$B/sample-rack-apply" "$T/edits" "$T/p/Temp-Project.xpj" ac50.service | tee "$T/log"
python3 - "$T/orig.xpj" "$T/p/Temp-Project.xpj" "$T/log" "$T/p/Temp-Project.xpj.rack-backup" <<'PY'
import gzip, json, sys
fails = 0
def check(c, m):
    global fails
    print(("ok   " if c else "FAIL ") + m); fails += not c
raw = lambda p: gzip.open(p).read()
o, n, log = raw(sys.argv[1]), raw(sys.argv[2]), open(sys.argv[3]).read()
check(n.split(b"\n", 5)[:5] == o.split(b"\n", 5)[:5], "the header lines are kept")
d = json.loads(n.split(b"\n", 5)[5])["data"]
names = lambda m: [(e["index"], e["plugin"]["description"]["name"], e["enable"]) for e in m["inserts"]["effects"]]
check(names(d["tracks"][0]["program"]["mixable"]) == [(0, 'Gain "PoC"', True)], "added into an empty list at insert 1")
e = d["tracks"][0]["program"]["mixable"]["inserts"]["effects"][0]["plugin"]
check(e["description"]["fileOrIdentifier"] == "/g g/gain.so" and e["description"]["uid"] == 0x46476e50 and
      e["description"]["pluginFormatName"] == "VST" and e["state"] == "", "as a VST with its file and uid")
check(names(d["mixer"]["input"]["mixable"]) == [(0, "LP Filter", True), (1, "NAM", False), (2, "Tuner", True), (3, 'Gain "PoC"', True)],
      "input: Tuner after the two, NAM off, one more fills insert 4")
check(names(d["mixer"]["outputs"][0]["mixable"]) == [(0, "Pad-Controlled FXs", True), (2, "Color Compressor", True)], "a middle one removed, indexes kept")
check(names(d["mixer"]["submixes"][0]["mixable"]) == [], "the only one removed")
check(names(d["tracks"][1]["program"]["mixable"]) == [(0, "LP Filter", True)], "the mirror program untouched")
check("Not NAM isn't at insert 2" in log and "no free insert slot" in log and "6 of 8 change(s) written" in log, "changed and full ones skipped, and said so")
check(raw(sys.argv[4]) == o, "the previous file kept as .rack-backup")
# everything outside the edited lists byte-for-byte the same: blank out the five effects lists in both and compare
def blank(b):
    t = json.loads(b.split(b"\n", 5)[5])
    for m in [t["data"]["tracks"][0]["program"], t["data"]["mixer"]["input"], t["data"]["mixer"]["outputs"][0], t["data"]["mixer"]["submixes"][0]]:
        m["mixable"]["inserts"]["effects"] = None
    return t
check(blank(n) == blank(o), "nothing else changed")
print("PASSED" if not fails else "FAILED"); sys.exit(fails != 0)
PY

# Live on/off: the real addin (unsanitised: it interposes dlsym, as the sanitizers' runtimes need it while they start)
# preloaded into a stand-in "MPC Sample" that loads a stand-in effect, alone and ahead of another addin that hooks dlsym.
W="-std=gnu11 -O1 -g -Wall -Wextra -Werror"
cc $W -fPIC -shared -fvisibility=hidden -DADDIN_VERSION=\"test\" -o "$B/mpc_sample_rack.so" src/addin.c src/page.c src/bridge.c src/live.c src/rack.c src/json.c -ldl -lpthread
cc $W -fPIC -shared -fvisibility=hidden -o "$B/fake_fx.so" tests/fake_fx.c
cc $W -fPIC -shared -o "$B/other_hook.so" tests/other_hook.c -ldl
cc $W -o "$B/MPC Sample" tests/live_host.c -ldl -lm
mkdir -p "$T/l"
FIXTURE_FILE_NAM="$PWD/$B/fake_fx.so" python3 tests/fixture.py "$T/l/Temp-Project.xpj" "$T/l/settings"
printf 'project=%s\nsettings=%s\n' "$T/l/Temp-Project.xpj" "$T/l/settings" > "$T/l/rack.conf"
export MPC_SAMPLE_RACK_CONF="$T/l/rack.conf"
LD_PRELOAD="$PWD/$B/mpc_sample_rack.so" "$B/MPC Sample" "$PWD/$B/fake_fx.so" 2>"$T/l/log1" || { cat "$T/l/log1"; exit 1; }
LD_PRELOAD="$PWD/$B/mpc_sample_rack.so:$PWD/$B/other_hook.so" "$B/MPC Sample" "$PWD/$B/fake_fx.so" other 2>"$T/l/log2" || { cat "$T/l/log2"; exit 1; }
grep -q "plugin lookups go on to .*other_hook.so" "$T/l/log2" || { echo "FAIL the rack didn't hand the lookup on"; cat "$T/l/log2"; exit 1; }
echo "ok   the lookup handed on to the next addin that hooks dlsym"
