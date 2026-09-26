//
// Guest implementation of cuttlefish::modem::DeviceConfig.
//
// Upstream cf_device_config.cpp resolves these paths through CuttlefishConfig,
// which is host-only. On the guest everything lives under
// /data/misc/modem_simulator (seeded from /vendor/etc/modem_simulator by
// init.virtio.sim.rc).
//
#include "host/commands/modem_simulator/device_config.h"

#include <string>

namespace cuttlefish {
namespace modem {

namespace {
// Writable, per-device state (SIM profile, nvram).
constexpr char kModemDataDir[] = "/data/misc/modem_simulator/";
// Read-only seed config shipped by the build.
constexpr char kModemEtcDir[] = "/vendor/";
}  // namespace

int DeviceConfig::host_id() {
    return 0;
}

std::string DeviceConfig::PerInstancePath(const char* file_name) {
    return std::string(kModemDataDir) + file_name;
}

// Upstream this resolves against the host's Cuttlefish artifacts root. Callers
// pass paths like "etc/modem_simulator/files/iccprofile_for_sim0.xml", which the
// build installs to /vendor/etc/modem_simulator/files/.
std::string DeviceConfig::DefaultHostArtifactsPath(const std::string& file) {
    return std::string(kModemEtcDir) + file;
}

// Emulated PDP context. Reported to the framework as "connected"; not bridged to
// the VM's real NIC (see sim/README.md).
std::string DeviceConfig::ril_address_and_prefix() {
    return "10.10.0.2/24";
}

std::string DeviceConfig::ril_gateway() {
    return "10.10.0.1";
}

std::string DeviceConfig::ril_dns() {
    return "8.8.8.8";
}

std::ifstream DeviceConfig::open_ifstream_crossplat(const char* filename) {
    return std::ifstream(filename);
}

std::ofstream DeviceConfig::open_ofstream_crossplat(
    const char* filename, std::ios_base::openmode mode) {
    return std::ofstream(filename, mode);
}

}  // namespace modem
}  // namespace cuttlefish
