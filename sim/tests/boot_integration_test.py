#!/usr/bin/env python3
"""Regression tests for the overlay's product, init and application wiring."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SIM = Path(__file__).resolve().parents[1]
OVERLAY = SIM / "overlay"


def build_config(no_telephony="false"):
    with tempfile.TemporaryDirectory() as directory:
        makefile = Path(directory) / "config.mk"
        makefile.write_text(
            f"TARGET_NO_TELEPHONY := {no_telephony}\n"
            "TARGET_COPY_OUT_VENDOR := vendor\n"
            "SRC_TARGET_DIR := build/target\n"
            "inherit-product =\n"
            "include virtio-sim.mk\n"
            "include virtio-sim-board.mk\n"
            ".PHONY: config\n"
            "config:\n"
            "\t@printf '%s\\n' 'packages=$(strip $(PRODUCT_PACKAGES))' "
            "'copies=$(strip $(PRODUCT_COPY_FILES))' "
            "'bootconfig=$(strip $(BOARD_BOOTCONFIG))' "
            "'properties=$(strip $(TARGET_VENDOR_PROP))' "
            "'policy=$(strip $(BOARD_VENDOR_SEPOLICY_DIRS))'\n"
        )
        output = subprocess.check_output(
            ["make", "--no-print-directory", "-f", str(makefile), "config"],
            cwd=OVERLAY,
            text=True,
        )
        return dict(line.split("=", 1) for line in output.splitlines())


class BootWiringTest(unittest.TestCase):
    def test_enabled_stack_is_packaged(self):
        config = build_config()
        self.assertTrue(
            {
                "com.google.cf.rild",
                "modem_simulator_virtio",
                "modem_console",
                "iccprofile_for_sim0.xml",
                "numeric_operator.xml",
            }.issubset(config["packages"].split())
        )
        self.assertIn("androidboot.modem_simulator_ports=9200", config["bootconfig"])

    def test_sim_init_actions_are_auto_imported(self):
        copies = dict(item.split(":", 1) for item in build_config()["copies"].split())
        source = "device/virt/virtio-common/configs/init/init.virtio.sim.rc"
        self.assertEqual(
            Path(copies[source]).parent,
            Path("vendor/etc/init"),
            "Android init does not recursively import etc/init/hw/",
        )

    def test_opt_out_does_not_advertise_an_absent_modem(self):
        for key, value in build_config("true").items():
            self.assertEqual(value, "", key)

    def test_monitor_starts_after_service_registration(self):
        source = SIM / "reference/modem_simulator/host/commands/modem_simulator"
        monitor = (source / "channel_monitor.cpp").read_text()
        constructor, start = monitor.split("ChannelMonitor::ChannelMonitor(", 1)[
            1
        ].split("void ChannelMonitor::Start()", 1)
        self.assertNotIn("monitor_thread_ = std::thread", constructor)
        self.assertIn("monitor_thread_ = std::thread", start)
        simulator = (source / "modem_simulator.cpp").read_text()
        initialize = simulator.split("void ModemSimulator::Initialize(", 1)[
            1
        ].split("void ModemSimulator::RegisterModemService()", 1)[0]
        self.assertLess(
            initialize.index("RegisterModemService();"),
            initialize.index("channel_monitor_->Start();"),
        )

    def test_guest_startup_errors_use_android_logging(self):
        main = (OVERLAY / "modem_simulator/main_virtio.cpp").read_text()
        self.assertIn("android::base::InitLogging(argv);", main)
        self.assertNotIn("android::base::StderrLogger", main)


class ApplyTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tree = Path(self.directory.name)
        self.device = self.tree / "device/virt/virtio-common"
        self.device.mkdir(parents=True)
        for name in ("device-common.mk", "BoardConfigCommon.mk"):
            (self.device / name).write_text("# fixture\n")
        cuttlefish = self.tree / "device/google/cuttlefish"
        shutil.copytree(
            SIM / "reference/modem_simulator/host/commands/modem_simulator",
            cuttlefish / "host/commands/modem_simulator",
        )
        self.ril = cuttlefish / "guest/hals/ril/reference-ril/reference-ril.c"
        self.ril.parent.mkdir(parents=True)
        self.ril.write_text("sa.svm_cid = VMADDR_CID_HOST;\n")

    def apply(self):
        return subprocess.run(
            ["bash", str(SIM / "apply.sh"), str(self.tree)],
            capture_output=True,
            text=True,
        )

    def snapshot(self):
        return {
            str(path.relative_to(self.tree)): path.read_bytes()
            for path in self.tree.rglob("*")
            if path.is_file()
        }

    def test_reapplying_preserves_the_same_overlay(self):
        first = self.apply()
        self.assertEqual(first.returncode, 0, first.stderr)
        snapshot = self.snapshot()
        second = self.apply()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.snapshot(), snapshot)
        self.assertIn("VMADDR_CID_LOCAL", self.ril.read_text())
        self.assertNotIn("VMADDR_CID_HOST", self.ril.read_text())
        self.assertEqual(
            (self.device / "device-common.mk").read_text().count(
                "$(call inherit-product, device/virt/virtio-common/virtio-sim.mk)"
            ),
            1,
        )

    def test_missing_ril_fails_before_installing_the_overlay(self):
        self.ril.unlink()
        snapshot = self.snapshot()
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required source not found", result.stderr)
        self.assertEqual(self.snapshot(), snapshot)

    def test_unknown_ril_transport_fails_before_installing_the_overlay(self):
        self.ril.write_text("unsupported transport\n")
        snapshot = self.snapshot()
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported guest RIL transport", result.stderr)
        self.assertEqual(self.snapshot(), snapshot)


if __name__ == "__main__":
    unittest.main()
