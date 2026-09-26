#!/bin/bash
#
# Run the modem console's host-side tests.
#
# Usage: sim/run-host-tests.sh [path-to-android-tree]
#
# Compiles the console's PDU codec together with the *real* PDU parser from the
# Cuttlefish modem simulator, so the encoder is checked against the exact code
# that will validate it in the guest. No device or build output is needed.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${1:-$HERE/../android/lineage}"
CF="$TREE/device/google/cuttlefish"

if [ ! -f "$CF/host/commands/modem_simulator/pdu_parser.cpp" ]; then
    echo "run-host-tests.sh: Cuttlefish sources not found under $CF" >&2
    echo "run-host-tests.sh: pass the path to a synced tree, e.g. '$0 android/lineage'" >&2
    exit 1
fi

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    for candidate in c++ g++ clang++; do
        if command -v "$candidate" >/dev/null 2>&1; then
            CXX="$candidate"
            break
        fi
    done
fi
if [ -z "$CXX" ]; then
    # Fall back to the toolchain the Android build itself uses.
    CXX="$(ls -d "$TREE"/prebuilts/clang/host/linux-x86/*/bin/clang++ 2>/dev/null | sort | tail -1 || true)"
fi
if [ -z "$CXX" ]; then
    echo "run-host-tests.sh: no C++ compiler found (set CXX=...)" >&2
    exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Patch a pristine copy of the simulator sources with the same script sim/apply.sh
# runs, so the tests exercise the exact code the guest will run (and prove the
# patch still applies to upstream).
SRC="$OUT/src/host/commands/modem_simulator"
mkdir -p "$SRC"
cp "$CF"/host/commands/modem_simulator/*.h "$CF"/host/commands/modem_simulator/*.cpp "$SRC/"
python3 "$HERE/patch_modem_simulator.py" "$SRC"

echo "run-host-tests.sh: compiler: $CXX"
"$CXX" -std=c++20 -Wall -Wextra -O1 \
    -I "$HERE/overlay/modem_console" \
    -I "$OUT/src" \
    -o "$OUT/pdu_test" \
    "$HERE/tests/pdu_test.cpp" \
    "$HERE/overlay/modem_console/sms_pdu.cpp" \
    "$SRC/pdu_parser.cpp"

"$OUT/pdu_test"
