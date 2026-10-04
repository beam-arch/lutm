#!/usr/bin/env python3
"""Patch the Cuttlefish modem simulator into a controllable in-guest service.

Usage: patch_modem_simulator.py <dir-with-modem_simulator-sources>

The simulator already knows how to emulate a UICC, a network and SMS; what it
lacks on a standalone device is the control plane Cuttlefish drives it with from
the host. These patches add exactly that, plus the fidelity fixes needed for
injected traffic to be indistinguishable from real traffic:

  * deferred command handling until the modem services are fully registered
  * a broadcast channel, so a control-plane client sees what the device sends
  * SMS-DELIVER parsing, so a genuine network delivery can be validated (and
    therefore delivered verbatim, service centre address and time stamp intact)
  * a working subscriber-number API: AT+CNUM, a remote query, and the existing
    remote update - which upstream never actually parsed correctly
  * AT+REMOTEOPERATOR, to move the device to another operator

Every edit is applied by exact match and is idempotent, so running this twice is
safe. The staged sources are refreshed from device/google/cuttlefish on each run
of sim/apply.sh, so this always starts from pristine upstream files.
"""

import os
import sys

EDITS = []


def edit(path, old, new, what):
    EDITS.append({"path": path, "old": old, "new": new, "what": what})


# ---------------------------------------------------------------------------
# 0. Register services before accepting commands
# ---------------------------------------------------------------------------

edit(
    "channel_monitor.h",
    """  ChannelMonitor(ModemSimulator& modem, cuttlefish::SharedFD server);
  ~ChannelMonitor();
""",
    """  ChannelMonitor(ModemSimulator& modem, cuttlefish::SharedFD server);
  ~ChannelMonitor();
  void Start();
""",
    "channel_monitor.h: expose deferred monitor startup",
)

edit(
    "channel_monitor.cpp",
    """ChannelMonitor::ChannelMonitor(ModemSimulator& modem, SharedFD server)
    : modem_(modem), server_(std::move(server)) {
  if (!SharedFD::Pipe(&read_pipe_, &write_pipe_)) {
    LOG(ERROR) << "Unable to create pipe, ignore";
  }

  if (server_->IsOpen()) {
    monitor_thread_ = std::thread([this]() { MonitorLoop(); });
  }
}
""",
    """ChannelMonitor::ChannelMonitor(ModemSimulator& modem, SharedFD server)
    : modem_(modem), server_(std::move(server)) {
  if (!SharedFD::Pipe(&read_pipe_, &write_pipe_)) {
    LOG(ERROR) << "Unable to create pipe, ignore";
  }
}

void ChannelMonitor::Start() {
  if (server_->IsOpen() && !monitor_thread_.joinable()) {
    monitor_thread_ = std::thread([this]() { MonitorLoop(); });
  }
}
""",
    "channel_monitor.cpp: do not dispatch commands during construction",
)

edit(
    "modem_simulator.cpp",
    """  channel_monitor_ = std::move(channel_monitor);
  LoadNvramConfig();
  RegisterModemService();
}
""",
    """  channel_monitor_ = std::move(channel_monitor);
  LoadNvramConfig();
  RegisterModemService();
  // Accept commands only after the service registry is complete.
  channel_monitor_->Start();
}
""",
    "modem_simulator.cpp: start the monitor after registering services",
)


# ---------------------------------------------------------------------------
# 1. Broadcast to control-plane clients
# ---------------------------------------------------------------------------

edit(
    "channel_monitor.h",
    """  ClientId SetRemoteClient(SharedFD client, bool is_accepted);
  void SendRemoteCommand(ClientId client, std::string& response);
  void CloseRemoteConnection(ClientId client);
""",
    """  ClientId SetRemoteClient(SharedFD client, bool is_accepted);
  void SendRemoteCommand(ClientId client, std::string& response);
  // Broadcast to every control-plane (remote) client. The control plane is how
  // the emulated network reports what the device did: an SMS the user sent, a
  // delivery report that came back, a message the network delivered.
  void SendCommandToRemoteClients(std::string& response);
  void CloseRemoteConnection(ClientId client);
""",
    "channel_monitor.h: declare the control-plane broadcast",
)

edit(
    "channel_monitor.cpp",
    "ChannelMonitor::~ChannelMonitor() {",
    """void ChannelMonitor::SendCommandToRemoteClients(std::string& response) {
  for (auto& client : remote_clients_) {
    if (client->is_valid) {
      client->SendCommandResponse(response);
    }
  }
}

ChannelMonitor::~ChannelMonitor() {""",
    "channel_monitor.cpp: implement the control-plane broadcast",
)

edit(
    "modem_service.h",
    """  cuttlefish::SharedFD ConnectToRemoteCvd(std::string port);
  void SendCommandToRemote(ClientId remote_client, std::string response);
""",
    """  cuttlefish::SharedFD ConnectToRemoteCvd(std::string port);
  void SendCommandToRemote(ClientId remote_client, std::string response);
  // Report an event to every control-plane client (see ChannelMonitor).
  void SendCommandToRemoteClients(std::string response);
""",
    "modem_service.h: declare the broadcast helper",
)

edit(
    "modem_service.cpp",
    """void ModemService::SendCommandToRemote(ClientId remote_client,
                                       std::string response) {
  if (channel_monitor_) {
    channel_monitor_->SendRemoteCommand(remote_client, response);
    ;
  }
}
""",
    """void ModemService::SendCommandToRemote(ClientId remote_client,
                                       std::string response) {
  if (channel_monitor_) {
    channel_monitor_->SendRemoteCommand(remote_client, response);
    ;
  }
}

void ModemService::SendCommandToRemoteClients(std::string response) {
  if (channel_monitor_) {
    channel_monitor_->SendCommandToRemoteClients(response);
  }
}
""",
    "modem_service.cpp: implement the broadcast helper",
)

# ---------------------------------------------------------------------------
# 2. SMS-DELIVER support + verbatim delivery
# ---------------------------------------------------------------------------

edit(
    "pdu_parser.h",
    "  std::string CreateStatuReport(int message_reference);\n",
    """  std::string CreateStatuReport(int message_reference);

  // The PDU exactly as it was handed in, service centre address included. The
  // simulator delivers this verbatim so an injected message keeps the SMSC and
  // the service centre time stamp a real network would have put there.
  const std::string& GetRawPDU() const { return raw_pdu_; }
""",
    "pdu_parser.h: expose the PDU as received",
)

edit(
    "pdu_parser.h",
    "  bool is_valid_pdu_;\n",
    """  bool is_valid_pdu_;
  std::string raw_pdu_;
  std::string service_center_time_stamp_;
""",
    "pdu_parser.h: raw PDU and service centre time stamp storage",
)

edit(
    "pdu_parser.cpp",
    """  std::string_view pdu_view = pdu;
  size_t pos = 0;
""",
    """  std::string_view pdu_view = pdu;
  size_t pos = 0;
  raw_pdu_ = pdu;
""",
    "pdu_parser.cpp: remember the PDU as received",
)

edit(
    "pdu_parser.cpp",
    """  /* 3. MR: 1 byte */
  message_reference_ = pdu_view.substr(std::min(pos, pdu_total_length), 2);
  pos += 2;
""",
    """  // SMS-DELIVER (TP-MTI 00) has no message reference: the originator address
  // follows the first octet directly, and a service centre time stamp comes
  // after the data coding scheme. Upstream only ever parsed SMS-SUBMIT here
  // (a device sending to itself), which is why a message delivered by the
  // emulated network never passed the length check at the end of this function.
  const bool is_deliver = (Hex2ToByte(pdu_type_) & 0x03) == 0x00;

  /* 3. MR: 1 byte (SMS-SUBMIT and SMS-STATUS-REPORT only) */
  if (is_deliver) {
    message_reference_ = "";
  } else {
    message_reference_ = pdu_view.substr(std::min(pos, pdu_total_length), 2);
    pos += 2;
  }
""",
    "pdu_parser.cpp: parse SMS-DELIVER layout",
)

edit(
    "pdu_parser.cpp",
    """  /* 7. Data Code Scheme: 1 byte */
  data_code_scheme_ = pdu_view.substr(std::min(pos, pdu_total_length), 2);
  pos += 2;
""",
    """  /* 7. Data Code Scheme: 1 byte */
  data_code_scheme_ = pdu_view.substr(std::min(pos, pdu_total_length), 2);
  pos += 2;

  /* 7b. Service centre time stamp: 7 bytes (SMS-DELIVER only) */
  if (is_deliver) {
    service_center_time_stamp_ =
        pdu_view.substr(std::min(pos, pdu_total_length), 14);
    pos += 14;
  }
""",
    "pdu_parser.cpp: skip the service centre time stamp for SMS-DELIVER",
)

edit(
    "sms_service.cpp",
    """  std::string pdu(*cmd);
  PDUParser sms_pdu(pdu);
  if (!sms_pdu.IsValidPDU()) {
    LOG(ERROR) << "Failed to decode PDU";
    return;
  }
  pdu = sms_pdu.CreatePDU();
  if (pdu != "") {
    SendUnsolicitedCommand("+CMT: 0");
    SendUnsolicitedCommand(pdu);
  }
}
""",
    """  std::string pdu(*cmd);
  PDUParser sms_pdu(pdu);
  if (!sms_pdu.IsValidPDU()) {
    LOG(ERROR) << "Failed to decode PDU";
    return;
  }
  // Deliver the message exactly as the control plane built it. A real network
  // delivery carries the service centre address and the service centre time
  // stamp, and the framework shows that time stamp as the message time, so
  // re-encoding the PDU here would be visible on the device.
  std::string deliver = sms_pdu.GetRawPDU();
  if (deliver.empty()) {
    deliver = sms_pdu.CreatePDU();
  }
  if (deliver != "") {
    SendUnsolicitedCommand("+CMT: 0");
    SendUnsolicitedCommand(deliver);
    // Tell the control plane what the network just delivered.
    SendCommandToRemoteClients("AT+REMOTEDELIVER:" + deliver);
  }
}
""",
    "sms_service.cpp: deliver injected PDUs verbatim and report them",
)

edit(
    "sms_service.cpp",
    """  std::stringstream ss;
  ss << "+CMGS: " << ++message_reference_;
  responses.push_back(ss.str());
  responses.push_back("OK");
  client.SendCommandResponse(responses);

  if (sms_pdu.IsNeededStatuReport()) {
""",
    """  std::stringstream ss;
  ss << "+CMGS: " << ++message_reference_;
  responses.push_back(ss.str());
  responses.push_back("OK");
  client.SendCommandResponse(responses);

  // Let the control plane see what the device just put on the air.
  SendCommandToRemoteClients("AT+REMOTESENT:" + sms_pdu.GetRawPDU());

  if (sms_pdu.IsNeededStatuReport()) {
""",
    "sms_service.cpp: report outgoing messages to the control plane",
)

edit(
    "sms_service.cpp",
    """  if (pdu != "" && pdu_length > 0) {
    ss << "+CDS: " << pdu_length;
    SendUnsolicitedCommand(ss.str());
    SendUnsolicitedCommand(pdu);
  }
""",
    """  if (pdu != "" && pdu_length > 0) {
    ss << "+CDS: " << pdu_length;
    SendUnsolicitedCommand(ss.str());
    SendUnsolicitedCommand(pdu);
    SendCommandToRemoteClients("AT+REMOTESTATUS:" + pdu);
  }
""",
    "sms_service.cpp: report delivery reports to the control plane",
)

# ---------------------------------------------------------------------------
# 3. Subscriber number (MSISDN) API
# ---------------------------------------------------------------------------

edit(
    "sim_service.h",
    """  void HandlePhoneNumberUpdate(const Client& client,
                               const std::string& command);
""",
    """  void HandlePhoneNumberUpdate(const Client& client,
                               const std::string& command);
  void HandleGetPhoneNumber(const Client& client);
  void HandleQueryPhoneNumber(const Client& client);
""",
    "sim_service.h: declare the number API handlers",
)

edit(
    "sim_service.cpp",
    """      CommandHandler("+REMOTEUPADATEPHONENUMBER",
                    [this](const Client& client, std::string& cmd) {
                      this->HandlePhoneNumberUpdate(client,cmd);
                    }),
""",
    """      CommandHandler("+REMOTEUPADATEPHONENUMBER",
                    [this](const Client& client, std::string& cmd) {
                      this->HandlePhoneNumberUpdate(client,cmd);
                    }),
      // The subscriber number. AT+CNUM is what a real modem answers when the
      // framework asks for the number stored on the SIM; the REMOTE variant is
      // the control plane's view of the same value.
      CommandHandler("+CNUM",
                     [this](const Client& client) {
                       this->HandleGetPhoneNumber(client);
                     }),
      CommandHandler("+REMOTEQUERYPHONENUMBER",
                     [this](const Client& client) {
                       this->HandleQueryPhoneNumber(client);
                     }),
""",
    "sim_service.cpp: register AT+CNUM and the remote number query",
)

edit(
    "sim_service.cpp",
    """void SimService::HandlePhoneNumberUpdate(const Client& client,
                                         const std::string& command) {
  (void)client;
  CommandParser cmd(command);
  cmd.SkipWhiteSpace();
  SetPhoneNumber(cmd.GetNextStr(' '));
}
""",
    """void SimService::HandlePhoneNumberUpdate(const Client& client,
                                         const std::string& command) {
  CommandParser cmd(command);
  if (command.find('=') != std::string::npos) {
    cmd.SkipPrefix();  // AT+REMOTEUPADATEPHONENUMBER=<number>
  } else {
    cmd.SkipWhiteSpace();  // AT+REMOTEUPADATEPHONENUMBER <number>
  }

  // Accept the usual presentations of a subscriber number ("+1 555 123 4567",
  // "15551234567") but store the digits the SIM profile holds.
  std::string number;
  for (char c : std::string(cmd.GetNextStr(' '))) {
    if (c >= '0' && c <= '9') {
      number += c;
    } else if (c == '+' && number.empty()) {
      continue;  // international prefix: the profile records TON/NPI separately
    } else if (c == ' ') {
      continue;
    } else {
      client.SendCommandResponse(ModemService::kCmeErrorOperationNotAllowed);
      return;
    }
  }

  std::vector<std::string> responses;
  if (number.empty() || !SetPhoneNumber(number)) {
    responses.push_back(ModemService::kCmeErrorOperationNotAllowed);
  } else {
    LOG(INFO) << "Subscriber number updated to " << number;
    responses.push_back("OK");
  }
  client.SendCommandResponse(responses);
}

void SimService::HandleGetPhoneNumber(const Client& client) {
  const std::string number = GetPhoneNumber();
  if (number.empty()) {
    client.SendCommandResponse(ModemService::kCmeErrorOperationNotAllowed);
    return;
  }

  // The SIM profile records EF_MSISDN with TON/NPI 0x91 (international), which
  // is type-of-number 145 in +CNUM.
  std::vector<std::string> responses;
  responses.push_back("+CNUM: ,\\"+" + number + "\\",145");
  responses.push_back("OK");
  client.SendCommandResponse(responses);
}

void SimService::HandleQueryPhoneNumber(const Client& client) {
  const std::string number = GetPhoneNumber();
  std::vector<std::string> responses;
  responses.push_back("+REMOTEPHONENUMBER: " +
                      (number.empty() ? std::string("NONE") : number));
  responses.push_back("OK");
  client.SendCommandResponse(responses);
}
""",
    "sim_service.cpp: fix the number update and add AT+CNUM/query",
)

# ---------------------------------------------------------------------------
# 4. Moving between operators
# ---------------------------------------------------------------------------

edit(
    "network_service.h",
    """  void HandleReceiveRemoteCTEC(const Client& client, std::string& command);
  void HandleReceiveRemoteSignal(const Client& client, std::string& command);
""",
    """  void HandleReceiveRemoteCTEC(const Client& client, std::string& command);
  void HandleReceiveRemoteSignal(const Client& client, std::string& command);
  void HandleRemoteOperator(const Client& client, std::string& command);
""",
    "network_service.h: declare AT+REMOTEOPERATOR",
)

edit(
    "network_service.cpp",
    """      CommandHandler("+REMOTEREG",
                     [this](const Client& client, std::string& cmd) {
                       this->HandleReceiveRemoteVoiceDataReg(client, cmd);
                     }),
""",
    """      CommandHandler("+REMOTEREG",
                     [this](const Client& client, std::string& cmd) {
                       this->HandleReceiveRemoteVoiceDataReg(client, cmd);
                     }),
      CommandHandler("+REMOTEOPERATOR",
                     [this](const Client& client, std::string& cmd) {
                       this->HandleRemoteOperator(client, cmd);
                     }),
""",
    "network_service.cpp: register AT+REMOTEOPERATOR",
)

edit(
    "network_service.cpp",
    """void NetworkService::HandleIdentifierDisclosure(const std::string& command) {""",
    """static std::string LowerCopy(const std::string& s) {
  std::string out = s;
  for (auto& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

/* AT+REMOTEOPERATOR: <mccmnc | operator name> */
void NetworkService::HandleRemoteOperator(const Client& client,
                                          std::string& command) {
  const std::string prefix = "AT+REMOTEOPERATOR=";
  std::string wanted =
      command.size() > prefix.size() ? command.substr(prefix.size()) : "";
  while (!wanted.empty() && wanted.front() == ' ') wanted.erase(0, 1);
  while (!wanted.empty() && wanted.back() == ' ') wanted.pop_back();

  if (wanted.empty()) {
    client.SendCommandResponse(ModemService::kCmeErrorOperationNotAllowed);
    return;
  }

  const std::string wanted_lower = LowerCopy(wanted);
  auto exact = operator_list_.end();
  auto partial = operator_list_.end();
  int partial_count = 0;
  for (auto iter = operator_list_.begin(); iter != operator_list_.end(); ++iter) {
    if (iter->numeric == wanted || LowerCopy(iter->long_name) == wanted_lower ||
        LowerCopy(iter->short_name) == wanted_lower) {
      exact = iter;
      break;
    }
    if (LowerCopy(iter->long_name).find(wanted_lower) != std::string::npos ||
        LowerCopy(iter->short_name).find(wanted_lower) != std::string::npos) {
      partial = iter;
      ++partial_count;
    }
  }

  auto found = exact;
  if (found == operator_list_.end() && partial_count == 1) {
    found = partial;
  }
  if (found == operator_list_.end()) {
    LOG(DEBUG) << "Unknown operator: " << wanted;
    client.SendCommandResponse(ModemService::kCmeErrorOperationNotAllowed);
    return;
  }

  current_operator_numeric_ = found->numeric;
  for (auto& op : operator_list_) {
    if (op.numeric == current_operator_numeric_) {
      op.operator_state = NetworkOperator::OPER_STATE_CURRENT;
    } else if (op.operator_state != NetworkOperator::OPER_STATE_FORBIDDEN) {
      op.operator_state = NetworkOperator::OPER_STATE_AVAILABLE;
    }
  }

  // Re-register on the new network so the framework learns about it now rather
  // than at the next poll.
  const RegistrationState saved_state =
      voice_registration_status_.registration_state;
  UpdateRegisterState(NET_REGISTRATION_UNREGISTERED);
  thread_looper_->Post(
      makeSafeCallback(this, &NetworkService::UpdateRegisterState,
                       saved_state == NET_REGISTRATION_UNREGISTERED
                           ? NET_REGISTRATION_HOME
                           : saved_state),
      std::chrono::milliseconds(300));

  LOG(INFO) << "Emulated operator is now " << found->long_name << " ("
            << found->numeric << ")";
  client.SendCommandResponse("OK");
}

void NetworkService::HandleIdentifierDisclosure(const std::string& command) {""",
    "network_service.cpp: implement AT+REMOTEOPERATOR",
)


def apply_edits(root):
    failures = []
    changed = 0
    skipped = 0
    for item in EDITS:
        path = os.path.join(root, item["path"])
        if not os.path.isfile(path):
            failures.append(f"{item['path']}: not found in {root}")
            continue
        with open(path, encoding="utf-8") as handle:
            content = handle.read()
        if item["new"] in content:
            skipped += 1
            continue
        if content.count(item["old"]) != 1:
            failures.append(
                f"{item['path']}: expected exactly one match for '{item['what']}' "
                f"({content.count(item['old'])} found) - upstream source changed?"
            )
            continue
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(content.replace(item["old"], item["new"]))
        changed += 1
        print(f"patch: {item['what']}")
    return changed, skipped, failures


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    root = sys.argv[1]
    if not os.path.isdir(root):
        print(f"patch_modem_simulator.py: {root} is not a directory", file=sys.stderr)
        return 2
    changed, skipped, failures = apply_edits(root)
    if failures:
        for failure in failures:
            print(f"patch_modem_simulator.py: ERROR: {failure}", file=sys.stderr)
        return 1
    print(f"patch_modem_simulator.py: {changed} applied, {skipped} already applied")
    if changed == 0 and skipped == 0:
        print("patch_modem_simulator.py: nothing to patch", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
