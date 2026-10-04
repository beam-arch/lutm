#!/bin/bash
#
# Run the boot-wiring regressions and modem console's host-side tests.
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
MS_SOURCE="$CF/host/commands/modem_simulator"

if [ ! -f "$MS_SOURCE/pdu_parser.cpp" ]; then
    if [ "$#" -gt 0 ]; then
        echo "run-host-tests.sh: Cuttlefish sources not found under $CF" >&2
        exit 1
    fi
    MS_SOURCE="$HERE/reference/modem_simulator/host/commands/modem_simulator"
    echo "run-host-tests.sh: using bundled simulator reference sources"
fi

python3 "$HERE/tests/boot_integration_test.py"
python3 "$HERE/tests/upload_gofile_test.py"

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

# The bundled reference is already patched; the patcher accepts both forms.
SRC="$OUT/src/host/commands/modem_simulator"
mkdir -p "$SRC"
cp "$MS_SOURCE"/*.h "$MS_SOURCE"/*.cpp "$SRC/"
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

"$CXX" -std=c++20 -Wall -Wextra -O1 \
    -I "$HERE/overlay/modem_console" \
    -o "$OUT/modem_console" \
    "$HERE/overlay/modem_console/modem_console.cpp" \
    "$HERE/overlay/modem_console/sms_pdu.cpp"

python3 "$HERE/tests/console_transport_test.py" "$OUT/modem_console"
