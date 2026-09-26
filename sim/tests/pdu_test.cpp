//
// Host-side test for the modem console's SMS PDU codec.
//
// Two things are checked, and both matter for "the device cannot tell this is
// emulated":
//
//  1. PDUs the console *generates* are accepted by the exact parser the in-guest
//     modem simulator runs (device/google/cuttlefish .../pdu_parser.cpp) before
//     it will deliver them to the RIL. That parser does strict length
//     arithmetic, so this catches septet-packing and UDH padding mistakes that a
//     round-trip through our own decoder would happily hide.
//
//  2. Vectors from AOSP's own PDU parser tests decode to the expected values.
//
// Run with sim/run-host-tests.sh.
//
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "host/commands/modem_simulator/pdu_parser.h"
#include "sms_pdu.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("FAIL: %s\n", what.c_str());
  }
}

template <typename T>
void CheckEqual(const T& actual, const T& expected, const std::string& what) {
  ++g_checks;
  if (!(actual == expected)) {
    ++g_failures;
    printf("FAIL: %s\n", what.c_str());
  }
}

// The simulator's own validation, run exactly as it runs in the guest.
bool SimulatorAccepts(const std::string& pdu, std::string* why) {
  std::string mutable_pdu = pdu;
  cuttlefish::PDUParser parser(mutable_pdu);
  if (!parser.IsValidPDU()) {
    if (why) *why = "PDUParser::IsValidPDU() rejected it";
    return false;
  }
  return true;
}

void ExpectDeliverable(const std::string& pdu, const std::string& label) {
  std::string why;
  // Note: the call must happen before the message is built, or `why` is empty.
  const bool accepted = SimulatorAccepts(pdu, &why);
  Check(accepted, label + ": the simulator accepts the PDU (" + why + ")");
}

// ---------------------------------------------------------------------------

void TestAospVector() {
  // From device/google/cuttlefish .../unittest/pdu_parser_test.cpp.
  const std::string pdu =
      "0001000D91688118109844F0000017AFD7903AB55A9BBA69D639D4ADCBF99E3DCCAE9701";
  const simsms::DecodedSms d = simsms::Decode(pdu);
  Check(d.ok, "AOSP SUBMIT vector decodes");
  CheckEqual(d.type, std::string("SMS-SUBMIT"), "AOSP SUBMIT vector: type");
  // AOSP's helper drops the leading country code (86) because it only cares
  // about the CVD port suffix; a correct decoder returns the whole address.
  CheckEqual(d.address, std::string("8618810189440"), "AOSP SUBMIT vector: address");
  CheckEqual(d.message_reference, 0, "AOSP SUBMIT vector: TP-MR");
  printf("     AOSP SUBMIT vector decodes to text: \"%s\" (scts %s, dcs 0x%02X)\n",
         d.text.c_str(), d.scts.empty() ? "-" : d.scts.c_str(), d.dcs);

  // The matching UCS2 vector: destination 5445 (a Cuttlefish "port" number),
  // one character of text.
  const simsms::DecodedSms u = simsms::Decode("000100048145540008024F60");
  Check(u.ok, "AOSP UCS2 vector decodes");
  CheckEqual(u.address, std::string("5445"), "AOSP UCS2 vector: address");
  CheckEqual(u.dcs, 0x08, "AOSP UCS2 vector: DCS");
  CheckEqual(u.text, std::string("\u4f60"), "AOSP UCS2 vector: text");
}

void TestRejectsCorrupt() {
  // AOSP expects this one to be invalid (0xFD instead of 0x0D). If our
  // expectations were inverted, everything else in this file would pass
  // vacuously, so this guards the harness.
  std::string bad = "000100fD91688118109844F0000017AFD7903AB55A9BBA69D639D4ADCBF99E3DCCAE9701";
  cuttlefish::PDUParser parser(bad);
  Check(!parser.IsValidPDU(), "corrupt PDU is rejected by the simulator parser");
  Check(!simsms::Decode(bad).ok, "corrupt PDU is rejected by our decoder");
}

void TestPlainGsm7() {
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  in.text = "Your verification code is 123456";
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "plain 7-bit PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "plain 7-bit");

  const simsms::DecodedSms d = simsms::Decode(pdu);
  Check(d.ok, "plain 7-bit PDU decodes");
  CheckEqual(d.smsc, std::string("15551234567"), "plain 7-bit: SMSC preserved");
  CheckEqual(d.address, std::string("15550001111"), "plain 7-bit: sender");
  CheckEqual(d.text, in.text, "plain 7-bit: text");
  CheckEqual(d.dcs, 0x00, "plain 7-bit: DCS is the general 7-bit alphabet");
}

void TestGsm7Specials() {
  // The escape table: these characters cost two septets and are the ones most
  // likely to be mis-packed.
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  in.text = "{}[]|^~\\ and \u20ac20 fare";
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "escape-table PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "escape-table");
  const simsms::DecodedSms d = simsms::Decode(pdu);
  CheckEqual(d.text, in.text, "escape-table text round-trips");
}

void TestUcs2Fallback() {
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  in.text = "\u041f\u0440\u0438\u0432\u0435\u0442 \u4e16\u754c \U0001f4f1";
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "UCS2 PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "UCS2");
  const simsms::DecodedSms d = simsms::Decode(pdu);
  CheckEqual(d.dcs, 0x08, "UCS2 selected automatically for non-GSM7 text");
  CheckEqual(d.text, in.text, "UCS2 text round-trips (incl. emoji)");

  // Forcing GSM7 must refuse rather than silently mangle the text.
  simsms::SmsDeliver forced = in;
  Check(!simsms::IsGsm7(forced.text), "IsGsm7 reports UCS2-only text");
}

void TestMultipartAndUdh() {
  // Concatenation, 8-bit reference: UDH 05 00 03 <ref> <total> <seq>.
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  in.text = "Part two of a multipart message";
  in.udh_hex = "0003010202";  // IEI 00, len 03, ref 01, total 02, seq 02
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "multipart PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "multipart with 7-bit UDH");
  const simsms::DecodedSms d = simsms::Decode(pdu);
  Check(d.udhi, "multipart: UDHI set");
  CheckEqual(d.udh_hex, std::string("0003010202"), "multipart: UDH preserved");
  CheckEqual(d.concat_ref, 1, "multipart: reference");
  CheckEqual(d.concat_seq, 2, "multipart: sequence");
  CheckEqual(d.concat_total, 2, "multipart: total");
  CheckEqual(d.text, in.text, "multipart: text is not shifted by the UDH");

  // 16-bit reference variant.
  simsms::SmsDeliver wide = in;
  wide.udh_hex = "0804ABCD0301";
  const std::string wide_pdu = simsms::BuildDeliver(wide, &err);
  Check(!wide_pdu.empty(), "16-bit reference PDU builds (" + err + ")");
  ExpectDeliverable(wide_pdu, "multipart with 16-bit reference");
  const simsms::DecodedSms w = simsms::Decode(wide_pdu);
  CheckEqual(w.concat_ref, 0xABCD, "multipart: 16-bit reference");
  CheckEqual(w.text, wide.text, "multipart: 16-bit reference text");
}

void TestBinaryPortPush() {
  // What a WAP push (or a carrier OTA configuration message) looks like: 8-bit
  // payload addressed to an application port.
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  in.eight_bit = true;
  in.data_hex = "0B05040B8423F000030100";
  in.udh_hex = "05040B8423F0";  // ports: dst 0B84, src 23F0
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "binary WAP push PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "binary WAP push");
  const simsms::DecodedSms d = simsms::Decode(pdu);
  CheckEqual(d.dcs, 0x04, "WAP push uses the 8-bit data coding scheme");
  CheckEqual(d.port_to, 0x0B84, "WAP push: destination port");
  CheckEqual(d.port_from, 0x23F0, "WAP push: source port");
  Check(d.text.rfind("0B05040B8423F000030100", 0) == 0, "WAP push: payload preserved");
}

void TestAlphanumericSender() {
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "AndroidBank";
  in.sender_alphanumeric = true;
  in.text = "Balance: 42.00";
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "alphanumeric sender PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "alphanumeric sender");
  const simsms::DecodedSms d = simsms::Decode(pdu);
  Check(d.address_alphanumeric, "alphanumeric sender detected");
  CheckEqual(d.address, in.sender, "alphanumeric sender round-trips");
  CheckEqual(d.text, in.text, "alphanumeric sender: text");
}

void TestTimestampAndClass() {
  // 2023-05-23 09:53:41 UTC, delivered as a flash (class 0) message.
  const std::time_t when = 1684835621;
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  in.text = "flash";
  in.scts = when;
  in.msg_class = 0;
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "class-0 PDU builds (" + err + ")");
  ExpectDeliverable(pdu, "class 0 with explicit timestamp");
  const simsms::DecodedSms d = simsms::Decode(pdu);
  Check((d.dcs & 0x10) != 0, "class bit present in DCS");
  CheckEqual(d.dcs & 0x03, 0, "class 0 encoded");
  Check(d.scts.rfind("2023-05-23 09:53:41", 0) == 0,
        "timestamp round-trips (" + d.scts + ")");
}

void TestLimits() {
  simsms::SmsDeliver in;
  in.smsc = "+15551234567";
  in.sender = "+15550001111";
  std::string err;

  in.text = std::string(160, 'a');  // exactly one 7-bit message
  Check(!simsms::BuildDeliver(in, &err).empty(), "160 septets fits in one part");
  in.text = std::string(161, 'a');
  Check(simsms::BuildDeliver(in, &err).empty(), "161 septets is refused");
  Check(err.find("too long") != std::string::npos, "over-length error explains itself");

  // 140 octets is the ceiling for UCS2/8-bit payloads.
  in.text = std::string(70, 'a');
  Check(!simsms::BuildDeliver(in, &err).empty(), "70 UCS2 characters fit");
}

void TestOmittingSmsc() {
  simsms::SmsDeliver in;
  in.sender = "+15550001111";
  in.text = "no service centre";
  std::string err;
  const std::string pdu = simsms::BuildDeliver(in, &err);
  Check(!pdu.empty(), "PDU without SMSC builds");
  Check(pdu.rfind("00", 0) == 0, "no-SMSC PDU starts with a zero length SMSC");
  ExpectDeliverable(pdu, "no SMSC");
}

}  // namespace

int main() {
  TestAospVector();
  TestRejectsCorrupt();
  TestPlainGsm7();
  TestGsm7Specials();
  TestUcs2Fallback();
  TestMultipartAndUdh();
  TestBinaryPortPush();
  TestAlphanumericSender();
  TestTimestampAndClass();
  TestLimits();
  TestOmittingSmsc();

  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
