//
// Guest-side entry point for the Cuttlefish modem simulator.
//
// Upstream main.cpp is a host program: it reads CuttlefishConfig, opens a
// launcher monitor socket and takes already-connected VSOCK server fds on the
// command line. In this port the simulator is the server itself, running inside
// the guest. The RIL (patched by sim/apply.sh to use VMADDR_CID_LOCAL) connects
// over VSOCK loopback, and modem_console attaches over a local socket as a
// control-plane client - the role Cuttlefish's host plays, minus the host.
//
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <android-base/logging.h>

#include "common/libs/fs/shared_fd.h"
#include "host/commands/modem_simulator/channel_monitor.h"
#include "host/commands/modem_simulator/modem_simulator.h"
#include "host/commands/modem_simulator/nvram_config.h"

namespace {

constexpr int kDefaultPort = 9200;
constexpr int kSimType = 1;  // 1 == normal SIM profile, 2 == CTS profile.

// Abstract (not a file on disk), so no directory or file label is needed. Keep
// in sync with modem_console's kDefaultSocket.
constexpr char kDefaultControlSocket[] = "modem_simulator_console";

// Keep in sync with androidboot.modem_simulator_ports (virtio-sim-board.mk).
int PortFromArgs(int argc, char** argv) {
    int port = kDefaultPort;
    const std::string prefix = "--port=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) {
            port = std::stoi(arg.substr(prefix.size()));
        }
    }
    return port;
}

std::string ControlSocketFromArgs(int argc, char** argv) {
    std::string name = kDefaultControlSocket;
    const std::string prefix = "--control-socket=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) {
            name = arg.substr(prefix.size());
        }
    }
    return name;
}

}  // namespace

int main(int argc, char** argv) {
    android::base::InitLogging(argv, android::base::StderrLogger);

    // The RIL can close the connection on reboot; don't take the process down.
    signal(SIGPIPE, SIG_IGN);

    const int port = PortFromArgs(argc, argv);
    LOG(INFO) << "Starting in-guest modem simulator on VSOCK port " << port;

    cuttlefish::NvramConfig::InitNvramConfigService(1, kSimType);

    auto server =
        cuttlefish::SharedFD::VsockServer(port, SOCK_STREAM, std::nullopt);
    if (!server->IsOpen()) {
        LOG(ERROR) << "Could not bind modem simulator VSOCK server on port "
                   << port << ": " << server->StrError();
        return 1;
    }

    auto modem_simulator = std::make_unique<cuttlefish::ModemSimulator>(0);
    auto channel_monitor =
        std::make_unique<cuttlefish::ChannelMonitor>(*modem_simulator, server);
    modem_simulator->Initialize(std::move(channel_monitor));

    // Control plane. Connections here are registered as remote clients, which
    // is how modem_console drives the emulated network: injecting SMS, changing
    // the registration state or the operator, and so on. A remote client must
    // send a token ("REM0", as Cuttlefish's launcher does) as its first bytes;
    // the simulator consumes it while accepting the connection.
    const std::string control_socket = ControlSocketFromArgs(argc, argv);
    auto control_server = cuttlefish::SharedFD::SocketLocalServer(
        control_socket, /*abstract=*/true, SOCK_STREAM, 0666);
    if (!control_server->IsOpen()) {
        LOG(ERROR) << "Could not listen for control-plane clients on "
                   << control_socket << ": " << control_server->StrError()
                   << " (modem_console will not be able to attach)";
    } else {
        LOG(INFO) << "Control plane listening on abstract socket "
                  << control_socket;
    }

    while (true) {
        if (!control_server->IsOpen()) {
            // Nothing left to accept; keep the modem running for the RIL.
            while (true) {
                pause();
            }
        }
        auto connection = cuttlefish::SharedFD::Accept(*control_server);
        if (!connection->IsOpen()) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "Control-plane accept failed: "
                       << connection->StrError();
            continue;
        }
        modem_simulator->SetRemoteClient(connection, /*is_accepted=*/true);
        LOG(INFO) << "Control-plane client attached";
    }
}
