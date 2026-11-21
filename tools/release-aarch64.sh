#!/usr/bin/env bash
# The release zip: the build (run ./build-aarch64.sh first) packaged with mpc-vst-plugins' shared addin installer, then
# checked as the catalog will check it. Needs mpc-vst-plugins next to this repo (or MPC_VST=/path), from e78fe440 or later:
# older installers don't know the Sample's ac50 service and install sample-rack-apply without its execute bit.
# Usage: tools/release-aarch64.sh <version>   ->  dist/<Name>-<version>-mpc-aarch64.zip
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: $0 <version>" >&2; exit 2; }
MPC_VST="${MPC_VST:-../mpc-vst-plugins}"
grep -q "for s in ac50 " "$MPC_VST/tools/release/addin/addin-lib.sh" || { echo "$MPC_VST: installer too old for the MPC Sample (no ac50)" >&2; exit 1; }
python3 "$MPC_VST/tools/release_addin.py" --dir build/aarch64 --version "$1" --repo jacob-sabella/mpc-addin-sample-rack --license MIT \
  --about "The MPC Sample's plugin rack on its own screen: add, turn on or off and remove plugins on the input, outputs, submixes and tracks." \
  --depends "sample-draw-handler>=0.3.0" -o dist
# release_addin.py describes every aarch64 addin as Gen2; this one is for the MPC Sample. Reword that line in INSTALL.md
# and its SHA256SUMS entry, keeping every other entry of the zip as it was.
python3 - dist/*-"$1"-mpc-aarch64.zip <<'PY'
import hashlib, os, sys, zipfile
path = sys.argv[1]
old = "- A Gen2 MPC OS standalone device (aarch64: Live III, One G2, Key(s) G2). The installer refuses anything else."
new = "- An MPC Sample (aarch64). RACK is for the Sample only; the installer refuses a device that is not aarch64."
with zipfile.ZipFile(path) as z:
    entries = [(i, z.read(i)) for i in z.infolist()]
out = []
for i, d in entries:
    if i.filename.endswith("/INSTALL.md"):
        t = d.decode()
        if old not in t:
            sys.exit("INSTALL.md: the device line changed in release_addin.py, update tools/release-aarch64.sh")
        d = t.replace(old, new).encode()
        digest = hashlib.sha256(d).hexdigest()
    out.append([i, d])
for e in out:
    if e[0].filename.endswith("/SHA256SUMS"):
        e[1] = "".join(("%s  INSTALL.md\n" % digest) if l.endswith("  INSTALL.md") else l + "\n"
                       for l in e[1].decode().splitlines()).encode()
tmp = path + ".tmp"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
    for i, d in out:
        z.writestr(i, d)
os.replace(tmp, path)
PY
python3 "$MPC_VST/tools/catalog_check.py" dist/*-"$1"-mpc-aarch64.zip --catalog --expect-id sample-rack --expect-repo jacob-sabella/mpc-addin-sample-rack
