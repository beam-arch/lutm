//
// modem_console - the emulated-SIM control plane for virtio devices.
//
// The in-guest modem simulator (modem_simulator_virtio) already knows how to
// emulate a UICC and a network; what it lacks on a standalone device is the
// host-side control plane Cuttlefish uses to drive it. This console is that
// control plane: it attaches to the simulator's remote channel and speaks the
// same AT+REMOTE* protocol the Cuttlefish launcher does, wrapped in commands
// that describe what the *network* is doing.
//
//   modem_console sms recv --from +15551234567 --text "Your code is 123456"
//   modem_console sms watch
//   modem_console tower deregister
//   modem_console number set +15559876543
//
// Everything it injects is a real PDU built the way a network builds it, so the
// device sees an ordinary SMS from an ordinary tower.
//
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "sms_pdu.h"

namespace {

// Must match modem_simulator_virtio (sim/overlay/modem_simulator/main_virtio.cpp).
constexpr char kDefaultSocket[] = "modem_simulator_console";
constexpr int kDefaultTimeoutMs = 700;

const char* const kUsage =
    "modem_console - drive the emulated SIM, SMS and tower of this device\n"
    "\n"
    "usage: modem_console [--socket NAME] [--json] [--verbose] <command> [args]\n"
    "       modem_console            # interactive console\n"
    "\n"
    "identity / number\n"
    "  status                              everything below, in one shot\n"
    "  identity                            MSISDN, IMSI, ICCID, IMEI, operator\n"
    "  number                              show the SIM's own MSISDN\n"
    "  number set <E.164>                  rewrite MSISDN in the SIM profile\n"
    "\n"
    "sms\n"
    "  sms recv --from <addr> --text <t>   deliver an SMS to the device\n"
    "      [--alnum]                       sender is a name, not a number\n"
    "      [--ucs2|--gsm7|--8bit --data-hex <hex>]\n"
    "      [--smsc auto|<num>|none] [--pid N] [--class N] [--dcs N]\n"
    "      [--port <dst>[,<src>]] [--concat <ref>,<seq>,<total>]\n"
    "      [--udh-hex <hex>] [--concat-hex 00,03,..]\n"
    "      [--scts now|<epoch>] [--age <seconds>]\n"
    "      [--more] [--rp] [--sri]\n"
    "  sms raw <pdu-hex>                   deliver an exact PDU\n"
    "  sms decode <pdu-hex>                decode a PDU (no device needed)\n"
    "  sms build  ...                      same flags as recv, print the PDU\n"
    "  sms watch [--interval <sec>]        live in/out SMS + registration events\n"
    "\n"
    "tower\n"
    "  tower status                        registration, signal, operator, RAT\n"
    "  tower register [home|searching|denied|unknown|roaming|emergency|<0-8>]\n"
    "  tower deregister                    tower disappears (no service)\n"
    "  tower signal <0-100>                signal strength in percent\n"
    "  tower tech <gsm|wcdma|lte|nr|...>   change the radio access technology\n"
    "  tower power on|off                  radio on/off (airplane-mode feel)\n"
    "  tower operator <mccmnc|name>        move to another operator\n"
    "\n"
    "other\n"
    "  raw <AT command>                    send an AT command verbatim\n"
    "  listen                              print unsolicited simulator events\n"
    "  help\n";

// ---------------------------------------------------------------------------
// Remote channel
// ---------------------------------------------------------------------------

class Channel {
 public:
  ~Channel() { Close(); }

  bool Connect(const std::string& name, std::string* err) {
    Close();
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      *err = std::string("socket: ") + strerror(errno);
      return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';  // abstract namespace, like SharedFD::SocketLocalServer
    const size_t namelen = name.size();
    if (namelen + 2 > sizeof(addr.sun_path)) {
      *err = "socket name too long";
      Close();
      return false;
    }
    memcpy(addr.sun_path + 1, name.data(), namelen);
    const socklen_t len = static_cast<socklen_t>(namelen + 2);
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), len) < 0) {
      *err = "cannot reach the modem simulator (" + name + "): " + strerror(errno) +
             "\nis modem_simulator_virtio running?";
      Close();
      return false;
    }
    // The simulator reads the first thing a remote client sends while accepting
    // it, and discards it as an unknown AT command; "REM0" is that token (the
    // same one Cuttlefish's launcher sends).
    const char token[] = "REM0\r";
    if (::write(fd_, token, sizeof(token) - 1) < 0) {
      *err = std::string("handshake failed: ") + strerror(errno);
      Close();
      return false;
    }
    return true;
  }

  bool Send(const std::string& command) {
    if (fd_ < 0) return false;
    const std::string line = command + "\r";
    return ::write(fd_, line.data(), line.size()) == static_cast<ssize_t>(line.size());
  }

  // Reads whatever the simulator sends until it goes quiet for `quiet_ms`.
  std::vector<std::string> Collect(int total_ms, int quiet_ms = 120) {
    std::vector<std::string> lines;
    if (fd_ < 0) return lines;
    std::string buffer;
    const long long deadline = NowMs() + total_ms;
    long long last_data = NowMs();
    while (true) {
      const long long now = NowMs();
      const long long remaining = std::min(deadline - now, quiet_ms - (now - last_data));
      if (remaining <= 0) break;
      struct pollfd pfd{fd_, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, static_cast<int>(remaining));
      if (ready <= 0) {
        if (now - last_data >= quiet_ms) break;
        continue;
      }
      char buf[4096];
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n <= 0) break;
      buffer.append(buf, static_cast<size_t>(n));
      last_data = NowMs();
      SplitLines(&buffer, &lines, /*final=*/false);
    }
    SplitLines(&buffer, &lines, /*final=*/true);
    return lines;
  }

  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  static long long NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  }

  static void SplitLines(std::string* buffer, std::vector<std::string>* lines, bool final) {
    for (;;) {
      const size_t pos = buffer->find_first_of("\r\n");
      if (pos == std::string::npos) break;
      std::string line = buffer->substr(0, pos);
      buffer->erase(0, pos + 1);
      if (!line.empty()) lines->push_back(line);
    }
    if (final && !buffer->empty()) {
      lines->push_back(*buffer);
      buffer->clear();
    }
  }

 private:
  int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

struct Options {
  std::string socket = kDefaultSocket;
  bool json = false;
  bool verbose = false;
};

std::string Timestamp() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv {};
  localtime_r(&ts.tv_sec, &tmv);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return buf;
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return s;
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

bool IsOk(const std::vector<std::string>& lines) {
  for (const auto& line : lines) {
    if (line == "OK") return true;
  }
  return false;
}

std::string FirstLineStarting(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0) return line;
  }
  return "";
}

std::string Trim(std::string s) {
  size_t begin = 0, end = s.size();
  while (begin < end && isspace(static_cast<unsigned char>(s[begin]))) ++begin;
  while (end > begin && isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return s.substr(begin, end - begin);
}

// The payload of the first line that starts with `prefix`, trimmed.
std::string AfterPrefix(const std::vector<std::string>& lines, const std::string& prefix) {
  const std::string line = FirstLineStarting(lines, prefix);
  if (line.size() < prefix.size()) return "";
  std::string value = Trim(line.substr(prefix.size()));
  if (!value.empty() && value.front() == ':') value = Trim(value.substr(1));
  return value;
}

std::string Quoted(const std::vector<std::string>& lines, const std::string& prefix) {
  const std::string line = FirstLineStarting(lines, prefix);
  const size_t first = line.find('"');
  if (first == std::string::npos) return "";
  const size_t second = line.find('"', first + 1);
  if (second == std::string::npos) return "";
  return line.substr(first + 1, second - first - 1);
}

int ParseInt(const std::string& s, int fallback = -1) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  const long v = strtol(s.c_str(), &end, 0);
  if (end == s.c_str() || *end != '\0') return fallback;
  return static_cast<int>(v);
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

unsigned ParsePort(const std::string& s, bool* ok) {
  *ok = true;
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
    const int v = ParseInt(s, -1);
    if (v < 0 || v > 65535) *ok = false;
    return static_cast<unsigned>(v);
  }
  const int v = ParseInt(s, -1);
  if (v < 0 || v > 65535) *ok = false;
  return static_cast<unsigned>(v);
}

std::string HexByte(unsigned v) {
  char buf[4];
  snprintf(buf, sizeof(buf), "%02X", v & 0xFF);
  return buf;
}

// A tiny flag parser: "--flag value" / "--flag=value" / boolean flags.
class Flags {
 public:
  explicit Flags(const std::vector<std::string>& args) { args_ = args; }

  // Flags that never take a value; without this, Positional() would swallow the
  // token after them.
  static bool IsBoolean(const std::string& name) {
    static const char* const kBoolean[] = {"--alnum",  "--ucs2", "--gsm7",  "--8bit",
                                           "--8-bit", "--more", "--rp",    "--sri"};
    for (const char* b : kBoolean) {
      if (name == b) return true;
    }
    return false;
  }

  bool Has(const std::string& name) const {
    for (const auto& a : args_) {
      if (a == name) return true;
      if (a.rfind(name + "=", 0) == 0) return true;
    }
    return false;
  }

  std::string Get(const std::string& name, const std::string& fallback = "") const {
    for (size_t i = 0; i < args_.size(); ++i) {
      if (args_[i] == name && i + 1 < args_.size()) return args_[i + 1];
      if (args_[i].rfind(name + "=", 0) == 0) return args_[i].substr(name.size() + 1);
    }
    return fallback;
  }

  // Positional arguments in order (flags and their values removed).
  std::vector<std::string> Positional() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < args_.size(); ++i) {
      if (args_[i].rfind("--", 0) == 0) {
        if (args_[i].find('=') == std::string::npos && !IsBoolean(args_[i])) ++i;  // skip its value
        continue;
      }
      out.push_back(args_[i]);
    }
    return out;
  }

 private:
  std::vector<std::string> args_;
};

// ---------------------------------------------------------------------------
// Tower / registration vocabulary
// ---------------------------------------------------------------------------

struct RegistrationState {
  const char* name;
  int value;
  const char* description;
};

const RegistrationState kRegStates[] = {
    {"deregister", 0, "not registered, not searching"},
    {"home", 1, "registered on the home network"},
    {"searching", 2, "not registered, searching for a tower"},
    {"denied", 3, "registration denied"},
    {"unknown", 4, "registration state unknown"},
    {"roaming", 5, "registered, roaming"},
    {"emergency", 8, "registered for emergency services only"},
};

int ResolveRegistrationState(const std::string& in, bool* ok) {
  *ok = true;
  if (in.empty()) return 1;
  const std::string lower = ToLower(in);
  for (const auto& s : kRegStates) {
    if (lower == s.name) return s.value;
  }
  const int value = ParseInt(in);
  if (value < 0) {
    *ok = false;
    return 0;
  }
  return value;
}

struct Tech {
  const char* name;
  int mask;
};

const Tech kTechs[] = {
    {"gsm", 1 << 0},      {"2g", 1 << 0},     {"wcdma", 1 << 1}, {"umts", 1 << 1},
    {"3g", 1 << 1},       {"cdma", 1 << 2},   {"evdo", 1 << 3},  {"tdscdma", 1 << 4},
    {"lte", 1 << 5},      {"4g", 1 << 5},     {"nr", 1 << 6},    {"5g", 1 << 6},
};

const char* TechName(int mask) {
  for (const auto& t : kTechs) {
    if (t.mask == mask) return t.name;
  }
  return "?";
}

int ResolveTech(const std::string& in, bool* ok) {
  *ok = true;
  const std::string lower = ToLower(in);
  for (const auto& t : kTechs) {
    if (lower == t.name) return t.mask;
  }
  int mask = 0;
  for (const auto& part : Split(lower, ',')) {
    bool found = false;
    for (const auto& t : kTechs) {
      if (part == t.name) {
        mask |= t.mask;
        found = true;
      }
    }
    if (!found) {
      *ok = false;
      return 0;
    }
  }
  return mask;
}

// ---------------------------------------------------------------------------
// The console
// ---------------------------------------------------------------------------

class Console {
 public:
  explicit Console(const Options& opt) : opt_(opt) {}

  // Commands that only build or decode PDUs work without a device, so the
  // connection is made on first use.
  bool EnsureConnected() {
    if (connected_) return true;
    std::string err;
    if (!channel_.Connect(opt_.socket, &err)) {
      fprintf(stderr, "modem_console: %s\n", err.c_str());
      return false;
    }
    connected_ = true;
    return true;
  }

  std::vector<std::string> Ask(const std::string& command, int timeout_ms = kDefaultTimeoutMs) {
    if (opt_.verbose) fprintf(stderr, "AT> %s\n", command.c_str());
    if (!EnsureConnected()) return {};
    if (!channel_.Send(command)) {
      fprintf(stderr, "modem_console: failed to send command\n");
      return {};
    }
    return channel_.Collect(timeout_ms);
  }

  void Print(const std::vector<std::string>& lines) {
    if (opt_.json) {
      std::string out = "{\"response\":[";
      for (size_t i = 0; i < lines.size(); ++i) {
        out += std::string(i ? "," : "") + "\"" + JsonEscape(lines[i]) + "\"";
      }
      out += "]}";
      printf("%s\n", out.c_str());
      return;
    }
    for (const auto& line : lines) printf("%s\n", line.c_str());
  }

  // Returns the command's exit status, so scripts can rely on it.
  int RunCommand(std::istringstream& in);
  int Watch(double interval_sec);
  int Listen();
  bool quit_requested() const { return quit_requested_; }

  Options opt_;

 private:
  bool connected_ = false;
  bool quit_requested_ = false;

  int CmdStatus();
  int CmdIdentity();
  int CmdNumber(const Flags& flags, const std::vector<std::string>& positional);
  int CmdSms(const std::vector<std::string>& positional, const Flags& flags);
  int CmdTower(const std::vector<std::string>& positional, const Flags& flags);
  int BuildAndMaybeSend(const Flags& flags, bool send);
  std::string ResolveSmsc(const std::string& requested);
  void PrintStrings(const std::vector<std::string>& items);

  Channel channel_;
};

void Console::PrintStrings(const std::vector<std::string>& items) {
  if (opt_.json) {
    std::string out = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
      out += std::string(i ? "," : "") + "\"" + JsonEscape(items[i]) + "\"";
    }
    out += "]}";
    printf("%s\n", out.c_str());
    return;
  }
  for (const auto& item : items) printf("%s\n", item.c_str());
}

std::string Console::ResolveSmsc(const std::string& requested) {
  if (requested == "none") return "";
  if (requested != "auto") return requested;
  const auto lines = Ask("AT+CSCA?");
  const std::string sca = Quoted(lines, "+CSCA:");
  if (opt_.verbose) {
    fprintf(stderr, "resolved SMSC from the SIM: %s\n", sca.empty() ? "(none)" : sca.c_str());
  }
  return sca;
}

int Console::CmdStatus() {
  struct Query {
    const char* command;
    const char* label;
  };
  const Query queries[] = {
      {"AT+CIMI", "IMSI"},
      {"AT+CICCID", "ICCID"},
      {"AT+CGSN", "IMEI"},
      {"AT+CSCA?", "SMSC"},
      {"AT+REMOTEQUERYPHONENUMBER", "MSISDN"},
      {"AT+COPS?", "operator"},
      {"AT+CREG?", "registration (CS)"},
      {"AT+CGREG?", "registration (PS)"},
      {"AT+CSQ", "signal"},
      {"AT+CTEC?", "radio access technology"},
  };
  std::vector<std::string> rows;
  for (const auto& q : queries) {
    std::string joined;
    for (const auto& line : Ask(q.command)) {
      if (line == "OK" || line == "ERROR") continue;
      if (!joined.empty()) joined += " | ";
      joined += line;
    }
    if (joined.empty()) joined = "(no response)";
    rows.push_back(std::string(q.label) + ": " + joined);
  }
  if (opt_.json) {
    std::string out = "{";
    for (size_t i = 0; i < rows.size(); ++i) {
      const size_t colon = rows[i].find(": ");
      out += std::string(i ? "," : "") + "\"" + JsonEscape(rows[i].substr(0, colon)) + "\":\"" +
             JsonEscape(rows[i].substr(colon + 2)) + "\"";
    }
    out += "}";
    printf("%s\n", out.c_str());
    return 0;
  }
  for (const auto& row : rows) printf("%s\n", row.c_str());
  return 0;
}

int Console::CmdIdentity() {
  const auto imsi = Ask("AT+CIMI");
  const auto iccid = Ask("AT+CICCID");
  const auto imei = Ask("AT+CGSN");
  const auto number = Ask("AT+REMOTEQUERYPHONENUMBER");
  const auto cops = Ask("AT+COPS?");
  std::vector<std::string> rows;
  auto add = [&](const char* label, const std::vector<std::string>& lines,
                 const std::string& prefix) {
    std::string value = FirstLineStarting(lines, prefix);
    if (value.empty()) {
      for (const auto& line : lines) {
        if (line != "OK" && line != "ERROR" && !line.empty()) {
          value = line;
          break;
        }
      }
    }
    if (!prefix.empty() && value.rfind(prefix, 0) == 0) value = value.substr(prefix.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == ':')) value.erase(0, 1);
    rows.push_back(std::string(label) + ": " + (value.empty() ? "(unknown)" : value));
  };
  add("MSISDN (SIM EF_MSISDN)", number, "+REMOTEPHONENUMBER:");
  add("IMSI", imsi, "+CIMI:");
  add("ICCID", iccid, "+CICCID:");
  add("IMEI", imei, "+CGSN:");
  add("operator", cops, "+COPS:");
  PrintStrings(rows);
  return 0;
}

int Console::CmdNumber(const Flags& flags, const std::vector<std::string>& positional) {
  (void)flags;
  if (positional.empty() || positional[0] == "get") {
    std::string value = AfterPrefix(Ask("AT+REMOTEQUERYPHONENUMBER"), "+REMOTEPHONENUMBER:");
    if (value.empty()) value = Quoted(Ask("AT+CNUM"), "+CNUM:");
    if (value.empty()) {
      PrintStrings({"MSISDN: (unknown)"});
      return 1;
    }
    PrintStrings({"MSISDN: " + value});
    return 0;
  }
  if (positional[0] == "set") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: number set needs a number\n");
      return 1;
    }
    const auto lines = Ask("AT+REMOTEUPADATEPHONENUMBER=" + positional[1]);
    if (!IsOk(lines)) {
      fprintf(stderr, "modem_console: the simulator rejected the new number\n");
      Print(lines);
      return 1;
    }
    const std::string current = AfterPrefix(Ask("AT+REMOTEQUERYPHONENUMBER"), "+REMOTEPHONENUMBER:");
    PrintStrings({"MSISDN: " + (current.empty() ? std::string("(unknown)") : current)});
    return 0;
  }
  fprintf(stderr, "modem_console: unknown number subcommand '%s'\n", positional[0].c_str());
  return 1;
}

int Console::BuildAndMaybeSend(const Flags& flags, bool send) {
  simsms::SmsDeliver pdu;
  std::string explicit_pdu = flags.Get("--hex");

  if (explicit_pdu.empty()) {
    pdu.sender = flags.Get("--from");
    pdu.sender_alphanumeric = flags.Has("--alnum");
    if (pdu.sender.empty()) {
      fprintf(stderr, "modem_console: --from <address> is required\n");
      return 1;
    }
    pdu.text = flags.Get("--text");
    if (flags.Has("--gsm7") && !simsms::IsGsm7(pdu.text)) {
      fprintf(stderr, "modem_console: --gsm7 was requested but the text needs more than the "
                      "GSM 7-bit alphabet (use --ucs2, or drop --gsm7 to send the same text as "
                      "UCS2 the way a handset would)\n");
      return 1;
    }
    pdu.ucs2 = flags.Has("--ucs2");
    pdu.eight_bit = flags.Has("--8bit") || flags.Has("--8-bit");
    pdu.data_hex = flags.Get("--data-hex");
    pdu.more_messages = flags.Has("--more");
    pdu.reply_path = flags.Has("--rp");
    pdu.status_report_request = flags.Has("--sri");
    const std::string pid = flags.Get("--pid");
    if (!pid.empty()) pdu.pid = static_cast<uint8_t>(ParseInt(pid, 0));
    const std::string klass = flags.Get("--class");
    if (!klass.empty()) pdu.msg_class = ParseInt(klass, -1);
    const std::string dcs = flags.Get("--dcs");
    if (!dcs.empty()) {
      pdu.use_dcs = true;
      pdu.dcs = static_cast<uint8_t>(ParseInt(dcs, 0));
    }

    // Timestamp: --scts now|<epoch> or --age <seconds>.
    const std::string age = flags.Get("--age");
    const std::string scts = flags.Get("--scts");
    if (!age.empty()) {
      pdu.scts = std::time(nullptr) - ParseInt(age, 0);
    } else if (!scts.empty() && scts != "now") {
      pdu.scts = static_cast<std::time_t>(ParseInt(scts, 0));
    }
    if (pdu.scts <= 0) pdu.scts = std::time(nullptr);

    // UDH: concatenation and/or application ports, in the order a handset uses.
    std::string udh;
    const std::string concat = flags.Get("--concat");
    if (!concat.empty()) {
      const auto parts = Split(concat, ',');
      if (parts.size() != 3) {
        fprintf(stderr, "modem_console: --concat wants <ref>,<seq>,<total>\n");
        return 1;
      }
      const int ref = ParseInt(parts[0], -1), seq = ParseInt(parts[1], -1),
                total = ParseInt(parts[2], -1);
      if (ref < 0 || seq < 1 || total < 1 || seq > total || total > 255) {
        fprintf(stderr, "modem_console: --concat wants 0<=ref<=65535, 1<=seq<=total<=255\n");
        return 1;
      }
      if (ref <= 255) {
        udh += HexByte(0x00) + HexByte(0x03) + HexByte(ref) + HexByte(total) + HexByte(seq);
      } else {
        udh += HexByte(0x08) + HexByte(0x04) + HexByte(ref >> 8) + HexByte(ref & 0xFF) +
               HexByte(total) + HexByte(seq);
      }
    }
    const std::string port = flags.Get("--port");
    if (!port.empty()) {
      const auto parts = Split(port, ',');
      bool ok = false;
      const unsigned dst = ParsePort(parts[0], &ok);
      if (!ok) {
        fprintf(stderr, "modem_console: --port wants <dst>[,<src>]\n");
        return 1;
      }
      const unsigned src = parts.size() > 1 ? ParsePort(parts[1], &ok) : dst;
      if (!ok) {
        fprintf(stderr, "modem_console: --port wants <dst>[,<src>]\n");
        return 1;
      }
      if (dst <= 255 && src <= 255) {
        udh += HexByte(0x04) + HexByte(0x02) + HexByte(dst) + HexByte(src);
      } else {
        udh += HexByte(0x05) + HexByte(0x04) + HexByte(dst >> 8) + HexByte(dst & 0xFF) +
               HexByte(src >> 8) + HexByte(src & 0xFF);
      }
    }
    const std::string extra_udh = flags.Get("--udh-hex");
    if (!extra_udh.empty()) {
      std::string cleaned;
      for (char c : extra_udh) {
        if (!isxdigit(static_cast<unsigned char>(c))) continue;
        cleaned += c;
      }
      if (cleaned.size() % 2 != 0) {
        fprintf(stderr, "modem_console: --udh-hex must be whole bytes\n");
        return 1;
      }
      udh += cleaned;
    }
    pdu.udh_hex = udh;

    pdu.smsc = ResolveSmsc(flags.Get("--smsc", "auto"));

    std::string err;
    explicit_pdu = simsms::BuildDeliver(pdu, &err);
    if (explicit_pdu.empty()) {
      fprintf(stderr, "modem_console: %s\n", err.empty() ? "cannot build the PDU" : err.c_str());
      return 1;
    }
  } else {
    const simsms::DecodedSms decoded = simsms::Decode(explicit_pdu);
    if (!decoded.ok) {
      fprintf(stderr, "modem_console: invalid PDU: %s\n", decoded.error.c_str());
      return 1;
    }
  }

  if (!send) {
    PrintStrings({explicit_pdu});
    return 0;
  }

  const auto lines = Ask("AT+REMOTESMS=" + explicit_pdu);
  const simsms::DecodedSms decoded = simsms::Decode(explicit_pdu);
  if (opt_.json) {
    printf("{\"delivered\":true,\"pdu\":\"%s\",\"from\":\"%s\",\"text\":\"%s\"}\n",
           explicit_pdu.c_str(), JsonEscape(decoded.address).c_str(),
           JsonEscape(decoded.text).c_str());
  } else {
    fprintf(stderr, "delivered SMS from %s\n  pdu:  %s\n  text: %s\n",
            decoded.address.empty() ? "(no address)" : decoded.address.c_str(),
            explicit_pdu.c_str(), decoded.text.c_str());
  }
  (void)lines;
  return 0;
}

int Console::CmdSms(const std::vector<std::string>& positional, const Flags& flags) {
  const std::string sub = positional.empty() ? "recv" : positional[0];
  if (sub.empty() || sub == "recv" || sub == "receive") {
    return BuildAndMaybeSend(flags, /*send=*/true);
  }
  if (sub == "build") {
    return BuildAndMaybeSend(flags, /*send=*/false);
  }
  if (sub == "raw") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: sms raw needs a PDU\n");
      return 1;
    }
    std::string hex;
    for (char c : positional[1]) {
      if (isxdigit(static_cast<unsigned char>(c))) hex += static_cast<char>(toupper(c));
    }
    const simsms::DecodedSms decoded = simsms::Decode(hex);
    if (!decoded.ok) {
      fprintf(stderr, "modem_console: invalid PDU: %s\n", decoded.error.c_str());
      return 1;
    }
    Ask("AT+REMOTESMS=" + hex);
    PrintStrings({hex});
    return 0;
  }
  if (sub == "decode") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: sms decode needs a PDU\n");
      return 1;
    }
    const simsms::DecodedSms d = simsms::Decode(positional[1]);
    if (!d.ok) {
      fprintf(stderr, "modem_console: %s\n", d.error.c_str());
      return 1;
    }
    std::vector<std::string> rows = {
        "type: " + d.type,
        "smsc: " + (d.smsc.empty() ? std::string("(none)") : d.smsc),
        std::string("address: ") + d.address + (d.address_alphanumeric ? " (alphanumeric)" : ""),
        "text: " + d.text,
        "scts: " + d.scts,
        "dcs: 0x" + HexByte(d.dcs) + "  pid: 0x" + HexByte(d.pid) + "  udl: " + std::to_string(d.udl),
    };
    if (!d.discharge_time.empty()) rows.push_back("discharge: " + d.discharge_time);
    if (d.status >= 0) rows.push_back("status: " + std::to_string(d.status));
    if (d.udhi) rows.push_back("udh: " + d.udh_hex);
    if (d.concat_ref >= 0) {
      rows.push_back("concat: ref=" + std::to_string(d.concat_ref) + " " +
                     std::to_string(d.concat_seq) + "/" + std::to_string(d.concat_total));
    }
    if (d.port_to >= 0) {
      rows.push_back("ports: dst=" + std::to_string(d.port_to) + " src=" +
                     std::to_string(d.port_from));
    }
    PrintStrings(rows);
    return 0;
  }
  if (sub == "watch") {
    return Watch(atof(flags.Get("--interval", "2").c_str()));
  }
  fprintf(stderr, "modem_console: unknown sms subcommand '%s'\n", sub.c_str());
  return 1;
}

int Console::CmdTower(const std::vector<std::string>& positional, const Flags& flags) {
  (void)flags;
  const std::string sub = positional.empty() ? "status" : positional[0];
  if (sub.empty() || sub == "status") {
    return CmdStatus();
  }

  auto report = [&](const std::vector<std::string>& lines, const std::string& success) {
    if (opt_.json) {
      printf("{\"ok\":%s,\"response\":\"%s\"}\n", IsOk(lines) ? "true" : "false",
             JsonEscape(lines.empty() ? "" : lines[0]).c_str());
      return IsOk(lines) ? 0 : 1;
    }
    if (!lines.empty()) {
      for (const auto& line : lines) {
        if (line != "OK") printf("%s\n", line.c_str());
      }
    }
    if (!success.empty()) printf("%s\n", success.c_str());
    return 0;
  };

  if (sub == "register" || sub == "deregister" || sub == "roaming") {
    const std::string state = sub == "register" ? (positional.size() > 1 ? positional[1] : "home")
                                                : sub;
    bool ok = false;
    const int value = ResolveRegistrationState(state, &ok);
    if (!ok) {
      fprintf(stderr, "modem_console: unknown registration state '%s'\n", state.c_str());
      return 1;
    }
    const auto lines = Ask("AT+REMOTEREG:" + std::to_string(value), 1500);
    std::string label = std::to_string(value);
    for (const auto& s : kRegStates) {
      if (s.value == value) label = std::string(s.name) + " - " + s.description;
    }
    return report(lines, "tower: " + label);
  }
  if (sub == "signal") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: tower signal needs a percentage\n");
      return 1;
    }
    const int percent = ParseInt(positional[1], -1);
    if (percent < 0 || percent > 100) {
      fprintf(stderr, "modem_console: signal must be 0-100\n");
      return 1;
    }
    const auto lines = Ask("AT+REMOTESIGNAL:" + std::to_string(percent), 1200);
    return report(lines, "tower: signal strength " + std::to_string(percent) + "%");
  }
  if (sub == "tech") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: tower tech needs a technology\n");
      return 1;
    }
    bool ok = false;
    const int mask = ResolveTech(positional[1], &ok);
    if (!ok || mask == 0) {
      fprintf(stderr, "modem_console: unknown technology '%s'\n", positional[1].c_str());
      return 1;
    }
    const auto lines = Ask("AT+REMOTECTEC: " + std::to_string(mask), 2000);
    return report(lines, "tower: preferred technology " + std::string(TechName(mask)) + " (mask " +
                             std::to_string(mask) + ")");
  }
  if (sub == "power") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: tower power needs on|off\n");
      return 1;
    }
    const std::string what = ToLower(positional[1]);
    if (what != "on" && what != "off") {
      fprintf(stderr, "modem_console: tower power needs on|off\n");
      return 1;
    }
    // +CFUN: 1 = full functionality, 0 = minimum (radio off).
    const std::string value = what == "on" ? "1" : "0";
    const auto lines = Ask("AT+REMOTECFUN=" + value, 1500);
    return report(lines, "tower: radio " + what);
  }
  if (sub == "operator") {
    if (positional.size() < 2) {
      fprintf(stderr, "modem_console: tower operator needs an MCC/MNC or a name\n");
      return 1;
    }
    const auto lines = Ask("AT+REMOTEOPERATOR=" + positional[1], 1500);
    return report(lines, "tower: operator " + positional[1]);
  }
  fprintf(stderr, "modem_console: unknown tower subcommand '%s'\n", sub.c_str());
  return 1;
}

int Console::Watch(double interval_sec) {
  if (interval_sec < 0.5) interval_sec = 0.5;
  PrintStrings({"modem_console: watching the emulated network (^C to stop)"});
  int last_signal = -1, last_reg = -1;
  std::string last_operator;
  for (;;) {
    for (const auto& line : channel_.Collect(0, 100)) {
      if (line.rfind("+REMOTEDELIVER:", 0) == 0) {
        const simsms::DecodedSms d = simsms::Decode(line.substr(strlen("+REMOTEDELIVER:")));
        printf("[%s] in   %s %s  \"%s\"\n", Timestamp().c_str(),
               d.type.empty() ? "SMS" : d.type.c_str(), d.address.c_str(), d.text.c_str());
      } else if (line.rfind("+REMOTESENT:", 0) == 0) {
        const simsms::DecodedSms d = simsms::Decode(line.substr(strlen("+REMOTESENT:")));
        printf("[%s] out  %s to %s  \"%s\"\n", Timestamp().c_str(),
               d.type.empty() ? "SMS" : d.type.c_str(), d.address.c_str(), d.text.c_str());
      } else if (line.rfind("+REMOTESTATUS:", 0) == 0) {
        const simsms::DecodedSms d = simsms::Decode(line.substr(strlen("+REMOTESTATUS:")));
        printf("[%s] report mr=%d %s -> %s\n", Timestamp().c_str(), d.message_reference,
               d.status == 0 ? "delivered" : "failed", d.address.c_str());
      } else if (!line.empty()) {
        printf("[%s] %s\n", Timestamp().c_str(), line.c_str());
      }
    }

    const std::string csq = FirstLineStarting(Ask("AT+CSQ", 400), "+CSQ:");
    if (!csq.empty()) {
      const auto fields = Split(csq.substr(strlen("+CSQ:")), ',');
      const int rssi = ParseInt(fields.empty() ? "" : fields[0], -1);
      if (rssi != last_signal) {
        printf("[%s] tower signal %d/31 (rssi %d dBm)\n", Timestamp().c_str(), rssi,
               rssi >= 0 ? -113 + 2 * rssi : 0);
        last_signal = rssi;
      }
    }
    const std::string creg = FirstLineStarting(Ask("AT+CREG?", 400), "+CREG:");
    if (!creg.empty()) {
      const auto fields = Split(creg.substr(strlen("+CREG:")), ',');
      const int state = ParseInt(fields.size() > 1 ? fields[1] : "", -1);
      if (state != last_reg) {
        std::string label = std::to_string(state);
        for (const auto& s : kRegStates) {
          if (s.value == state) label = std::string(s.name) + " (" + s.description + ")";
        }
        printf("[%s] tower registration: %s\n", Timestamp().c_str(), label.c_str());
        last_reg = state;
      }
    }
    const std::string cops = FirstLineStarting(Ask("AT+COPS?", 400), "+COPS:");
    if (!cops.empty() && cops != last_operator) {
      printf("[%s] tower operator: %s\n", Timestamp().c_str(), cops.c_str());
      last_operator = cops;
    }
    usleep(static_cast<useconds_t>(interval_sec * 1e6));
  }
  return 0;
}

int Console::Listen() {
  PrintStrings({"modem_console: streaming simulator events (^C to stop)"});
  for (;;) {
    for (const auto& line : channel_.Collect(0, 200)) {
      printf("[%s] %s\n", Timestamp().c_str(), line.c_str());
    }
  }
  return 0;
}

std::vector<std::string> Tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    if (!in_quotes && isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    if (c == '\\' && in_quotes && i + 1 < line.size()) {
      cur += line[++i];
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

int Console::RunCommand(std::istringstream& in) {
  std::string line;
  if (!std::getline(in, line)) return 0;
  const auto tokens = Tokenize(line);
  if (tokens.empty()) return 0;
  const std::string command = tokens[0];
  const std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
  const Flags flags(rest);
  const auto positional = flags.Positional();

  if (command == "help" || command == "?") {
    printf("%s", kUsage);
    return 0;
  }
  if (command == "status") return CmdStatus();
  if (command == "identity") return CmdIdentity();
  if (command == "number") return CmdNumber(flags, positional);
  if (command == "sms") return CmdSms(positional, flags);
  if (command == "tower") return CmdTower(positional, flags);
  if (command == "raw") {
    std::string at;
    for (size_t i = 1; i < tokens.size(); ++i) at += std::string(i > 1 ? " " : "") + tokens[i];
    const auto lines = Ask(at, 1500);
    Print(lines);
    return IsOk(lines) ? 0 : 1;
  }
  if (command == "listen") return Listen();
  if (command == "quit" || command == "exit") {
    quit_requested_ = true;
    return 0;
  }
  fprintf(stderr, "modem_console: unknown command '%s' (try help)\n", command.c_str());
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      opt.socket = argv[++i];
    } else if (arg.rfind("--socket=", 0) == 0) {
      opt.socket = arg.substr(strlen("--socket="));
    } else if (arg == "--json") {
      opt.json = true;
    } else if (arg == "--verbose" || arg == "-v") {
      opt.verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      printf("%s", kUsage);
      return 0;
    } else {
      args.push_back(arg);
    }
  }

  Console console(opt);

  if (args.empty()) {
    if (!console.EnsureConnected()) return 1;
    if (isatty(fileno(stdin))) {
      printf("modem_console: connected to the emulated SIM. Type 'help'.\n");
    }
    std::string line;
    while (std::getline(std::cin, line)) {
      std::istringstream one(line);
      console.RunCommand(one);
      if (console.quit_requested()) break;
    }
    return 0;
  }

  std::string joined;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) joined += " ";
    // Re-quote values that contain spaces so the flag parser sees one token.
    joined += args[i].find(' ') == std::string::npos ? args[i] : "\"" + args[i] + "\"";
  }
  std::istringstream in(joined);
  return console.RunCommand(in);
}
