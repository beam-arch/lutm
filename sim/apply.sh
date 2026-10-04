#!/bin/bash
#
# Apply the SIM / telephony emulation overlay to a synced LineageOS tree.
#
# Usage: sim/apply.sh [path-to-android-tree]
#
# Idempotent: safe to run more than once.

set -euo pipefail

TREE="${1:-$PWD}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY="$HERE/overlay"

DEVICE="$TREE/device/virt/virtio-common"
CF_MS="$TREE/device/google/cuttlefish/host/commands/modem_simulator"
RIL="$TREE/device/google/cuttlefish/guest/hals/ril/reference-ril/reference-ril.c"
if [ ! -d "$DEVICE" ]; then
    echo "apply.sh: device tree not found at $DEVICE" >&2
    echo "apply.sh: run this after 'repo sync', e.g. '$0 android/lineage'" >&2
    exit 1
fi

for required in "$DEVICE/device-common.mk" "$DEVICE/BoardConfigCommon.mk" \
    "$CF_MS/pdu_parser.cpp" "$RIL"; do
    if [ ! -f "$required" ]; then
        echo "apply.sh: required source not found: $required" >&2
        echo "apply.sh: sync the complete LineageOS tree before applying the overlay" >&2
        exit 1
    fi
done
if ! grep -Eq 'VMADDR_CID_(HOST|LOCAL)' "$RIL"; then
    echo "apply.sh: unsupported guest RIL transport in $RIL; expected a VSOCK CID" >&2
    exit 1
fi

echo "apply.sh: installing overlay into $DEVICE"
cp -r "$OVERLAY/." "$DEVICE/"

# Soong resolves `srcs` relative to the .bp's own directory, so the Cuttlefish
# modem simulator sources are staged inside our module directory (under a
# host/commands/modem_simulator layout, matching their #include paths). The
# host-only main.cpp and cf_device_config.cpp are replaced by our own
# main_virtio.cpp and cf_device_config_virtio.cpp.
MS_SRC="$DEVICE/modem_simulator/src/host/commands/modem_simulator"
mkdir -p "$MS_SRC"
cp "$CF_MS"/*.cpp "$CF_MS"/*.h "$MS_SRC/"
rm -f "$MS_SRC/main.cpp" "$MS_SRC/cf_device_config.cpp"
echo "apply.sh: staged modem simulator sources in ${MS_SRC#"$TREE/"}"

# The staged copy is pristine upstream, so the control-plane and fidelity
# patches are re-applied on every run (they are idempotent).
python3 "$HERE/patch_modem_simulator.py" "$MS_SRC"

MARK_BEGIN="# >>> SIM emulation (managed by sim/apply.sh) >>>"
MARK_END="# <<< SIM emulation <<<"

append_once() {
    local file="$1" body="$2"
    if grep -qF "$MARK_BEGIN" "$file"; then
        echo "apply.sh: already patched: ${file#"$TREE/"}"
        return
    fi
    {
        printf '\n%s\n' "$MARK_BEGIN"
        printf '%s\n' "$body"
        printf '%s\n' "$MARK_END"
    } >> "$file"
    echo "apply.sh: patched: ${file#"$TREE/"}"
}

append_once "$DEVICE/device-common.mk" \
    '$(call inherit-product, device/virt/virtio-common/virtio-sim.mk)'

append_once "$DEVICE/BoardConfigCommon.mk" \
    'include device/virt/virtio-common/virtio-sim-board.mk'

# The Cuttlefish guest RIL connects to the modem over VSOCK. On a real Cuttlefish
# the modem simulator runs on the host, so the RIL targets VMADDR_CID_HOST. Here
# both ends live in the guest, so target VSOCK loopback instead.
if grep -q 'VMADDR_CID_HOST' "$RIL"; then
    sed -i 's/VMADDR_CID_HOST/VMADDR_CID_LOCAL/g' "$RIL"
    echo "apply.sh: patched guest RIL to use VMADDR_CID_LOCAL"
else
    echo "apply.sh: guest RIL already uses VMADDR_CID_LOCAL"
fi

echo "apply.sh: done"
