#!/usr/bin/env python3
"""Exercise both release layouts without compiling Android."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile


SIM = Path(__file__).resolve().parents[1]
MOCK_COMMAND = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
import zipfile

operation, *args = sys.argv[1:]
record = {
    "operation": operation,
    "args": args,
    "ab": os.environ.get("AB_OTA_UPDATER"),
    "branches": os.environ.get("ROOMSERVICE_BRANCHES"),
    "product": os.environ.get("TARGET_PRODUCT"),
    "variant": os.environ.get("TARGET_BUILD_VARIANT"),
    "out_dir": os.environ.get("OUT_DIR"),
}
with open(os.environ["MOCK_LOG"], "a") as log:
    log.write(json.dumps(record) + "\n")

tree = Path(os.environ["MOCK_TREE"])
if operation == "repo":
    if args[0] == "init":
        (tree / ".repo").mkdir(exist_ok=True)
    elif args[0] == "manifest":
        Path(args[args.index("-o") + 1]).write_text("<manifest/>\n")
elif operation == "build":
    if Path(os.environ["OUT_DIR"]).is_absolute():
        raise SystemExit("Soong requires a source-relative OUT_DIR")
    output = Path(os.environ["OUT_DIR"]) / "target/product" / os.environ["TARGET_PRODUCT"]
    output.mkdir(parents=True, exist_ok=True)
    variant = os.environ["TARGET_BUILD_VARIANT"]
    layout = "ab" if os.environ["AB_OTA_UPDATER"] == "true" else "non-ab"
    if layout == "non-ab":
        (output / "recovery.img").write_text(layout + " " + variant + " recovery")
    else:
        (output / "vendor_boot.img").write_text(layout + " " + variant + " vendor_boot")
    if "vm-utm-zip" in args:
        (output / "boot.img").write_text(layout + " " + variant + " boot")
        version = "23.2-test-UNOFFICIAL-" + os.environ["TARGET_PRODUCT"]
        utm = output / "VirtualMachine/UTM" / ("UTM-VM-lineage-" + version + ".zip")
        utm.parent.mkdir(parents=True, exist_ok=True)
        for path in (utm, output / ("lineage_" + os.environ["TARGET_PRODUCT"] + "-ota.zip")):
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("fixture.txt", layout + " " + variant)
        if os.environ.get("MOCK_STALE_ARCHIVE") == "1":
            with zipfile.ZipFile(utm.parent / "UTM-VM-lineage-previous.zip", "w") as archive:
                archive.writestr("fixture.txt", "stale bundle")
'''
ENVSETUP = r'''
breakfast() {
    export TARGET_PRODUCT="$1" TARGET_BUILD_VARIANT="$2"
    python3 "$MOCK_COMMAND" breakfast "$@"
}
m() { python3 "$MOCK_COMMAND" build "$@"; }
get_build_var() {
    case "$1" in
        AB_OTA_UPDATER) printf '%s\n' "${MOCK_AB_OVERRIDE:-$AB_OTA_UPDATER}" ;;
        TARGET_NO_RECOVERY) printf '%s\n' "${MOCK_NO_RECOVERY:-$AB_OTA_UPDATER}" ;;
        BOARD_INCLUDE_RECOVERY_RAMDISK_IN_VENDOR_BOOT) printf '%s\n' "$AB_OTA_UPDATER" ;;
        BOARD_RECOVERYIMAGE_PARTITION_SIZE)
            if [ "$AB_OTA_UPDATER" = false ]; then
                printf '%s\n' "${MOCK_RECOVERY_SIZE:-67108864}"
            fi ;;
        PRODUCT_OUT) printf '%s\n' "$OUT_DIR/target/product/$TARGET_PRODUCT" ;;
        LINEAGE_VERSION) printf '23.2-test-UNOFFICIAL-%s\n' "$TARGET_PRODUCT" ;;
        *) return 1 ;;
    esac
}
'''


class BuildReleaseTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.sim = self.root / "sim"
        self.sim.mkdir()
        shutil.copy(SIM / "build.sh", self.sim / "build.sh")
        shutil.copy(SIM / "lineage-virtio.xml", self.sim / "lineage-virtio.xml")
        for name in ("apply.sh", "host-quirks.sh", "run-host-tests.sh"):
            (self.sim / name).write_text("#!/bin/bash\nexit 0\n")
        self.tree = self.root / "android"
        (self.tree / "build").mkdir(parents=True)
        (self.tree / "build/envsetup.sh").write_text(ENVSETUP)
        self.command = self.root / "mock.py"
        self.command.write_text(MOCK_COMMAND)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("repo", "git-lfs", "qemu-img"):
            executable = self.bin / name
            executable.write_text(
                f'#!/bin/bash\nexec python3 "$MOCK_COMMAND" {name} "$@"\n'
            )
            executable.chmod(0o755)
        self.log = self.root / "commands.jsonl"

    def build(self, product="virtio_arm64only", layout="both", **overrides):
        environment = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "MOCK_TREE": str(self.tree),
            "MOCK_COMMAND": str(self.command),
            "MOCK_LOG": str(self.log),
            "BUILD_JOBS": "2",
            "OUT_DIR": "out",
            "SKIP_SYNC": "0",
            "AB_OTA_UPDATER": "true",
            **overrides,
        }
        return subprocess.run(
            ["bash", str(self.sim / "build.sh"), str(self.tree), product, layout],
            env=environment,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_default_builds_both_layouts_in_separate_output_directories(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.commands()
        variants = [item for item in commands if item["operation"] == "breakfast"]
        self.assertEqual([item["variant"] for item in variants], ["userdebug", "user", "user"])
        self.assertEqual([item["ab"] for item in variants], ["false", "false", "true"])
        self.assertEqual(variants[0]["out_dir"], variants[1]["out_dir"])
        self.assertNotEqual(variants[1]["out_dir"], variants[2]["out_dir"])
        self.assertEqual([item["out_dir"] for item in variants], ["out/non-ab", "out/non-ab", "out/ab"])
        self.assertTrue(
            all(item["branches"] == "lineage-23.1 lineage-23.0" for item in variants)
        )
        builds = [item["args"] for item in commands if item["operation"] == "build"]
        self.assertEqual(builds, [
            ["-j2", "recoveryimage"],
            ["-j2", "vm-utm-zip", "otapackage"],
            ["-j2", "vm-utm-zip", "otapackage"],
        ])
        non_ab = self.check_release("virtio_arm64only", "non-ab", result.stdout)
        ab = self.check_release("virtio_arm64only", "ab", result.stdout)
        self.assertTrue(set(non_ab["artifacts"]).isdisjoint(ab["artifacts"]))

    def check_release(self, product, layout, stdout, output_root=None):
        architecture = product.removeprefix("virtio_")
        output = (output_root or self.tree / "out") / "releases" / product / layout
        images = {f"boot_{architecture}-{layout}.img": f"{layout} user boot"}
        if layout == "non-ab":
            images.update({
                f"recovery_{architecture}-{layout}.img": f"{layout} user recovery",
                f"recovery_{architecture}-{layout}-userdebug.img": f"{layout} userdebug recovery",
            })
        else:
            images[f"vendor_boot_{architecture}-{layout}.img"] = f"{layout} user vendor_boot"
        for name, contents in images.items():
            self.assertEqual((output / name).read_text(), contents)
            self.assertIn(name, stdout)
        metadata = json.loads((output / "release.json").read_text())
        self.assertEqual(metadata["product"], product)
        self.assertEqual(metadata["partition_layout"], layout)
        self.assertEqual(metadata["ab_ota_updater"], layout == "ab")
        self.assertEqual(metadata["recovery_partition"], layout == "non-ab")
        self.assertEqual(metadata["recovery_location"], "vendor_boot" if layout == "ab" else "recovery")
        self.assertTrue(set(images).issubset(metadata["artifacts"]))
        for name in metadata["artifacts"]:
            if name.endswith(".zip"):
                with zipfile.ZipFile(output / name) as archive:
                    self.assertEqual(archive.read("fixture.txt").decode(), f"{layout} user")
        for entry in (output / "SHA256SUMS").read_text().splitlines():
            digest, name = entry.split("  ", 1)
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
        self.assertTrue((output / f"lutm-source-manifest-{layout}.xml").is_file())
        return metadata

    def test_x86_64_release_uses_matching_image_names(self):
        result = self.build("virtio_x86_64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_release("virtio_x86_64", "non-ab", result.stdout)
        self.check_release("virtio_x86_64", "ab", result.stdout)

    def test_single_non_ab_build_keeps_upstream_recovery_variants(self):
        result = self.build(layout="non-ab")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_release("virtio_arm64only", "non-ab", result.stdout)
        self.assertFalse((self.tree / "out/ab").exists())

    def test_single_ab_build_ships_vendor_boot_not_standalone_recovery(self):
        result = self.build(layout="ab")
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata = self.check_release("virtio_arm64only", "ab", result.stdout)
        self.assertFalse(any(name.startswith("recovery_") for name in metadata["artifacts"]))
        self.assertFalse((self.tree / "out/non-ab").exists())
        builds = [item["args"] for item in self.commands() if item["operation"] == "build"]
        self.assertEqual(builds, [["-j2", "vm-utm-zip", "otapackage"]])

    def test_ab_board_override_fails_before_compilation(self):
        result = self.build(layout="non-ab", MOCK_AB_OVERRIDE="true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the requested non-ab layout", result.stderr)
        self.assertFalse(any(item["operation"] == "build" for item in self.commands()))

    def test_absent_recovery_partition_fails_before_compilation(self):
        result = self.build(layout="non-ab", MOCK_RECOVERY_SIZE="0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected the non-A/B layout", result.stderr)
        self.assertFalse(any(item["operation"] == "build" for item in self.commands()))

    def test_ab_layout_requires_recovery_in_vendor_boot(self):
        result = self.build(layout="ab", MOCK_NO_RECOVERY="false")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected the A/B layout with recovery in vendor_boot", result.stderr)
        self.assertFalse(any(item["operation"] == "build" for item in self.commands()))

    def test_stale_bundle_does_not_replace_the_current_release(self):
        result = self.build(MOCK_STALE_ARCHIVE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        for layout in ("non-ab", "ab"):
            metadata = self.check_release("virtio_arm64only", layout, result.stdout)
            self.assertFalse(any("previous" in name for name in metadata["artifacts"]))

    def test_absolute_output_root_inside_tree_becomes_source_relative(self):
        output = self.tree / "custom-output"
        result = self.build(layout="ab", OUT_DIR=str(output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_release("virtio_arm64only", "ab", result.stdout, output)
        variants = [item for item in self.commands() if item["operation"] == "breakfast"]
        self.assertEqual([item["out_dir"] for item in variants], ["custom-output/ab"])

    def test_output_root_outside_tree_fails_before_compilation(self):
        result = self.build(OUT_DIR=str(self.root / "outside"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OUT_DIR must be inside the Android source tree", result.stderr)
        self.assertFalse(any(item["operation"] == "build" for item in self.commands()))


if __name__ == "__main__":
    unittest.main()
