# SIM / telephony emulation: board config
#
# Included from device/virt/virtio-common/BoardConfigCommon.mk by sim/apply.sh.

ifneq ($(TARGET_NO_TELEPHONY),true)

# The guest RIL reads the modem simulator port from ro.boot.modem_simulator_ports.
# modem_simulator.rc binds the same port (9200).
BOARD_BOOTCONFIG += androidboot.modem_simulator_ports=9200

# SELinux: Cuttlefish's telephony policy (RIL domain, vsock, data) plus our
# modem simulator policy.
BOARD_VENDOR_SEPOLICY_DIRS += \
    device/google/cuttlefish/shared/sepolicy/vendor/telephony \
    device/virt/virtio-common/sepolicy/vendor-sim

# Emulation properties.
TARGET_VENDOR_PROP += device/virt/virtio-common/configs/properties/vendor.sim.prop

endif
