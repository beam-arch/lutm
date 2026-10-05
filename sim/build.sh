#!/bin/bash
# Usage: bash sim/build.sh <android-tree> [product] [both|non-ab|ab]
set -eo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${1:?usage: bash sim/build.sh <android-tree> [product] [both|non-ab|ab]}"
PRODUCT="${2:-virtio_x86_64}"
LAYOUT="${3:-both}"
case "$PRODUCT" in
    virtio_x86_64|virtio_arm64only) ;;
    *) echo "build.sh: unsupported product: $PRODUCT" >&2; exit 2 ;;
esac
case "$LAYOUT" in
    both) layouts=(non-ab ab) ;;
    non-ab|ab) layouts=("$LAYOUT") ;;
    *) echo "build.sh: unsupported partition layout: $LAYOUT" >&2; exit 2 ;;
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

# Some Soong modules require output paths relative to the Android tree.
OUTPUT_ROOT="$(python3 - "${OUT_DIR:-out}" <<'PY'
import os
import sys

directory = os.path.relpath(os.path.abspath(sys.argv[1]))
if directory == os.pardir or directory.startswith(os.pardir + os.sep):
    raise SystemExit('build.sh: OUT_DIR must be inside the Android source tree')
print(directory)
PY
)"
RELEASE_ROOT="$OUTPUT_ROOT/releases/$PRODUCT"
IMAGE_ARCH="${PRODUCT#virtio_}"

check_layout() {
    local layout="$1" recovery_size
    if [ "$(get_build_var AB_OTA_UPDATER)" != "$AB_OTA_UPDATER" ]; then
        echo "build.sh: board configuration does not match the requested $layout layout" >&2
        exit 1
    fi
    if [ "$layout" = non-ab ]; then
        recovery_size="$(get_build_var BOARD_RECOVERYIMAGE_PARTITION_SIZE)"
        if [ "$(get_build_var TARGET_NO_RECOVERY)" = true ] || \
            ! [[ "$recovery_size" =~ ^[0-9]+$ ]] || [ "$recovery_size" -eq 0 ]; then
            echo "build.sh: expected the non-A/B layout with a recovery partition" >&2
            exit 1
        fi
    elif [ "$(get_build_var TARGET_NO_RECOVERY)" != true ] || \
        [ "$(get_build_var BOARD_INCLUDE_RECOVERY_RAMDISK_IN_VENDOR_BOOT)" != true ]; then
        echo "build.sh: expected the A/B layout with recovery in vendor_boot" >&2
        exit 1
    fi
}

build_layout() (
    layout="$1"
    export OUT_DIR="$OUTPUT_ROOT/$layout"
    export ROOMSERVICE_BRANCHES="lineage-23.1 lineage-23.0"
    if [ "$layout" = ab ]; then
        export AB_OTA_UPDATER=true
    else
        export AB_OTA_UPDATER=false
    fi
    source build/envsetup.sh

    if [ "$layout" = non-ab ]; then
        breakfast "$PRODUCT" userdebug
        check_layout "$layout"
        m -j"${BUILD_JOBS:-$(nproc)}" recoveryimage
        PRODUCT_OUT="$(get_build_var PRODUCT_OUT)"
        cp "$PRODUCT_OUT/recovery.img" "$PRODUCT_OUT/recovery_${IMAGE_ARCH}-userdebug.img"
    fi

    breakfast "$PRODUCT" user
    check_layout "$layout"
    m -j"${BUILD_JOBS:-$(nproc)}" vm-utm-zip otapackage

    PRODUCT_OUT="$(get_build_var PRODUCT_OUT)"
    version="$(get_build_var LINEAGE_VERSION)"
    utm_file="$PRODUCT_OUT/VirtualMachine/UTM/UTM-VM-lineage-$version.zip"
    ota_file="$PRODUCT_OUT/lineage_$PRODUCT-ota.zip"
    if [ -z "$version" ] || [ ! -s "$utm_file" ] || [ ! -s "$ota_file" ]; then
        echo "build.sh: expected the current UTM bundle and OTA zip in $PRODUCT_OUT" >&2
        exit 1
    fi
    for artifact in "$utm_file" "$ota_file"; do
        unzip -tq "$artifact"
    done

    release_dir="$RELEASE_ROOT/$layout"
    mkdir -p "$release_dir"
    utm_name="$(basename "$utm_file" .zip)-$layout.zip"
    ota_name="$(basename "$ota_file" -ota.zip)-$layout-ota.zip"
    cp "$utm_file" "$release_dir/$utm_name"
    cp "$ota_file" "$release_dir/$ota_name"
    cp "$PRODUCT_OUT/boot.img" "$release_dir/boot_${IMAGE_ARCH}-$layout.img"
    artifacts=("$utm_name" "$ota_name" "boot_${IMAGE_ARCH}-$layout.img")
    if [ "$layout" = non-ab ]; then
        cp "$PRODUCT_OUT/recovery.img" "$release_dir/recovery_${IMAGE_ARCH}-$layout.img"
        cp "$PRODUCT_OUT/recovery_${IMAGE_ARCH}-userdebug.img" \
            "$release_dir/recovery_${IMAGE_ARCH}-$layout-userdebug.img"
        artifacts+=("recovery_${IMAGE_ARCH}-$layout.img" \
            "recovery_${IMAGE_ARCH}-$layout-userdebug.img")
    else
        cp "$PRODUCT_OUT/vendor_boot.img" "$release_dir/vendor_boot_${IMAGE_ARCH}-$layout.img"
        artifacts+=("vendor_boot_${IMAGE_ARCH}-$layout.img")
    fi
    for artifact in "${artifacts[@]}"; do
        if [ ! -s "$release_dir/$artifact" ]; then
            echo "build.sh: missing or empty release artifact: $artifact" >&2
            exit 1
        fi
    done
    repo manifest -r -o "$release_dir/lutm-source-manifest-$layout.xml"
    python3 - "$PRODUCT" "$layout" "$release_dir" "${artifacts[@]}" <<'PY'
import json
from pathlib import Path
import sys

product, layout, directory, *artifacts = sys.argv[1:]
metadata = {
    "product": product,
    "partition_layout": layout,
    "ab_ota_updater": layout == "ab",
    "recovery_partition": layout == "non-ab",
    "recovery_location": "vendor_boot" if layout == "ab" else "recovery",
    "artifacts": artifacts,
}
(Path(directory) / "release.json").write_text(json.dumps(metadata, indent=2) + "\n")
PY
    (
        cd "$release_dir"
        sha256sum "${artifacts[@]}" release.json "lutm-source-manifest-$layout.xml" > SHA256SUMS
        cat SHA256SUMS
    )
    printf 'build.sh: %s release staged in %s; runtime not tested\n' "$layout" "$release_dir"
)

for layout in "${layouts[@]}"; do
    build_layout "$layout"
done
printf 'build.sh: images built and archive integrity checked; runtime not tested\n'
