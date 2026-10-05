#!/usr/bin/env python3
"""Exercise the upstream-compatible build sequence without compiling Android."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


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
    output = tree / "out/target/product" / os.environ["TARGET_PRODUCT"]
    output.mkdir(parents=True, exist_ok=True)
    variant = os.environ["TARGET_BUILD_VARIANT"]
    (output / "recovery.img").write_text(variant + " recovery")
    if "vm-utm-zip" in args:
        (output / "boot.img").write_text(variant + " boot")
        utm = output / "VirtualMachine/UTM/UTM-VM-test.zip"
        utm.parent.mkdir(parents=True, exist_ok=True)
        for path in (utm, output / (os.environ["TARGET_PRODUCT"] + "-ota.zip")):
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("fixture.txt", "archive integrity fixture")
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
        TARGET_NO_RECOVERY) printf '%s\n' "${MOCK_NO_RECOVERY:-false}" ;;
        BOARD_RECOVERYIMAGE_PARTITION_SIZE) printf '%s\n' "${MOCK_RECOVERY_SIZE:-67108864}" ;;
        PRODUCT_OUT) printf '%s\n' "$MOCK_TREE/out/target/product/$TARGET_PRODUCT" ;;
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

    def build(self, product="virtio_arm64only", **overrides):
        environment = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "MOCK_TREE": str(self.tree),
            "MOCK_COMMAND": str(self.command),
            "MOCK_LOG": str(self.log),
            "BUILD_JOBS": "2",
            "SKIP_SYNC": "0",
            "AB_OTA_UPDATER": "true",
            **overrides,
        }
        return subprocess.run(
            ["bash", str(self.sim / "build.sh"), str(self.tree), product],
            env=environment,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_arm64_release_matches_non_ab_and_recovery_variants(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.commands()
        variants = [item for item in commands if item["operation"] == "breakfast"]
        self.assertEqual([item["variant"] for item in variants], ["userdebug", "user"])
        self.assertTrue(all(item["ab"] == "false" for item in variants))
        self.assertTrue(
            all(item["branches"] == "lineage-23.1 lineage-23.0" for item in variants)
        )
        builds = [item["args"] for item in commands if item["operation"] == "build"]
        self.assertEqual(builds, [["-j2", "recoveryimage"], ["-j2", "vm-utm-zip", "otapackage"]])
        self.check_images("virtio_arm64only", "arm64only", result.stdout)

    def check_images(self, product, architecture, stdout):
        output = self.tree / "out/target/product" / product
        for name, contents in (
            (f"boot_{architecture}.img", "user boot"),
            (f"recovery_{architecture}.img", "user recovery"),
            (f"recovery_{architecture}-userdebug.img", "userdebug recovery"),
        ):
            self.assertEqual((output / name).read_text(), contents)
            self.assertIn(name, stdout)

    def test_x86_64_release_uses_matching_image_names(self):
        result = self.build("virtio_x86_64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_images("virtio_x86_64", "x86_64", result.stdout)

    def test_ab_board_override_fails_before_compilation(self):
        result = self.build(MOCK_AB_OVERRIDE="true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected the upstream non-A/B layout", result.stderr)
        self.assertFalse(any(item["operation"] == "build" for item in self.commands()))

    def test_absent_recovery_partition_fails_before_compilation(self):
        result = self.build(MOCK_RECOVERY_SIZE="0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected the upstream non-A/B layout", result.stderr)
        self.assertFalse(any(item["operation"] == "build" for item in self.commands()))


if __name__ == "__main__":
    unittest.main()
