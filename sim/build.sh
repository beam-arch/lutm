#!/bin/bash
# Usage: bash sim/build.sh <android-tree> [virtio_x86_64|virtio_arm64only]
set -eo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${1:?usage: bash sim/build.sh <android-tree> [product]}"
PRODUCT="${2:-virtio_x86_64}"
case "$PRODUCT" in
    virtio_x86_64|virtio_arm64only) ;;
    *) echo "build.sh: unsupported product: $PRODUCT" >&2; exit 2 ;;
esac

for tool in repo git git-lfs python3 make qemu-img unzip; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "build.sh: required host tool not found: $tool" >&2
        exit 1
    fi
done

mkdir -p "$TREE"
cd "$TREE"
if [ ! -d .repo ]; then
    repo init -u https://github.com/LineageOS/android.git -b lineage-23.2 \
        --depth=1 --git-lfs --no-clone-bundle
fi
mkdir -p .repo/local_manifests
cp "$HERE/lineage-virtio.xml" .repo/local_manifests/lutm-virtio.xml
if [ "${SKIP_SYNC:-0}" != 1 ]; then
    repo sync -c --no-tags --no-clone-bundle -j"${SYNC_JOBS:-8}"
fi

bash "$HERE/apply.sh" "$PWD"
bash "$HERE/host-quirks.sh" --fix "$PWD"
bash "$HERE/run-host-tests.sh" "$PWD"

# Match jqssun/android-lineage-qemu's standalone recovery layout.
export AB_OTA_UPDATER=false ROOMSERVICE_BRANCHES="lineage-23.1 lineage-23.0"
source build/envsetup.sh

check_recovery_layout() {
    local recovery_size
    recovery_size="$(get_build_var BOARD_RECOVERYIMAGE_PARTITION_SIZE)"
    if [ "$(get_build_var AB_OTA_UPDATER)" != false ] || \
        [ "$(get_build_var TARGET_NO_RECOVERY)" = true ] || \
        ! [[ "$recovery_size" =~ ^[0-9]+$ ]] || [ "$recovery_size" -eq 0 ]; then
        echo "build.sh: expected the upstream non-A/B layout with a recovery partition" >&2
        exit 1
    fi
}

IMAGE_ARCH="${PRODUCT#virtio_}"
breakfast "$PRODUCT" userdebug
check_recovery_layout
m -j"${BUILD_JOBS:-$(nproc)}" recoveryimage
PRODUCT_OUT="$(get_build_var PRODUCT_OUT)"
cp "$PRODUCT_OUT/recovery.img" "$PRODUCT_OUT/recovery_${IMAGE_ARCH}-userdebug.img"

breakfast "$PRODUCT" user
check_recovery_layout
m -j"${BUILD_JOBS:-$(nproc)}" vm-utm-zip otapackage

PRODUCT_OUT="$(get_build_var PRODUCT_OUT)"
cp "$PRODUCT_OUT/boot.img" "$PRODUCT_OUT/boot_${IMAGE_ARCH}.img"
cp "$PRODUCT_OUT/recovery.img" "$PRODUCT_OUT/recovery_${IMAGE_ARCH}.img"
shopt -s nullglob
artifacts=("$PRODUCT_OUT"/VirtualMachine/UTM/UTM-VM-*.zip "$PRODUCT_OUT"/*-ota.zip)
if [ "${#artifacts[@]}" -lt 2 ]; then
    echo "build.sh: expected both a UTM bundle and an OTA zip in $PRODUCT_OUT" >&2
    exit 1
fi
for artifact in "${artifacts[@]}"; do
    unzip -tq "$artifact"
done
artifacts+=("$PRODUCT_OUT/boot_${IMAGE_ARCH}.img" \
    "$PRODUCT_OUT/recovery_${IMAGE_ARCH}.img" \
    "$PRODUCT_OUT/recovery_${IMAGE_ARCH}-userdebug.img")
for artifact in "${artifacts[@]}"; do
    if [ ! -s "$artifact" ]; then
        echo "build.sh: missing or empty release artifact: $artifact" >&2
        exit 1
    fi
    sha256sum "$artifact"
done
repo manifest -r -o "$PRODUCT_OUT/lutm-source-manifest.xml"
printf 'build.sh: images built and archive integrity checked; runtime not tested\n'
