//
// SMS PDU codec for the in-guest modem console. See sms_pdu.h.
//
#include "sms_pdu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace simsms {
namespace {

// GSM 03.38 default alphabet, 16 entries per row.
const char* const kGsm7Basic[128] = {
    // 0x00
    "@", "\u00a3", "$", "\u00a5", "\u00e8", "\u00e9", "\u00f9", "\u00ec", "\u00f2", "\u00c7", "\n", "\u00d8", "\u00f8", "\r", "\u00c5", "\u00e5",
    // 0x10
    "\u0394", "_", "\u03a6", "\u0393", "\u039b", "\u03a9", "\u03a0", "\u03a8", "\u03a3", "\u0398", "\u039e", "\x1b", "\u00c6", "\u00e6", "\u00df", "\u00c9",
    // 0x20
    " ", "!", "\"", "#", "\u00a4", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    // 0x30
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    // 0x40
    "\u00a1", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    // 0x50
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "\u00c4", "\u00d6", "\u00d1", "\u00dc", "\u00a7",
    // 0x60
    "\u00bf", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    // 0x70
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "\u00e4", "\u00f6", "\u00f1", "\u00fc", "\u00e0",
};
static_assert(sizeof(kGsm7Basic) / sizeof(kGsm7Basic[0]) == 128, "GSM 7-bit table size");

struct ExtEntry {
  uint8_t code;
  const char* text;
};

const ExtEntry kGsm7Ext[] = {
    {0x0A, "\f"},     // form feed
    {0x14, "^"},
    {0x28, "{"},
    {0x29, "}"},
    {0x2F, "\\"},
    {0x3C, "["},
    {0x3D, "~"},
    {0x3E, "]"},
    {0x40, "|"},
    {0x65, "\u20ac"},  // euro
};

std::string HexByte(unsigned value) {
  char buf[4];
  snprintf(buf, sizeof(buf), "%02X", value & 0xFF);
  return buf;
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

char HexChar(int v) {
  return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10));
}

size_t Utf8CharLen(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;  // malformed; treat as single byte
}

uint32_t DecodeUtf8(const std::string& s, size_t pos, size_t len) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data() + pos);
  if (len == 1) return p[0];
  if (len == 2) return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
  if (len == 3) return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

// GSM 03.38 packs septets starting at the least significant bit of the first
// octet. `start_bit` lets a UDH occupy the beginning of the user data.
std::string PackSeptets(const std::vector<uint8_t>& septets, size_t start_bit) {
  std::string out;
  size_t bit = start_bit;
  for (uint8_t s : septets) {
    for (int b = 0; b < 7; ++b, ++bit) {
      const size_t byte = bit / 8;
      if (out.size() <= byte) out.resize(byte + 1, '\0');
      if ((s >> b) & 1) out[byte] = static_cast<char>(out[byte] | (1 << (bit % 8)));
    }
  }
  return out;
}

std::vector<uint8_t> UnpackSeptets(const std::string& bytes, size_t start_bit, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  size_t bit = start_bit;
  for (size_t i = 0; i < count; ++i) {
    uint8_t value = 0;
    for (int b = 0; b < 7; ++b, ++bit) {
      const size_t byte = bit / 8;
      if (byte < bytes.size() &&
          ((static_cast<unsigned char>(bytes[byte]) >> (bit % 8)) & 1)) {
        value = static_cast<uint8_t>(value | (1 << b));
      }
    }
    out.push_back(value);
  }
  return out;
}

std::string DigitsOnly(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c >= '0' && c <= '9') out += c;
  }
  return out;
}

struct UdhInfo {
  int concat_ref = -1, concat_total = 0, concat_seq = 0;
  int port_from = -1, port_to = -1;
};

UdhInfo ParseUdh(const std::string& udh) {
  UdhInfo info;
  size_t pos = 0;
  while (pos + 2 <= udh.size()) {
    const int iei = static_cast<unsigned char>(udh[pos]);
    const int len = static_cast<unsigned char>(udh[pos + 1]);
    pos += 2;
    if (pos + static_cast<size_t>(len) > udh.size()) break;
    const auto* d = reinterpret_cast<const unsigned char*>(udh.data() + pos);
    switch (iei) {
      case 0x00:  // concatenated, 8-bit reference
        if (len >= 3) {
          info.concat_ref = d[0];
          info.concat_total = d[1];
          info.concat_seq = d[2];
        }
        break;
      case 0x08:  // concatenated, 16-bit reference
        if (len >= 4) {
          info.concat_ref = (d[0] << 8) | d[1];
          info.concat_total = d[2];
          info.concat_seq = d[3];
        }
        break;
      case 0x04:  // application port addressing, 8-bit
      case 0x05:  // application port addressing, 16-bit
        if (len >= 2) {
          info.port_to = (d[0] << 8) | d[1];
          info.port_from = (len >= 4) ? ((d[2] << 8) | d[3]) : info.port_to;
        }
        break;
      default:
        break;
    }
    pos += len;
  }
  return info;
}

// "240523095341" -> "2305 23 09 53 41" style decoding of the 7 SCTS octets.
std::string DecodeScts(const std::string& octets) {
  if (octets.size() < 14) return "";
  auto pair = [&](size_t i) {
    const int hi = HexVal(octets[i * 2]);
    const int lo = HexVal(octets[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::string("00");
    std::string s;
    if (lo <= 9) s += static_cast<char>('0' + lo);
    if (hi <= 9) s += static_cast<char>('0' + hi);
    return s;
  };
  const std::string yy = pair(0);
  char buf[64];
  const int year = std::atoi(yy.c_str());
  snprintf(buf, sizeof(buf), "%04d-%s-%s %s:%s:%s", year < 70 ? 2000 + year : 1900 + year,
           pair(1).c_str(), pair(2).c_str(), pair(3).c_str(), pair(4).c_str(), pair(5).c_str());
  const int tz_hi = HexVal(octets[12]);
  const int tz_lo = HexVal(octets[13]);
  if (tz_hi >= 0 && tz_lo >= 0) {
    // The time zone is the offset from GMT in quarters of an hour, and bit 3 of
    // the low nibble is its sign - the same layout Android's own PDU decoder
    // uses, so a handset reads these the way they are written here.
    const bool negative = (tz_lo & 0x8) != 0;
    const int quarters = (tz_lo & 0x7) * 10 + tz_hi;
    char tz[32];
    snprintf(tz, sizeof(tz), "%c%02d:%02d", negative ? '-' : '+', quarters / 4,
             (quarters % 4) * 15);
    return std::string(buf) + " " + tz;
  }
  return buf;
}

// Decodes a TP-Address field (starting at its length octet, as hex) and reports
// how many hex characters it consumed.
//
//   [length: 1 octet][TON/NPI: 1 octet][digits: ceil(length/2) octets]
//
std::string DecodeAddress(const std::string& field, size_t* consumed, int* ton_npi,
                          bool* alphanumeric, std::string* error) {
  *consumed = 0;
  if (field.size() < 4) {
    *error = "truncated TP-Address";
    return "";
  }
  const int addr_digits = HexVal(field[0]) * 16 + HexVal(field[1]);
  if (addr_digits < 0) {
    *error = "bad TP-Address length";
    return "";
  }
  const size_t digit_hex = static_cast<size_t>((addr_digits + 1) / 2) * 2;
  if (field.size() < 4 + digit_hex) {
    *error = "truncated TP-Address digits";
    return "";
  }
  *consumed = 4 + digit_hex;
  const int ton = HexVal(field[2]) * 16 + HexVal(field[3]);
  *ton_npi = ton < 0 ? 0x81 : ton;
  const std::string digits = field.substr(4, digit_hex);
  if (ton >= 0 && (ton & 0x70) == 0x50) {  // alphanumeric: septets, not digits
    *alphanumeric = true;
    bool ok = false;
    const std::string packed = FromHex(digits, &ok);
    if (!ok) {
      *error = "bad alphanumeric address";
      return "";
    }
    // The length counts semi-octets; 4 bits each.
    const size_t septets = static_cast<size_t>(addr_digits) * 4 / 7;
    return FromGsm7Septets(UnpackSeptets(packed, 0, septets));
  }
  return SemiOctetsToDigits(digits);
}

}  // namespace

std::string ToHex(const std::string& bytes) {
  char buf[3];
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    snprintf(buf, sizeof(buf), "%02X", c);
    out += buf;
  }
  return out;
}

std::string FromHex(const std::string& hex, bool* ok) {
  if (ok) *ok = true;
  if (hex.size() % 2 != 0) {
    if (ok) *ok = false;
    return {};
  }
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = HexVal(hex[i]), lo = HexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      if (ok) *ok = false;
      return {};
    }
    out += static_cast<char>((hi << 4) | lo);
  }
  return out;
}

bool IsGsm7(const std::string& utf8) {
  size_t pos = 0;
  while (pos < utf8.size()) {
    const size_t len = Utf8CharLen(static_cast<unsigned char>(utf8[pos]));
    const std::string ch = utf8.substr(pos, len);
    bool found = false;
    for (int i = 0; i < 128 && !found; ++i) {
      if (i == 0x1B || kGsm7Basic[i] == nullptr) continue;
      if (ch == kGsm7Basic[i]) found = true;
    }
    for (const auto& ext : kGsm7Ext) {
      if (ch == ext.text) found = true;
    }
    if (!found) return false;
    pos += len;
  }
  return true;
}

std::vector<uint8_t> ToGsm7Septets(const std::string& utf8) {
  std::vector<uint8_t> out;
  size_t pos = 0;
  while (pos < utf8.size()) {
    const size_t len = Utf8CharLen(static_cast<unsigned char>(utf8[pos]));
    const std::string ch = utf8.substr(pos, len);
    bool found = false;
    for (int i = 0; i < 128 && !found; ++i) {
      if (i == 0x1B || kGsm7Basic[i] == nullptr) continue;
      if (ch == kGsm7Basic[i]) {
        out.push_back(static_cast<uint8_t>(i));
        found = true;
      }
    }
    if (!found) {
      for (const auto& ext : kGsm7Ext) {
        if (ch == ext.text) {
          out.push_back(0x1B);
          out.push_back(ext.code);
          found = true;
          break;
        }
      }
    }
    if (!found) return {};  // not representable
    pos += len;
  }
  return out;
}

std::string FromGsm7Septets(const std::vector<uint8_t>& septets) {
  std::string out;
  for (size_t i = 0; i < septets.size(); ++i) {
    const uint8_t s = septets[i];
    if (s == 0x1B) {
      if (i + 1 < septets.size()) {
        const uint8_t esc = septets[++i];
        for (const auto& ext : kGsm7Ext) {
          if (ext.code == esc) {
            out += ext.text;
            break;
          }
        }
      }
      continue;
    }
    if (s < 128 && kGsm7Basic[s] != nullptr) out += kGsm7Basic[s];
  }
  return out;
}

std::string Utf8ToUcs2Hex(const std::string& utf8) {
  std::string out;
  size_t pos = 0;
  while (pos < utf8.size()) {
    const size_t len = Utf8CharLen(static_cast<unsigned char>(utf8[pos]));
    const uint32_t cp = DecodeUtf8(utf8, pos, len);
    if (cp <= 0xFFFF) {
      out += HexByte(cp >> 8);
      out += HexByte(cp & 0xFF);
    } else {
      const uint32_t v = cp - 0x10000;
      const uint32_t hi = 0xD800 + (v >> 10);
      const uint32_t lo = 0xDC00 + (v & 0x3FF);
      out += HexByte(hi >> 8);
      out += HexByte(hi & 0xFF);
      out += HexByte(lo >> 8);
      out += HexByte(lo & 0xFF);
    }
    pos += len;
  }
  return out;
}

std::string Ucs2HexToUtf8(const std::string& hex) {
  std::string out;
  auto emit = [&](uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  };
  for (size_t i = 0; i + 3 < hex.size(); i += 4) {
    const int a = HexVal(hex[i]), b = HexVal(hex[i + 1]), c = HexVal(hex[i + 2]), d = HexVal(hex[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) break;
    uint32_t cp = (a << 12) | (b << 8) | (c << 4) | d;
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 7 < hex.size()) {
      const int e = HexVal(hex[i + 4]), f = HexVal(hex[i + 5]), g = HexVal(hex[i + 6]), h = HexVal(hex[i + 7]);
      if (e >= 0 && f >= 0 && g >= 0 && h >= 0) {
        const uint32_t lo = (e << 12) | (f << 8) | (g << 4) | h;
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 4;
        }
      }
    }
    emit(cp);
  }
  return out;
}

std::string DigitsToSemiOctets(const std::string& digits) {
  std::string out;
  for (size_t i = 0; i < digits.size(); i += 2) {
    const char hi = digits[i];
    const char lo = (i + 1 < digits.size()) ? digits[i + 1] : 'F';
    out += lo;
    out += hi;
  }
  return out;
}

std::string SemiOctetsToDigits(const std::string& semi_octets) {
  std::string out;
  for (size_t i = 0; i + 1 < semi_octets.size(); i += 2) {
    const char lo = semi_octets[i];
    const char hi = semi_octets[i + 1];
    if (hi != 'F' && hi != 'f') out += hi;
    if (lo != 'F' && lo != 'f') out += lo;
  }
  return out;
}

namespace {

std::string EncodeScts(std::time_t when) {
  struct tm tmv {};
  localtime_r(&when, &tmv);
  const long offset_minutes = tmv.tm_gmtoff / 60;
  const bool negative = offset_minutes < 0;
  const long abs_minutes = negative ? -offset_minutes : offset_minutes;
  // The time zone is the offset in quarters of an hour, as two BCD digits.
  unsigned quarters = static_cast<unsigned>(abs_minutes / 15);
  if (quarters > 99) quarters = 99;  // cannot happen; keep the field well formed
  const unsigned year = static_cast<unsigned>(((tmv.tm_year % 100) + 100) % 100);
  const unsigned month = static_cast<unsigned>(((tmv.tm_mon % 12) + 12) % 12 + 1);
  const unsigned mday = static_cast<unsigned>(tmv.tm_mday);
  const unsigned hour = static_cast<unsigned>(tmv.tm_hour);
  const unsigned minute = static_cast<unsigned>(tmv.tm_min);
  const unsigned second = static_cast<unsigned>(tmv.tm_sec);
  char digits[32];
  snprintf(digits, sizeof(digits), "%02u%02u%02u%02u%02u%02u", year, month, mday, hour, minute,
           second);
  char tz_digits[16];
  snprintf(tz_digits, sizeof(tz_digits), "%02u", quarters);
  std::string tz = DigitsToSemiOctets(tz_digits);
  if (negative && tz.size() == 2) {
    tz[1] = HexChar(HexVal(tz[1]) | 0x8);  // sign bit: bit 3 of the low nibble
  }
  // Both halves are semi-octet swapped, as every other BCD field is.
  return DigitsToSemiOctets(digits) + tz;
}

std::string EncodeSmsc(const std::string& smsc) {
  const std::string digits = DigitsOnly(smsc);
  if (digits.empty()) return "00";
  const int ton_npi = (smsc.find('+') != std::string::npos) ? 0x91 : 0x81;
  const std::string semi = DigitsToSemiOctets(digits);
  return HexByte(1 + semi.size() / 2) + HexByte(ton_npi) + semi;
}

}  // namespace

std::string BuildDeliver(const SmsDeliver& in, std::string* err) {
  auto fail = [&](const std::string& msg) {
    if (err) *err = msg;
    return std::string();
  };

  // ---- user data encoding -------------------------------------------------
  enum class Enc { Gsm7, Ucs2, EightBit };
  Enc enc = Enc::Gsm7;
  if (in.eight_bit) {
    enc = Enc::EightBit;
  } else if (in.ucs2) {
    enc = Enc::Ucs2;
  } else if (in.use_dcs) {
    const int family = in.dcs & 0x0C;
    if (family == 0x08) enc = Enc::Ucs2;
    else if (family == 0x04) enc = Enc::EightBit;
  } else if (!IsGsm7(in.text)) {
    enc = Enc::Ucs2;
  }

  // `udh` is the raw user data header, length octet included. It is octet
  // aligned at the start of TP-UD, not septet packed like the text after it.
  std::string udh;
  if (!in.udh_hex.empty()) {
    bool ok = false;
    const std::string raw = FromHex(in.udh_hex, &ok);
    if (!ok) return fail("--udh-hex must be an even number of hex digits");
    if (raw.size() > 140) return fail("UDH is longer than one message can hold");
    udh = std::string(1, static_cast<char>(raw.size())) + raw;
  }
  const bool udhi = !udh.empty();

  std::string ud;
  int udl = 0;
  if (enc == Enc::Gsm7) {
    const auto septets = ToGsm7Septets(in.text);
    if (septets.empty() && !in.text.empty()) {
      return fail("text is not representable in the GSM 7-bit alphabet");
    }
    // The UDH takes whole septets of the user data; the text starts at the next
    // septet boundary, with any leftover bits left zero.
    const size_t udh_septets = udhi ? (udh.size() * 8 + 6) / 7 : 0;
    if (udh_septets + septets.size() > 160) {
      return fail("message too long: " + std::to_string(septets.size()) + " septets (max " +
                  std::to_string(160 - udh_septets) + (udhi ? " with this UDH" : "") +
                  "); split it into a concatenated message with --concat");
    }
    udl = static_cast<int>(udh_septets + septets.size());
    ud = PackSeptets(septets, udh_septets * 7);
    if (udhi) {
      // Place the UDH octets at the start, untouched by septet packing.
      if (ud.size() < udh.size()) ud.resize(udh.size(), '\0');
      for (size_t i = 0; i < udh.size(); ++i) ud[i] = udh[i];
    }
  } else {
    std::string payload;
    if (enc == Enc::Ucs2) {
      payload = FromHex(Utf8ToUcs2Hex(in.text), nullptr);
    } else {
      bool ok = false;
      payload = FromHex(in.data_hex, &ok);
      if (!ok) return fail("--data-hex must be an even number of hex digits");
    }
    const size_t total = udh.size() + payload.size();
    if (total > 140) {
      return fail("message too long: " + std::to_string(total) +
                  " octets (max 140); split it into a concatenated message");
    }
    udl = static_cast<int>(total);
    ud = udh + payload;
  }

  // ---- data coding scheme -------------------------------------------------
  int dcs;
  if (in.use_dcs) {
    dcs = in.dcs;
    if (in.msg_class >= 0 && (dcs & 0x10) == 0) dcs |= 0x10 | (in.msg_class & 0x03);
  } else if (enc == Enc::Ucs2) {
    dcs = 0x08;
  } else if (enc == Enc::EightBit) {
    dcs = 0x04;
  } else {
    dcs = 0x00;
  }
  if (in.msg_class >= 0 && !in.use_dcs) dcs |= 0x10 | (in.msg_class & 0x03);

  // ---- addresses ----------------------------------------------------------
  std::string sender;
  if (in.sender_alphanumeric) {
    const auto septets = ToGsm7Septets(in.sender);
    if (septets.empty() && !in.sender.empty()) {
      return fail("alphanumeric sender is not representable in the GSM 7-bit alphabet");
    }
    const std::string packed = PackSeptets(septets, 0);
    const int ton = in.ton >= 0 ? in.ton : 0xD0;
    sender = HexByte(packed.size() * 2) + HexByte(ton) + ToHex(packed);
  } else {
    const std::string digits = DigitsOnly(in.sender);
    if (digits.empty()) return fail("sender number is required (or use an alphanumeric sender)");
    if (digits.size() > 20) return fail("sender number is too long");
    const int ton = in.ton >= 0 ? in.ton
                                : (in.sender.find('+') != std::string::npos ? 0x91 : 0x81);
    sender = HexByte(digits.size()) + HexByte(ton) + DigitsToSemiOctets(digits);
  }

  // ---- first octet --------------------------------------------------------
  int fo = 0x00;  // TP-MTI = SMS-DELIVER
  if (in.more_messages) fo |= 0x04;
  if (in.reply_path) fo |= 0x08;
  if (udhi) fo |= 0x40;
  if (in.status_report_request) fo |= 0x20;

  std::string pdu = EncodeSmsc(in.smsc);
  pdu += HexByte(fo);
  pdu += sender;
  pdu += HexByte(in.pid & 0xFF);
  pdu += HexByte(dcs);
  pdu += EncodeScts(in.scts > 0 ? in.scts : std::time(nullptr));
  pdu += HexByte(udl);
  pdu += ToHex(ud);

  if (err) err->clear();
  return pdu;
}

DecodedSms Decode(const std::string& pdu_hex) {
  DecodedSms out;
  out.pdu = pdu_hex;
  if (pdu_hex.size() < 4 || pdu_hex.size() % 2 != 0) {
    out.error = "PDU must be an even number of hex digits";
    return out;
  }
  for (char c : pdu_hex) {
    if (HexVal(c) < 0) {
      out.error = "PDU contains non-hex characters";
      return out;
    }
  }
  for (auto& c : out.pdu) c = static_cast<char>(toupper(c));

  size_t pos = 0;

  // Every remaining field is read through this so a truncated or non-hex PDU
  // reports an error instead of running off the end of the string.
  auto read_byte = [&](int* value) {
    if (pos + 2 > out.pdu.size()) {
      out.error = "truncated PDU";
      return false;
    }
    const int hi = HexVal(out.pdu[pos]), lo = HexVal(out.pdu[pos + 1]);
    pos += 2;
    if (hi < 0 || lo < 0) {
      out.error = "non-hex character in PDU";
      return false;
    }
    *value = hi * 16 + lo;
    return true;
  };
  // SMSC
  const int smsc_len = HexVal(out.pdu[0]) * 16 + HexVal(out.pdu[1]);
  pos = 2;
  if (smsc_len > 0) {
    if (pos + static_cast<size_t>(smsc_len) * 2 > out.pdu.size()) {
      out.error = "truncated SMSC field";
      return out;
    }
    const std::string smsc = out.pdu.substr(pos, smsc_len * 2);
    if (smsc.size() >= 2) out.smsc = SemiOctetsToDigits(smsc.substr(2));
    pos += smsc_len * 2;
  }
  if (pos + 2 > out.pdu.size()) {
    out.error = "truncated first octet";
    return out;
  }
  int fo = 0;
  if (!read_byte(&fo)) return out;
  const int mti = fo & 0x03;

  std::string error;
  std::string address;
  if (mti == 0x00) {  // SMS-DELIVER
    out.type = "SMS-DELIVER";
    size_t consumed = 0;
    int ton = 0;
    address = DecodeAddress(out.pdu.substr(pos), &consumed, &ton, &out.address_alphanumeric, &error);
    if (!error.empty()) {
      out.error = error;
      return out;
    }
    pos += consumed;
  } else if (mti == 0x01) {  // SMS-SUBMIT
    out.type = "SMS-SUBMIT";
    if (!read_byte(&out.message_reference)) return out;
    size_t consumed = 0;
    int ton = 0;
    address = DecodeAddress(out.pdu.substr(pos), &consumed, &ton, &out.address_alphanumeric, &error);
    if (!error.empty()) {
      out.error = error;
      return out;
    }
    pos += consumed;
  } else if (mti == 0x02) {  // SMS-STATUS-REPORT
    out.type = "SMS-STATUS-REPORT";
    if (!read_byte(&out.message_reference)) return out;
    size_t consumed = 0;
    int ton = 0;
    address = DecodeAddress(out.pdu.substr(pos), &consumed, &ton, &out.address_alphanumeric, &error);
    if (!error.empty()) {
      out.error = error;
      return out;
    }
    pos += consumed;
    out.scts = DecodeScts(out.pdu.substr(pos, 14));
    pos += 14;
    out.discharge_time = DecodeScts(out.pdu.substr(pos, 14));
    pos += 14;
    if (!read_byte(&out.status)) return out;
  } else {
    out.error = "unsupported TP-MTI " + std::to_string(mti);
    return out;
  }
  out.address = address;

  // SMS-SUBMIT carries PID, DCS, then the validity period; SMS-DELIVER carries
  // PID, DCS, then the service centre time stamp (which we read below).
  if (!read_byte(&out.pid)) return out;
  if (!read_byte(&out.dcs)) return out;

  if (mti == 0x01) {
    const int vpf = (fo >> 3) & 0x03;
    if (vpf == 0x01 || vpf == 0x02) pos += 2;
    else if (vpf == 0x03) pos += 14;
  }

  if (mti == 0x00) {  // deliver carries SCTS after the DCS
    out.scts = DecodeScts(out.pdu.substr(pos, 14));
    pos += 14;
  }

  if (!read_byte(&out.udl)) return out;
  const std::string ud_hex = out.pdu.substr(pos);
  bool ok = false;
  const std::string ud = FromHex(ud_hex, &ok);
  out.user_data_hex = ud_hex;

  out.udhi = (fo & 0x40) != 0;
  size_t udh_octets = 0;
  if (out.udhi && !ud.empty()) {
    udh_octets = 1 + static_cast<unsigned char>(ud[0]);
    if (udh_octets > ud.size()) {
      out.error = "truncated UDH";
      return out;
    }
    const std::string udh = ud.substr(1, static_cast<unsigned char>(ud[0]));
    out.udh_hex = ToHex(udh);
    const UdhInfo info = ParseUdh(udh);
    out.concat_ref = info.concat_ref;
    out.concat_total = info.concat_total;
    out.concat_seq = info.concat_seq;
    out.port_from = info.port_from;
    out.port_to = info.port_to;
  }

  const int family = out.dcs & 0x0C;
  if (family == 0x00) {  // GSM 7-bit
    const int text_start = static_cast<int>((udh_octets * 8 + 6) / 7);
    const int text_septets = out.udl - (out.udhi ? text_start : 0);
    if (text_septets > 0) {
      out.text = FromGsm7Septets(UnpackSeptets(ud, text_start * 7, text_septets));
    }
  } else if ((out.dcs & 0x08) && (family == 0x08)) {  // UCS2
    out.text = Ucs2HexToUtf8(ud_hex.substr(udh_octets * 2));
  } else {
    out.text = ud_hex.substr(udh_octets * 2);  // 8-bit: raw hex
  }

  out.ok = true;
  return out;
}

}  // namespace simsms
