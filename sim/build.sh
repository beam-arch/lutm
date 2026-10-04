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
source build/envsetup.sh
breakfast "$PRODUCT" user
m -j"${BUILD_JOBS:-$(nproc)}" vm-utm-zip otapackage

PRODUCT_OUT="$(get_build_var PRODUCT_OUT)"
shopt -s nullglob
artifacts=("$PRODUCT_OUT"/VirtualMachine/UTM/UTM-VM-*.zip "$PRODUCT_OUT"/*-ota.zip)
if [ "${#artifacts[@]}" -lt 2 ]; then
    echo "build.sh: expected both a UTM bundle and an OTA zip in $PRODUCT_OUT" >&2
    exit 1
fi
for artifact in "${artifacts[@]}"; do
    unzip -tq "$artifact"
    sha256sum "$artifact"
done
repo manifest -r -o "$PRODUCT_OUT/lutm-source-manifest.xml"
printf 'build.sh: images built and archive integrity checked; runtime not tested\n'
