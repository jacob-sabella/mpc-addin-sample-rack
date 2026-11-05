#!/usr/bin/env bash
# The release zip: the build (run ./build-aarch64.sh first) packaged with mpc-vst-plugins' shared addin installer, then
# checked as the catalog will check it. Needs mpc-vst-plugins next to this repo (or MPC_VST=/path).
# Usage: tools/release-aarch64.sh <version>   ->  dist/<Name>-<version>-mpc-aarch64.zip
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: $0 <version>" >&2; exit 2; }
MPC_VST="${MPC_VST:-../mpc-vst-plugins}"
python3 "$MPC_VST/tools/release_addin.py" --dir build/aarch64 --version "$1" --repo jacob-sabella/mpc-addin-sample-rack --license MIT \
  --about "The MPC Sample's plugin rack on its own screen: add, turn on or off and remove plugins on the input, outputs, submixes and tracks." -o dist
python3 "$MPC_VST/tools/catalog_check.py" dist/*-"$1"-mpc-aarch64.zip --catalog --expect-id sample-rack --expect-repo jacob-sabella/mpc-addin-sample-rack
