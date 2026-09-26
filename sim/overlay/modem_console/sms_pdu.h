//
// SMS PDU codec for the in-guest modem console.
//
// Builds and parses TPDUs the way a real network does, so injected messages are
// indistinguishable from ones that arrived over the air:
//
//   * GSM 03.38 default alphabet, including the escape (extension) table, with
//     automatic fallback to UCS2 for text the alphabet cannot carry.
//   * SMSC address prefix (always present on the air, unlike Cuttlefish's own
//     encoder which drops it), semi-octet BCD with TON/NPI.
//   * Service centre time stamp with the local time zone and sign.
//   * UDH: concatenation (8- and 16-bit reference) and application ports, for
//     multipart SMS and binary (WAP push / OTA) payloads.
//   * SMS-DELIVER, plus a decoder for SMS-SUBMIT and SMS-STATUS-REPORT so the
//     console can show what the device transmits.
//
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace simsms {

// --- hex helpers ------------------------------------------------------------

std::string ToHex(const std::string& bytes);
std::string FromHex(const std::string& hex, bool* ok);

// --- alphabet ---------------------------------------------------------------

// True if every character can be represented in the GSM 7-bit default alphabet
// (with escape sequences) - i.e. a real handset would send this as 7-bit.
bool IsGsm7(const std::string& utf8);

// Septets including 0x1B escape prefixes; empty if the text is not GSM 7-bit.
std::vector<uint8_t> ToGsm7Septets(const std::string& utf8);
std::string FromGsm7Septets(const std::vector<uint8_t>& septets);

std::string Utf8ToUcs2Hex(const std::string& utf8);
std::string Ucs2HexToUtf8(const std::string& hex);

// Semi-octet BCD: "15551234567" -> "5155214365F7".
std::string DigitsToSemiOctets(const std::string& digits);
std::string SemiOctetsToDigits(const std::string& semi_octets);

// --- building ---------------------------------------------------------------

struct SmsDeliver {
  // Service centre. Empty means "no SMSC address" (TPDU length 00), which is
  // what a handset writes and what some networks accept, but a real delivery
  // carries the SMSC that relayed it. "auto" (the console default) resolves it
  // from the SIM's own AT+CSCA.
  std::string smsc;

  std::string sender;              // MSISDN, or text when alphanumeric
  bool sender_alphanumeric = false;
  int ton = -1;                    // -1 = derive from sender ("+" -> 0x91)
  std::string text;

  uint8_t pid = 0;
  bool use_dcs = false;            // force `dcs` instead of deriving it
  uint8_t dcs = 0;
  int msg_class = -1;              // 0..3 when set

  bool ucs2 = false;               // force UCS2
  bool eight_bit = false;          // force 8-bit; payload is `data_hex`
  std::string data_hex;

  std::string udh_hex;             // UDH bytes (without the length octet)
  bool more_messages = false;      // TP-MMS
  bool reply_path = false;         // TP-RP
  bool status_report_request = false;  // TP-SRI

  std::time_t scts = 0;            // 0 = now (local time)
};

// Full PDU as uppercase hex, SMSC prefix included (exactly what +CMT carries).
std::string BuildDeliver(const SmsDeliver& in, std::string* err);

// --- parsing ----------------------------------------------------------------

struct DecodedSms {
  bool ok = false;
  std::string error;

  std::string type;                // SMS-DELIVER / SMS-SUBMIT / SMS-STATUS-REPORT
  std::string smsc;
  std::string address;             // originator (deliver) or destination (submit)
  bool address_alphanumeric = false;
  std::string text;
  std::string scts;                // human readable, or empty
  std::string discharge_time;      // status reports only
  int message_reference = -1;
  int status = -1;                 // status reports only
  int pid = -1;
  int dcs = -1;
  bool udhi = false;
  std::string udh_hex;
  int concat_ref = -1, concat_total = 0, concat_seq = 0;
  int port_from = -1, port_to = -1;
  int udl = 0;
  std::string pdu;                 // the input, normalised to uppercase
  std::string user_data_hex;       // raw TP-UD, for `--8bit` payloads
};

DecodedSms Decode(const std::string& pdu_hex);

}  // namespace simsms
