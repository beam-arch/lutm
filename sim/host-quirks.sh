#!/bin/bash
#
# Host + tree preflight for the SIM / telephony emulation build.
#
# Every check here is a problem that stops the build with an error that looks
# unrelated to SIM emulation. Run this once after `repo sync`:
#
#   sim/host-quirks.sh [--fix] [path-to-android-tree]
#
# Without --fix it only reports and exits non-zero if the build would fail.
# --fix repairs what it can locally (the prebuilts/bootmgr glibc substitution);
# the package-level problems are printed as the exact commands to run, because
# installing packages is a system change this script will not make for you.

set -uo pipefail

FIX=false
if [ "${1:-}" = "--fix" ]; then
    FIX=true
    shift
fi
TREE="${1:-$PWD}"

FAIL=0
probe=""
trap 'rm -rf "${probe:-}"' EXIT
ok()   { printf '  [ ok ] %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n' "$1"; FAIL=1; }
warn() { printf '  [warn] %s\n' "$1"; }

echo "host-quirks: checking $TREE"

# ---------------------------------------------------------------------------
# 1. GNU coreutils
#
# LineageOS gates BOARD_MESA3D_MESON_ARGS on `$(shell expr $(MESA_VERSION_MAJOR)
# \== 25)`. Ubuntu 26.04 ships uutils' expr, where `==` is a syntax error, so the
# variable silently ends up empty, Mesa loses -Dmesa-clc=system and fails with
# `Dependency "libclc" not found`.
# ---------------------------------------------------------------------------
echo
echo "1. expr is GNU coreutils"
if [ "$(expr 1 == 1 2>/dev/null)" = "1" ]; then
    ok "expr supports '=='"
else
    if command -v gnuexpr >/dev/null 2>&1; then
        maybe=" (a GNU expr is available as $(command -v gnuexpr))"
    else
        maybe=""
    fi
    bad "expr does not support '==' -- install GNU coreutils$maybe: apt install coreutils-from-gnu"
    echo "        otherwise Mesa fails with: Dependency \"libclc\" not found"
fi

# ---------------------------------------------------------------------------
# 2. Host tools Mesa's meson setup needs
# ---------------------------------------------------------------------------
echo
echo "2. host tools"
for tool in pkg-config ninja; do
    if command -v "$tool" >/dev/null 2>&1; then
        ok "$tool $(command -v "$tool")"
    else
        case "$tool" in
            pkg-config) pkg=pkg-config ;;
            ninja)      pkg=ninja-build ;;
        esac
        bad "$tool not found -- apt install $pkg"
    fi
done

if python3 -c 'import mako; from mako.template import Template; assert tuple(map(int, mako.__version__.split(".")[:2])) >= (0, 8)' >/dev/null 2>&1; then
    ok "Python Mako >= 0.8.0"
else
    bad "Python Mako >= 0.8.0 not found -- apt install python3-mako"
fi

# ---------------------------------------------------------------------------
# 3. prebuilts/bootmgr's bundled glibc
#
# device/virt/virt-common/build/tasks/10-bootmgr-defs.mk runs the prebuilt
# mtools/xorriso/grub tools through the *bundled* loader:
#
#   BOOTMGR_TOOLS_64_EXEC_ENV := LD_LIBRARY_PATH=<lib64> <lib64>/ld-linux-x86-64.so.2
#
# and 30-create_images.mk then runs `mformat -F -i <img> -v <label> ::`, which
# converts the volume label through codepage 850. On Ubuntu 26.04 the bundled
# glibc cannot do that conversion:
#
#   mformat: Error converting to codepage 850 Invalid argument
#
# which aborts the build while generating the FAT boot images. Pointing the two
# libs at the system glibc fixes it; the other bundled libs (libz, liblzma,
# libpthread) are still used via LD_LIBRARY_PATH.
# ---------------------------------------------------------------------------
echo
echo "3. prebuilts/bootmgr bundled glibc (mformat / codepage 850)"
BM="$TREE/prebuilts/bootmgr"
BM_LD="$BM/lib64/ld-linux-x86-64.so.2"
MT="$BM/tools/linux-x86/bin/mformat"

find_system() {
    local p
    for p in "$@"; do
        [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

if [ ! -d "$BM" ]; then
    warn "not present: $BM (nothing to do)"
elif [ ! -x "$MT" ]; then
    warn "not present: $MT (nothing to do)"
else
    probe="$(mktemp -d)"
    dd if=/dev/zero of="$probe/probe.img" bs=1M count=8 >/dev/null 2>&1
    probe_err=$(LD_LIBRARY_PATH="$BM/lib64" "$BM_LD" "$MT" \
                    -F -i "$probe/probe.img" -v PROBE :: 2>&1)
    probe_rc=$?

    if [ $probe_rc -eq 0 ]; then
        ok "mformat works with the bundled loader"
    elif printf '%s' "$probe_err" | grep -qi 'codepage'; then
        SYS_LD="$(find_system /lib64/ld-linux-x86-64.so.2 \
                              /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 \
                              /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2)"
        SYS_LIBC="$(find_system /usr/lib/x86_64-linux-gnu/libc.so.6 \
                                /lib/x86_64-linux-gnu/libc.so.6 \
                                /usr/lib64/libc.so.6)"
        if [ -z "${SYS_LD:-}" ] || [ -z "${SYS_LIBC:-}" ]; then
            bad "bundled glibc cannot convert to codepage 850 and no system glibc was found"
            echo "        mformat said: $probe_err"
        elif [ "$FIX" = true ]; then
            ln -sfn "$SYS_LD"   "$BM/lib64/ld-linux-x86-64.so.2"
            ln -sfn "$SYS_LIBC" "$BM/lib64/libc.so.6"
            dd if=/dev/zero of="$probe/probe.img" bs=1M count=8 >/dev/null 2>&1
            if LD_LIBRARY_PATH="$BM/lib64" "$BM_LD" "$MT" \
                   -F -i "$probe/probe.img" -v PROBE :: >/dev/null 2>&1; then
                ok "repaired: bootmgr loader/libc now use $SYS_LD / $SYS_LIBC"
            else
                bad "repair did not make mformat work"
            fi
        else
            bad "bundled glibc cannot convert to codepage 850 (rerun with --fix)"
            echo "        mformat said: $probe_err"
            echo "        fix: ln -sfn $SYS_LD $BM_LD"
            echo "             ln -sfn $SYS_LIBC $BM/lib64/libc.so.6"
        fi
    else
        warn "mformat failed for an unrelated reason: $probe_err"
    fi
fi

# ---------------------------------------------------------------------------
# 4. OTA packaging on a QEMU guest
#
# The ramdisk ships a `d -> /sys/kernel/debug` symlink. Info-ZIP `zip`, which
# ota_from_target_files uses, follows symlinks and therefore walks
# /sys/kernel/debug on the host. Where a HID debug node exists (QEMU's virtual
# USB tablet registers one), zip blocks forever reading
# /sys/kernel/debug/hid/*/events. Packaging inside a private mount namespace
# with debugfs hidden avoids it. Images are produced before this step, so only
# the OTA zip/target-files zip is affected.
# ---------------------------------------------------------------------------
echo
echo "4. OTA packaging (informational)"
if ls /sys/kernel/debug/hid/*/events >/dev/null 2>&1; then
    warn "a HID debug node exists; package the OTA with debugfs hidden:"
    echo "        unshare -m --propagation private bash -c \\"
    echo "          'umount /sys/kernel/debug; source build/envsetup.sh; m otapackage'"
else
    ok "no blocking HID debug node (plain 'm otapackage' is fine)"
fi

echo
if [ "$FAIL" = 0 ]; then
    echo "host-quirks: all good"
else
    echo "host-quirks: problems found (see [FAIL] above)" >&2
fi
exit "$FAIL"
