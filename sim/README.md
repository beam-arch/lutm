# SIM / telephony emulation for LineageOS-on-QEMU (virtio)

This directory adds a **guest-side, self-contained** emulated physical SIM to the
`virtio_x86_64` / `virtio_arm64only` builds so that Android believes a real
physical SIM is inserted and brings the full telephony stack up
(SIM + carrier, voice, SMS, data; IMS/RCS surfaces exist because the HAL
implements the IMS interfaces).

Nothing is required from the host/QEMU command line and nothing is required from
UTM: the modem lives entirely inside the guest image.

## Why this shape

Android needs a `android.hardware.radio` HAL that reports a present UICC. The
device tree used here (`device/virt/virtio-common`) ships **no radio HAL at all**,
which is why the framework currently reports "no SIM".

Rather than reimplement the radio HAL, this uses the pieces AOSP already
maintains for its virtual devices, which are already synced as part of this build
tree (`device/virt/virt-common` already consumes `device/google/cuttlefish`):

| Piece | Source (in-tree) | Role |
|---|---|---|
| Guest radio HAL + `reference-ril` | `device/google/cuttlefish/guest/hals/ril/{reference-libril,reference-ril}` | AIDL `android.hardware.radio.*` services; run through the vendored RIL |
| `com.google.cf.rild` APEX | `device/google/cuttlefish/apex/com.google.cf.rild` | Packages the RIL and overrides `rild`/`libril`/`libreference-ril`; provides `vendor.ril-daemon` |
| Modem/UICC simulator | `device/google/cuttlefish/host/commands/modem_simulator` | Speaks the AT protocol + emulates the SIM, network, calls, SMS, data |

In stock Cuttlefish the modem simulator runs on the **host** and the guest RIL
reaches it over **virtio-vsock** (`androidboot.modem_simulator_ports=<port>`).
For a self-contained VM we run the same simulator **inside the guest** and use
vsock **loopback** (`VMADDR_CID_LOCAL`) instead.

```
 guest
 ┌───────────────────────────────────────────────────────────────┐
 │ vendor.ril-daemon (com.google.cf.rild)                        │
 │   libril-modem-lib  ── AIDL radio HAL (IRadioSim/Network/…)   │
 │   reference-ril     ── AT over vsock ──┐                      │
 │                                        │ AF_VSOCK cid=LOCAL   │
 │ modem_simulator  ── vsock server ──────┘                      │
 │   (SIM/network/call/SMS/data emulation)                       │
 └───────────────────────────────────────────────────────────────┘
```

## What `apply.sh` does

Run automatically by `build.sh` right after `repo sync`, before `breakfast`:

1. Copies this overlay into `device/virt/virtio-common`:
   - `virtio-sim.mk` / `virtio-sim-board.mk` — product + board config
   - `configs/init/init.virtio.sim.rc` — seeds `/data/misc/modem_simulator`
   - `configs/properties/vendor.sim.prop` — telephony properties
   - `sepolicy/vendor-sim/*` — SELinux for the simulator, plus the
     `ro.boot.modem_simulator_ports` property type the Cuttlefish RIL policy
     expects (its own copy lives in a Cuttlefish sepolicy dir the virtio board
     config does not include)
   - `modem_simulator/*` — device build of the modem simulator, plus the
     control-plane socket in `main_virtio.cpp`
   - `modem_console/*` — `modem_console`, the tool that drives the emulated
     network (see below)
2. Hooks `virtio-sim.mk` into `device-common.mk` and `virtio-sim-board.mk` into
   `BoardConfigCommon.mk`.
3. Stages the Cuttlefish simulator sources into `modem_simulator/src/`, minus the
   host-only `main.cpp` / `cf_device_config.cpp`. Soong resolves `srcs` relative
   to the `.bp`'s own directory, so they cannot be referenced in place.
4. Patches that staged copy with `sim/patch_modem_simulator.py` (18 exact-match
   edits; re-applied from pristine sources on every run):
   - a control-plane broadcast, so a console sees the SMS the device sends and
     the delivery reports that come back;
   - SMS-DELIVER parsing in `PDUParser`, which upstream never needed because a
     Cuttlefish device only ever sends to itself. Without it a genuine
     network-delivered PDU fails validation, and the message can only be
     delivered re-encoded (`CreatePDU()`), which drops the service centre
     address and rewrites the time stamp;
   - the subscriber number API: `AT+CNUM`, `AT+REMOTEQUERYPHONENUMBER`, and a
     workable `AT+REMOTEUPADATEPHONENUMBER` (`GetNextStr(' ')` on the whole
     command string, with no `=` stripped, meant upstream's remote update could
     never have worked);
   - `AT+REMOTEOPERATOR`, to move the device to another emulated operator.
5. Rewrites the guest RIL's vsock target from `VMADDR_CID_HOST` to
   `VMADDR_CID_LOCAL` so the RIL talks to the in-guest simulator.

## Building the images

Apply the overlay first (`sim/apply.sh`), then build the same targets `build.sh`
uses:

```shell
breakfast virtio_x86_64 user          # or virtio_arm64only
m vm-utm-zip otapackage
```

**`m otapackage` on its own is not enough.** It produces the flashable OTA zip and
the partition images, but *not* the UTM bundle: that is a separate phony target
(`vm-utm` / `vm-utm-zip`) defined in
`device/virt/virtio-common/build/tasks/90-vm-utm.mk`. `vm-utm-zip` needs
`disk-vda.img`, which assembles the partition images into a bootable disk with an
`lpmake`-generated `super` partition, and then converts it with `qemu-img`
(install `qemu-utils`, otherwise the rule falls back to hard-linking the raw
image):

```
out/target/product/<product>/VirtualMachine/UTM/UTM-VM-<version>.zip
out/target/product/<product>/VirtualMachine/UTM/<name>.utm/config.plist
out/target/product/<product>/VirtualMachine/UTM/<name>.utm/Data/{vda.qcow2,vdb.qcow2,efi_vars.fd}
```

`vda.qcow2` is the bootable disk (5 GiB virtual); its `super` partition holds
system/vendor/product/system_ext/odm, so it is also where the emulated SIM ends
up. `vdb.qcow2` is the 16 GiB empty userdata disk.

Run `sim/host-quirks.sh --fix` before the first build — see the environment
section below.

## Boot flow

* `androidboot.modem_simulator_ports=9200` is added to `BOARD_BOOTCONFIG`, so the
  guest RIL reads `ro.boot.modem_simulator_ports=9200`.
* `modem_simulator` starts in init class `core` and binds a vsock server on port
  9200; `init.virtio.sim.rc` also starts `vendor.ril-daemon` once the simulator
  reports `running`.
* `vendor.ril-daemon` (via `libcuttlefish-ril-2.so`) connects to the simulator
  over vsock loopback and registers the AIDL radio HAL. If it loses the race it
  retries every 10s.
* Android sees a SIM, a registered network, and a mobile data connection.

## Verifying at runtime

```shell
adb shell getprop ro.boot.modem_simulator_ports        # 9200
adb shell getprop init.svc.modem-simulator             # running
adb shell getprop init.svc.vendor.ril-daemon           # running
adb shell dumpsys telephony.registry | head
adb shell service list | grep android.hardware.radio
adb shell getprop gsm.operator.alpha                   # non-empty
```

## Driving the emulated network: `modem_console`

On stock Cuttlefish the simulator is driven over the **host** control plane: the
launcher connects to a monitor socket, sends `REM0` to be registered as a
"remote" client and then writes `AT+REMOTE*` commands. On a standalone device
there is no host, so `main_virtio.cpp` serves that same role itself: it listens
on an **abstract local socket** (`modem_simulator_console`) and registers every
connection as a remote client. `modem_console` is the client.

```
adb shell modem_console
```

```
identity                     MSISDN, IMSI, ICCID, IMEI, operator
status                       identity + registration, signal, RAT
number                       show the SIM's MSISDN (EF_MSISDN)
number set +15559876543      rewrite it, the way OMA-DM would
sms recv --from +15551234567 --text "Your code is 123456"
sms recv --alnum --from AndroidBank --text "Balance: 42.00" --age 30
sms recv --from +1555... --text "part 2" --concat 4A,2,2
sms recv --from +1555... --port 2948 --8bit --data-hex 0206DEADBEEF
tower deregister             tower disappears ("no service")
tower register home          back on the home network
tower signal 15              drop to one bar
tower tech 5g                change the radio access technology
tower operator Alternative   move to the second emulated operator
tower power off              radio off
sms watch                    live in/out messages + registration changes
raw AT+CSQ                   anything else, verbatim
```

`--json` makes any of it scriptable; `sms build` prints a PDU without a device
and `sms decode <hex>` decodes one, which is handy when checking a capture.

What "looks real" means here, concretely — every injected message is a genuine
transport PDU, not a re-encoded one:

* **GSM 03.38 default alphabet** with the escape table (`{}[]|^~\\€` cost two
  septets and must be packed that way); automatic fallback to **UCS2** for text
  the alphabet cannot carry, exactly as a handset decides.
* **SMSC address** always present, semi-octet BCD with TON/NPI, resolved from the
  SIM's own `AT+CSCA?` unless told otherwise.
* **Service centre time stamp** with the local time zone and its sign (`--age 30`
  backdates it, `--scts <epoch>` sets it). This is what the framework shows as
  the message time, which is why the PDU is delivered byte for byte.
* **UDH**: concatenation (8- and 16-bit references) and application ports, so
  multipart SMS and binary WAP-push/OTA payloads arrive as such.
* **Alphanumeric senders**, message classes, `--pid`, explicit `--dcs`.
* **Delivery reports**: ask for one with the device (`--sri` on the submit is the
  device's side) and the simulator sends the `+CDS` status report itself; the
  console reports it in `sms watch`.

Transport notes: the socket is abstract, so no `/dev/socket` entry or file label
is involved. `shell` (and `su`) may attach; the platform neverallow that forbids
coredomain→vendor socket communication is waived for this one pair via
`socket_between_core_and_vendor_violators`, with the `allow` rules still limiting
access to a shell. If you would rather not run the console on the device at all,
the same protocol is reachable through adbd without any exemption:

```shell
adb forward tcp:5000 localabstract:modem_simulator_console
# then write "REM0\r" followed by AT+REMOTE* commands to localhost:5000
```

## Which half of the RIL talks to the simulator

The APEX binary `libcuttlefish-rild` is only the shim: it reads
`ro.boot.modem_simulator_ports` and, if empty, deliberately comes up as
\"no-ril\". The AT/vsock transport lives in `libcuttlefish-ril-2.so`
(`guest/hals/ril/reference-ril/reference-ril.c`). Its SELinux domain is
`libcuttlefish_rild` (not `rild`), and that domain may only use vsock because
Cuttlefish adds it to `unconstrained_vsock_violators`; the simulator needs the
same exemption, which is why `modem_simulator.te` mirrors it under
`starting_at_board_api(202504, ...)`.

## Verification status

Verified by real builds against a synced LineageOS 23.2 tree:

| Check | Target | Result |
|---|---|---|
| Simulator compiles and links for device | `m modem_simulator_virtio` | pass (installs `/vendor/bin/hw/modem_simulator_virtio`) |
| Console compiles and links for device | `m modem_console` | pass (installs `/system/bin/modem_console`, no shared library dependencies) |
| PDU codec behaviour | `sim/run-host-tests.sh` | pass (60 checks) |
| RIL APEX builds/signs | `m com.google.cf.rild` | pass (incl. `apex_sepolicy_tests`) |
| SIM seed prebuilts resolve | `m iccprofile_for_sim0.xml numeric_operator.xml …` | pass |
| Vendor policy compiles | `m vendor_sepolicy.cil` | pass (control-plane socket rules included) |
| Platform neverallows (vsock, core/vendor sockets) | `m sepolicy_neverallows` | pass |
| Product/board wiring | `get_build_var PRODUCT_PACKAGES`, `…_COPY_FILES`, `BOARD_BOOTCONFIG`, `TARGET_VENDOR_PROP` | pass |
| **Full user build + OTA package** | `m otapackage` | pass (`lineage_virtio_x86_64-ota.zip`, signed, `unzip -t` clean) |
| **UTM VM bundle (x86_64)** | `m vm-utm-zip` | pass (`UTM-VM-lineage-23.2-…-virtio_x86_64.zip`, 4 files in the `.utm` bundle) |
| **UTM VM bundle (arm64)** | `m vm-utm-zip` on `virtio_arm64only` | pass (`UTM-VM-…-virtio_arm64only.zip` → `LineageOS_on_arm64.utm`, `Architecture=aarch64`, ESP is `BOOTAA64.EFI` + `arm64-efi` GRUB only) |
| **Arm64 build with the control plane** | `m vm-utm-zip otapackage` on `virtio_arm64only` | pass (`UTM-VM-lineage-23.2-20260924-…`, 1139 MB; OTA 1032 MB, `unzip -t` clean) |

The arm64 build dated **20260924** is the one that carries `modem_console`; the
`modem_console` row in the image table below was re-checked against it. The
patched simulator *sources* that `apply.sh` generates (upstream plus the
`patch_modem_simulator.py` changes) are shipped under `sim/reference/` in the
source archive, so the exact code that got compiled is auditable even without
re-running the patcher.
| Raw dynamic-partition disk the UTM bundles boot | `diskimage-vda` | pass (`disk-vda.img`, `super.img`), both products |

The two products are independent and each has its own `vm_templates/utm/config.plist`
(`virtio_x86_64` → `x86_64`/`q35`, `virtio_arm64only` → `aarch64`/`virt`). Both
inherit `device/virt/virtio-common` — where this overlay hooks in — so the SIM
config applies to both without duplication:

```
virtio_arm64only → virtio_arm64 → virtio-common   (overlay + hooks)
virtio_x86_64    →                virtio-common
```

Checked inside the produced images (not just the staging dirs) — the same six
entries below hold for **both** `virtio_x86_64` and `virtio_arm64only`, verified
per product by unpacking the generated `super.img`:

| In the image | Where |
|---|---|
| `modem_simulator_virtio` (373 KB) | `vendor.img:/bin/hw/` |
| `modem_console` (151 KB) | `system.img:/bin/` |
| `init.virtio.sim.rc` | `vendor.img:/etc/init/hw/` |
| SIM profile + operator DB | `vendor.img:/etc/modem_simulator/files/{iccprofile_for_sim0.xml, iccprofile_for_sim0_for_CtsCarrierApiTestCases.xml, numeric_operator.xml}` |
| `ro.radio.noril=0`, `ro.telephony.default_network=33` | `vendor.img:/build.prop` |
| `modem_simulator`, `modem_simulator_exec`, `modem_simulator_data_file`, `libcuttlefish_rild*` | `vendor.img:/etc/selinux/vendor_sepolicy.cil` |
| `com.google.cf.rild.apex` | `vendor.img:/apex/` |
| all of the above | the **bootable** disk: `super.img` → `lpunpack -p vendor` → `vendor.img` (i.e. `diskvda`'s `super` partition) |

### What the host tests actually prove

`sim/run-host-tests.sh` is not a self-consistency round-trip: it compiles the
console's codec against the **real** `PDUParser` from the Cuttlefish simulator
(patched exactly as `apply.sh` patches it), so every PDU the console builds is
validated by the code that will validate it in the guest - including the strict
length arithmetic that catches septet-packing and UDH-padding mistakes. It also
decodes AOSP's own test vectors (whence the byte-exact expectation
`//CTSVVM:SYNC:key=value`), and guards the harness by asserting that a known-bad
PDU is still rejected.

| Group | Covers |
|---|---|
| Generated PDUs accepted by the simulator parser | plain 7-bit, escape table, UCS2 (incl. emoji), multipart with 8- and 16-bit references, binary WAP push with ports, alphanumeric sender, class 0 with an explicit time stamp, no-SMSC |
| Decode round-trips | text, SMSC, sender, DCS, UDH, concatenation, ports, time stamp |
| AOSP vectors | `0001000D9168…` → `//CTSVVM:SYNC:key=value` on `8618810189440`; `00010004814554…` → `你` on `5445` |
| Limits | 160 septets fits, 161 refused, 140-octet payload ceiling |
| Negative control | the corrupt PDU AOSP expects to fail is still rejected |

Still unverified: actually **booting** the image and confirming the framework
reports the SIM, that injected SMS appear in the Messaging app, and that the
control-plane socket is reachable from `adb shell` (expect the first boot to need
SELinux denial triage: `modem_simulator` is a brand-new domain and the console is
a coredomain client).

## Build environment gotchas

Four host issues made this look like a SIM problem when it was not. Run
`sim/host-quirks.sh --fix` to check all of them (and to apply the third one,
which is a repair inside the synced tree); otherwise check them by hand first:

1. **GNU coreutils, not uutils.** Ubuntu 26.04 ships `uutils` `expr`, whose
   `expr 25 == 25` is a *syntax error*. LineageOS gates
   `BOARD_MESA3D_MESON_ARGS` on `$(shell expr $(MESA_VERSION_MAJOR) \== 25)`, so on
   uutils that variable silently ends up **empty** and Mesa then tries to build
   `libclc` and fails with `Dependency "libclc" not found`. Fix:
   `apt install coreutils-from-gnu` (or point `/usr/bin/expr` at `/usr/bin/gnuexpr`).
   Check with `get_build_var BOARD_MESA3D_MESON_ARGS` — it must contain
   `-Dmesa-clc=system`.
2. **`pkg-config` and `ninja` must be installed.** Mesa's meson setup needs both;
   the failures read `Pkg-config for machine host machine not found` and
   `Could not detect Ninja v1.8.2 or newer`.
3. **`prebuilts/bootmgr`'s bundled glibc is broken on modern hosts.**
   `build/tasks/10-bootmgr-defs.mk` runs the prebuilt mtools/xorriso/grub tools
   through the *bundled* loader
   (`LD_LIBRARY_PATH=prebuilts/bootmgr/lib64 prebuilts/bootmgr/lib64/ld-linux-x86-64.so.2`),
   and `30-create_images.mk` then runs `mformat -F -i <img> -v <label> ::`, which
   converts the volume label through codepage 850. That bundled glibc cannot do
   the conversion and the build dies generating the FAT boot images:

   ```
   mformat: Error converting to codepage 850 Invalid argument
   ```

   Verified by replaying the shipped library from git against the repaired one:
   original → exit 1 with that error, system glibc → exit 0. Repair (what
   `host-quirks.sh --fix` does):

   ```shell
   ln -sfn /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 prebuilts/bootmgr/lib64/ld-linux-x86-64.so.2
   ln -sfn /usr/lib/x86_64-linux-gnu/libc.so.6          prebuilts/bootmgr/lib64/libc.so.6
   ```

   The other bundled libs (`libz`, `liblzma`, `libpthread`) are still used via
   `LD_LIBRARY_PATH`. This is a change inside a `repo`-managed prebuilt project,
   so `repo status` will show it as a modification of `prebuilts/bootmgr`.
4. **OTA packaging hangs on a QEMU guest.** The ramdisk ships
   `ROOT/d -> /sys/kernel/debug`; Info-ZIP `zip` (used by
   `ota_from_target_files`) follows symlinks and walks `/sys/kernel/debug` on the
   *host*. On a QEMU VM whose USB tablet registers a HID debug node, `zip` blocks
   forever reading `/sys/kernel/debug/hid/*/events`. Work around it by packaging
   in a private mount namespace with debugfs hidden:

   ```shell
   unshare -m --propagation private bash -c \
     'umount /sys/kernel/debug; source build/envsetup.sh; lunch lineage_virtio_x86_64-user; m otapackage'
   ```

   (Only the OTA/target-files zips are affected — the images, and `vm-utm-zip`,
   are produced before it.)

## Notes / limitations

* Injected SMS appear in whatever app handles them, but the *device's* own SMS
  database is the framework's: the console can make the network deliver a
  message and can observe what the user sends, it cannot write into `mmssms.db`.
  A message the user sends to a real-looking number is reported to `sms watch`
  rather than actually leaving the device (there is no network to leave).
* `modem_console` only works while `modem_simulator_virtio` is running; it reconnects
  per invocation, so a dead simulator gives `cannot reach the modem simulator`.
* Data call addresses/gateway/DNS are supplied by the simulator
  (`cf_device_config_virtio.cpp`). The emulated PDP context is reported as
  connected, but it is not bridged to the VM's real NIC, so cellular data does
  not route to the internet. Bridge it to `eth0` if you need real throughput.
* The vsock loopback rewrite is a one-line substitution against the pinned
  Cuttlefish source; if upstream changes that call site, update `apply.sh`.
* This overlay is applied at build time and is not a fork of the device tree:
  delete `sim/` (or skip `apply.sh`) to build plain LineageOS again.
