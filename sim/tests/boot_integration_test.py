#!/usr/bin/env python3
"""Regression tests for the overlay's product, init and application wiring."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


SIM = Path(__file__).resolve().parents[1]
OVERLAY = SIM / "overlay"


def build_config(no_telephony="false", package_overlays=""):
    with tempfile.TemporaryDirectory() as directory:
        makefile = Path(directory) / "config.mk"
        makefile.write_text(
            f"TARGET_NO_TELEPHONY := {no_telephony}\n"
            f"PRODUCT_PACKAGE_OVERLAYS := {package_overlays}\n"
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
            "'overlays=$(strip $(PRODUCT_PACKAGE_OVERLAYS))' "
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

    def test_one_sim_overrides_the_wifi_only_framework_defaults(self):
        inherited = "vendor/lineage/overlay/wifionly"
        config = build_config(package_overlays=inherited)
        self.assertEqual(
            config["overlays"].split(),
            ["device/virt/virtio-common/framework-overlay", inherited],
        )
        resources = ET.parse(
            OVERLAY / "framework-overlay/frameworks/base/core/res/res/values/config.xml"
        ).getroot()
        values = {item.get("name"): item.text for item in resources}
        self.assertEqual(values["config_num_physical_slots"], "1")
        for capability in ("voice", "sms", "mobile_data"):
            self.assertEqual(values[f"config_{capability}_capable"], "true")
        self.assertEqual(build_config("true", inherited)["overlays"], inherited)

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

    def test_modem_can_listen_and_accept_ril_connections(self):
        policy = (OVERLAY / "sepolicy/vendor-sim/modem_simulator.te").read_text()
        self.assertIn(
            "allow modem_simulator self:vsock_socket { listen accept };", policy
        )

    def test_vendor_init_uses_a_vendor_service_state_trigger(self):
        service = (OVERLAY / "modem_simulator/modem_simulator.rc").read_text()
        actions = (OVERLAY / "configs/init/init.virtio.sim.rc").read_text()
        self.assertIn("service vendor.modem-simulator ", service)
        self.assertIn("on property:init.svc.vendor.modem-simulator=running", actions)
        self.assertNotIn("on property:init.svc.modem-simulator=running", actions)

    def test_writable_modem_state_stays_in_vendor_data(self):
        directory = "/data/vendor/modem_simulator"
        actions = (OVERLAY / "configs/init/init.virtio.sim.rc").read_text()
        config = (OVERLAY / "modem_simulator/cf_device_config_virtio.cpp").read_text()
        labels = (OVERLAY / "sepolicy/vendor-sim/file_contexts").read_text()
        types = (OVERLAY / "sepolicy/vendor-sim/file.te").read_text()
        policy = (OVERLAY / "sepolicy/vendor-sim/modem_simulator.te").read_text()
        self.assertIn(f"mkdir {directory} 0770 radio radio", actions)
        self.assertIn(f'kModemDataDir[] = "{directory}/"', config)
        self.assertIn(f"{directory}(/.*)?", labels)
        self.assertIn(
            "type modem_simulator_data_file, file_type, data_file_type;",
            types,
        )
        self.assertNotIn("vendor_data_file_type", types)
        self.assertNotIn("core_data_file_type", types)
        self.assertIn("allow modem_simulator vendor_data_file:dir search;", policy)


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
        self.kernel_config = self.tree / "vendor/lineage/config/BoardConfigKernel.mk"
        self.kernel_config.parent.mkdir(parents=True)
        self.kernel_config.write_text(
            "OUT_DIR_PREFIX := $(OUT_DIR)\n"
            "KERNEL_BUILD_OUT_PREFIX :=\n"
            "ifeq ($(OUT_DIR_PREFIX),out)\n"
            "    KERNEL_BUILD_OUT_PREFIX := $(BUILD_TOP)/\n"
            "endif\n"
        )

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

    def test_kernel_output_prefix_handles_relative_layout_directories(self):
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stderr)
        for directory, expected in (
            ("out/non-ab", str(self.tree) + "/"),
            ("out/ab", str(self.tree) + "/"),
            ("/tmp/absolute-output", ""),
        ):
            with self.subTest(output=directory):
                output = subprocess.check_output(
                    ["make", "--no-print-directory", "-f", str(self.kernel_config),
                     f"OUT_DIR={directory}", f"BUILD_TOP={self.tree}",
                     "--eval", "print:;@printf '%s' '$(KERNEL_BUILD_OUT_PREFIX)'", "print"],
                    text=True,
                )
                self.assertEqual(output, expected)


if __name__ == "__main__":
    unittest.main()
