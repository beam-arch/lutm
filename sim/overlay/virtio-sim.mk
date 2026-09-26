# SIM / telephony emulation for virtio VMs
#
# Guest-side, self-contained emulated physical SIM. Reuses AOSP's virtual-device
# radio stack (Cuttlefish guest RIL + modem simulator) which is already synced in
# this build tree.
#
# Opt out with TARGET_NO_TELEPHONY=true.

ifneq ($(TARGET_NO_TELEPHONY),true)

# Soong namespace for the Cuttlefish guest RIL libraries we build against.
PRODUCT_SOONG_NAMESPACES += device/google/cuttlefish

# system_ext telephony support modules (CarrierConfig, EmergencyInfo, ...).
# telephony_system_ext.mk uses PRODUCT_PACKAGES +=, so inheriting it late is safe
# (unlike telephony_vendor.mk, which assigns PRODUCT_PACKAGES with :=).
$(call inherit-product, $(SRC_TARGET_DIR)/product/telephony_system_ext.mk)

# Guest radio HAL + reference-ril. The APEX overrides platform
# rild/libril/libreference-ril and provides vendor.ril-daemon, and it ships the
# android.hardware.telephony.* feature declarations.
PRODUCT_PACKAGES += com.google.cf.rild

# In-guest modem / UICC simulator.
PRODUCT_PACKAGES += modem_simulator_virtio

# Control plane for the emulated SIM, SMS and network. Attaches to the simulator
# over a local socket; see sim/README.md for how to drive it from adb.
PRODUCT_PACKAGES += modem_console

# Seed the simulator with a SIM profile and operator database. These are the
# Cuttlefish prebuilts, installed to /vendor/etc/modem_simulator/files/; init
# copies them into /data/misc/modem_simulator on boot.
PRODUCT_PACKAGES += \
    iccprofile_for_sim0.xml \
    iccprofile_for_sim0_for_CtsCarrierApiTestCases.xml \
    numeric_operator.xml

# Init actions: seed /data and start ordering for vendor.ril-daemon.
PRODUCT_COPY_FILES += \
    device/virt/virtio-common/configs/init/init.virtio.sim.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.virtio.sim.rc
endif
