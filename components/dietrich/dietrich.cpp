#include "dietrich.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace dietrich {

static const char *const TAG = "dietrich";

// Remeha protocol (protocol.nr 1 in Recom's DeviceConfiguration.xml), used by
// both the MCR3 and the PCU-05:
//
//   02 | SRC | DEST | TYPE | LEN | COMMAND | EXTCMD | data.. | CRC-lo CRC-hi | 03
//
// Byte 1 is the sender and byte 2 the recipient (PC 0xFE, PCU 0x01, PSU 0x00);
// the two come back swapped in the response. Byte 3 is the message type: 0x05 in
// a request, 0x06 in a response - Recom's IsAcknowledged() is exactly that byte
// == 6, so an ACK is nothing more than an ordinary response frame. Byte 4 is
// (frame length - 2), so a 10 byte request carries 0x08 there. Both directions
// use the same 7 byte header, so the data block starts at index 7 and runs for
// (frame length - 10) bytes. COMMAND 0x02 is SAMPLES (EXTCMD 0x01 selects the
// sample format) and COMMAND 0x10 is READ_EPROM_BLOCK, where EXTCMD is a 16 byte
// EEPROM block index - that is what the two counter requests below really are.
// See mapping/pcu05_p3_protocol.md for the full command set, recovered from
// Recom's own RemehaMessageFactory/RemehaReceiver and checked byte-for-byte
// against a live PCU-05 P3 response.
static const uint8_t CMD_SAMPLE_MCR3[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x02, 0x01, 0x69, 0xAB, 0x03};
static const uint8_t CMD_COUNTER1_MCR3[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x1C, 0x98, 0xC2, 0x03};
static const uint8_t CMD_COUNTER2_MCR3[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x1D, 0x59, 0x02, 0x03};

// Parameter block reads: COMMAND 0x10 (READ_EPROM_BLOCK) with the EEPROM block
// index in the EXTCMD byte. Blocks 0x14..0x1B are the 128 byte parameter block,
// 16 bytes per reply. Byte 2 is 0x00 to match the counter reads, which are the
// same command against blocks 0x1C/0x1D and are known to work on this bus. Note
// 0x00 is nominally the PSU's address and 0x01 the PCU's: the board answers on
// either, echoing back whichever it was sent. Writes should still use 0x01, which
// is what Recom does.
static const uint8_t CMD_PARAM_REMEHA[8][10] = {
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x14, 0x99, 0x04, 0x03},  // bytes   0..15
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x15, 0x58, 0xC4, 0x03},  // bytes  16..31
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x16, 0x18, 0xC5, 0x03},  // bytes  32..47
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x17, 0xD9, 0x05, 0x03},  // bytes  48..63
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x18, 0x99, 0x01, 0x03},  // bytes  64..79
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x19, 0x58, 0xC1, 0x03},  // bytes  80..95
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x1A, 0x18, 0xC0, 0x03},  // bytes  96..111
    {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x1B, 0xD9, 0x00, 0x03},  // bytes 112..127
};

// Service mode, from RemehaBoilerController.EnableServiceMode: a bare 10 byte
// frame with no payload. COMMAND 0x08 (CODE_SERVICE_START) unlocks writing and
// 0x1F (CODE_SERVICE_STOP) re-locks it, both with EXTCMD 0x0C (CODE_SERVICE).
// No service code is sent - the 0012 PIN Recom asks for is an application-level
// gate only, and nothing in the write path touches SERVICE_CODE (0x37).
// Addressed to the PCU at 0x01, which is what Recom does for the write path;
// reads get away with 0x00 but there is no reason to risk it here.
static const uint8_t CMD_SERVICE_ON_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x08, 0x0C, 0xAE, 0xCE, 0x03};
static const uint8_t CMD_SERVICE_OFF_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x1F, 0x0C, 0xA1, 0x3E, 0x03};

// The same eight reads addressed to the PCU (0x01) instead of 0x00, which is how
// Recom sends them. Used only inside a write transaction: everything Recom does
// on the write path - unlock, read, write, re-lock - is addressed to one device,
// and a frame aimed at a different address in the middle of that sequence is one
// of the few remaining ways this component still differs from it. Polling keeps
// using the 0x00 table above, which is known to work on this bus.
static const uint8_t CMD_PARAM_REMEHA_PCU[8][10] = {
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x14, 0xA4, 0xC4, 0x03},  // bytes   0..15
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x15, 0x65, 0x04, 0x03},  // bytes  16..31
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x16, 0x25, 0x05, 0x03},  // bytes  32..47
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x17, 0xE4, 0xC5, 0x03},  // bytes  48..63
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x18, 0xA4, 0xC1, 0x03},  // bytes  64..79
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x19, 0x65, 0x01, 0x03},  // bytes  80..95
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x1A, 0x25, 0x00, 0x03},  // bytes  96..111
    {0x02, 0xFE, 0x01, 0x05, 0x08, 0x10, 0x1B, 0xE4, 0xC0, 0x03},  // bytes 112..127
};

// Avanta protocol (protocol.nr 2), XOR checksum, 6 byte response header
static const uint8_t CMD_SAMPLE_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x02, 0x00, 0x53, 0x03};
static const uint8_t CMD_COUNTER1_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x10, 0x01, 0x40, 0x03};
static const uint8_t CMD_COUNTER2_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x10, 0x02, 0x43, 0x03};

// byte 3 of a Remeha frame: 0x05 in a request, 0x06 in a response
static const uint8_t REMEHA_TYPE_RESPONSE = 0x06;
// STX + 6 header bytes + CRC16 + ETX
static const size_t REMEHA_MIN_FRAME = 10;

// give up on a response after this long
static const uint32_t RESPONSE_TIMEOUT_MS = 600;
// A write gets longer: Recom allows a full second for the ACK, and an EEPROM
// program cycle plausibly delays it past the 600 ms a read is given.
static const uint32_t WRITE_TIMEOUT_MS = 1000;
// a gap this long after the last byte ends the frame
static const uint32_t RX_QUIET_MS = 40;
// settle time between two requests of the same cycle
static const uint32_t INTER_REQUEST_MS = 60;
// Parameters are configuration, not measurements - re-read them roughly hourly
// (240 x the default 15s poll) so a service-tool edit shows up without a reboot.
static const int PARAM_REFRESH_CYCLES = 240;

struct CodeText {
  uint16_t code;
  const char *text;
};

// The parameters this component is willing to write, with the min/max from the
// PCU-05 P3 map. Recom clamps every value to these before sending it
// (ValidateDataModel) and so does write_param(): a value outside the range is
// not an error, it is pulled to the nearest end.
//
// Deliberately absent, and not to be added casually: the gas/air settings
// (p17-p21, p77, p78) and the controller-protection limits (p55-p57). A bad
// write there is a combustion-safety problem rather than a comfort one. Also
// absent are the parameters stored as two's complement (p27, p30, p61, p86,
// p106), which would need a signed API.
//
// Adding a parameter is a one-line change; `offset` is the "Byte" column of the
// full parameter table in mapping/pcu05_p3_protocol.md.
struct ParamLimit {
  uint8_t param;   // the pNN number, as the manual and the map number it
  uint8_t offset;  // byte offset into the 128 byte parameter block
  uint8_t min;
  uint8_t max;
};

static const ParamLimit PARAM_LIMITS[] = {
    {1, 0, 20, 90},     // T flow set point - max flow temperature during CH
    {2, 1, 40, 65},     // DHW set point - the tank charge temperature
    {3, 2, 0, 3},       // Boiler controls - CH/DHW on/off
    {4, 3, 0, 2},       // Comfort DHW - always on / always off / controller
    {5, 4, 0, 99},      // Pump post run CH, minutes, 99 = continuous
    {23, 22, 20, 90},   // Max flow system
    {25, 24, 0, 30},    // Footpoint T outside (heating curve)
    {26, 25, 0, 90},    // Footpoint T flow (heating curve)
    {31, 30, 0, 2},     // Anti legionella
    {32, 31, 0, 25},    // Setpoint raise while charging the calorifier
    {33, 32, 2, 15},    // Hysteresis calorifier - DHW cut-in below tank setpoint
    {73, 72, 1, 10},    // Hysteresis CH
    {85, 84, 0, 20},    // Hysteresis warming up (DHW comfort)
    {88, 87, 1, 10},    // Hysteresis DHW - combi/flow-through only
    {105, 104, 0, 10},  // Offset calorifier - switch-off offset, tank sensor
};
static const size_t PARAM_LIMITS_LEN = sizeof(PARAM_LIMITS) / sizeof(PARAM_LIMITS[0]);

static const CodeText STATUS_CODES[] = {
    {0, "0:Standby"},
    {1, "1:Boiler start"},
    {2, "2:Burner start"},
    {3, "3:Burning CH"},
    {4, "4:Burning DHW"},
    {5, "5:Burner stop"},
    {6, "6:Boiler stop"},
    {7, "7:-"},
    {8, "8:Controlled stop"},
    {9, "9:Blocking mode"},
    {10, "10:Locking mode"},
    {11, "11:Chimney mode L"},
    {12, "12:Chimney mode h"},
    {13, "13:Chimney mode H"},
    {14, "14:-"},
    {15, "15:Manual-heatdemand"},
    {16, "16:Boiler-frost-protection"},
    {17, "17:De-airation"},
    {18, "18:Controller temp protection"},
};
static const size_t STATUS_CODES_LEN = sizeof(STATUS_CODES) / sizeof(STATUS_CODES[0]);

static const CodeText SUBSTATUS_CODES[] = {
    {0, "0:Standby"},
    {1, "1:Anti-cycling"},
    {2, "2:Open hydraulic valve"},
    {3, "3:Pump start"},
    {4, "4:Wait for burner start"},
    {10, "10:Open external gas valve"},
    {11, "11:Fan to fluegasvalve speed"},
    {12, "12:Open fluegasvalve"},
    {13, "13:Pre-purge"},
    {14, "14:Wait for release"},
    {15, "15:Burner start"},
    {16, "16:VPS test"},
    {17, "17:Pre-ignition"},
    {18, "18:Ignition"},
    {19, "19:Flame check"},
    {20, "20:Interpurge"},
    {30, "30:Normal internal setpoint"},
    {31, "31:Limited internal setpoint"},
    {32, "32:Normal power control"},
    {33, "33:Gradient control level 1"},
    {34, "34:Gradient control level 2"},
    {35, "35:Gradient control level 3"},
    {36, "36:Flame protection"},
    {37, "37:Stabilization time"},
    {38, "38:Cold start"},
    {39, "39:Limited power Tfg"},
    {40, "40:Burner stop"},
    {41, "41:Post purge"},
    {42, "42:Fan to fluegasvalve speed"},
    {43, "43:Close fluegasvalve"},
    {44, "44:Stop fan"},
    {45, "45:Close external gas valve"},
    {60, "60:Pump post running"},
    {61, "61:Pump stop"},
    {62, "62:Close hydraulic valve"},
    {63, "63:Start anti-cycle timer"},
    {255, "255:Reset wait time"},
};
static const size_t SUBSTATUS_CODES_LEN = sizeof(SUBSTATUS_CODES) / sizeof(SUBSTATUS_CODES[0]);

static const CodeText LOCKING_CODES[] = {
    {0, "PSU not connected (Locking 0)"},
    {1, "SU parameter fault (Locking 1)"},
    {2, "02:T Flow closed"},
    {3, "03:T Flow open"},
    {4, "04:T Flow < min."},
    {5, "05:T Flow > max."},
    {6, "T Return closed (Locking 6)"},
    {7, "T Return open (Locking 7)"},
    {8, "T Return < min. (Locking 8)"},
    {9, "T Return > max. (Locking 9)"},
    {10, "10:dT(Flow,Return) > max."},
    {11, "11:dT(Return,Flow) > max."},
    {12, "STB activated (Locking 12)"},
    {14, "5x Unsuccessful start (Locking 14)"},
    {16, "False flame (Locking 16)"},
    {17, "SU Gasvalve driver error (Locking 17)"},
    {34, "Fan out of control range (Locking 34)"},
    {35, "Return over Flow temp. (Locking 35)"},
    {36, "5x Flame loss (Locking 36)"},
    {37, "SU communication (Locking 37)"},
    {38, "SCU-S communication (Locking 38)"},
    {39, "BL input as lockout (Locking 39)"},
    {40, "- (Locking 40)"},
    {41, "E11: Airbox temp. > max."},
    {255, "No locking"},
};
static const size_t LOCKING_CODES_LEN = sizeof(LOCKING_CODES) / sizeof(LOCKING_CODES[0]);

static const CodeText BLOCKING_CODES[] = {
    {0, "PCU parameter fault (Blocking 0)"},
    {1, "T Flow > max.(Blocking 1)"},
    {2, "dT/s Flow > max. (Blocking 2)"},
    {7, "dT(Flow,Return) > max.(Blocking 7)"},
    {8, "No release signal(Blocking 8)"},
    {9, "L-N swept(Blocking 9)"},
    {10, "Blocking signal ex frost(Blocking 10)"},
    {11, "Blocking signal inc frost(Blocking 11)"},
    {12, "HMI not connected(Blocking 12)"},
    {13, "SCU communication(Blocking 13)"},
    {14, "Min. water pressure(Blocking 14)"},
    {15, "Min. gas pressure(Blocking 15)"},
    {16, "Ident. SU mismatch(Blocking 16)"},
    {17, "Ident. dF/dU table error(Blocking 17)"},
    {18, "Ident. PSU mismatch(Blocking 18)"},
    {19, "Ident. dF/dU needed(Blocking 19)"},
    {20, "Identification running(Blocking 20)"},
    {21, "SU communications lost(Blocking 21)"},
    {22, "Flame lost(Blocking 22)"},
    {25, "Internal SU error(Blocking 25)"},
    {26, "Calorifier sensor error(Blocking 26)"},
    {27, "DHW in sensor error(Blocking 27)"},
    {28, "Reset in progress...(Blocking 28)"},
    {255, "No blocking"},
};
static const size_t BLOCKING_CODES_LEN = sizeof(BLOCKING_CODES) / sizeof(BLOCKING_CODES[0]);

static const char *lookup_code_(const CodeText *table, size_t len, uint16_t code, char *fallback,
                                size_t fallback_len) {
  for (size_t i = 0; i < len; i++) {
    if (table[i].code == code)
      return table[i].text;
  }
  snprintf(fallback, fallback_len, "Unknown (%u)", static_cast<unsigned>(code));
  return fallback;
}

float Dietrich::signed_float_(float value) {
  if (value > 32768)
    value -= 65536;
  return value;
}

// A disconnected (open) temperature sensor input reads as 0x8000, which would
// scale to 327.68 degC - publish NAN instead so Home Assistant shows "unknown"
float Dietrich::temp_or_nan_(uint16_t raw) {
  if (raw == 0x8000)
    return NAN;
  return signed_float_(raw) * 0.01f;
}

std::string Dietrich::hex_str_(const uint8_t *data, size_t len) {
  std::string s;
  s.reserve(len * 2);
  static const char HEX_CHARS[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    s += HEX_CHARS[(data[i] >> 4) & 0x0F];
    s += HEX_CHARS[data[i] & 0x0F];
  }
  return s;
}

// CRC16, poly 0xA001, init 0xFFFF, over data[from .. to-1]
uint16_t Dietrich::crc16_(const uint8_t *data, size_t from, size_t to) {
  uint16_t crc = 0xFFFF;
  for (size_t i = from; i < to; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

// The CRC covers bytes 1..n-4; the frame ends with CRC (LSB, MSB) and 0x03
bool Dietrich::is_valid_crc_(const uint8_t *response, size_t n) {
  if (n < 5)
    return false;

  const uint16_t expected_crc = response[n - 3] | (response[n - 2] << 8);
  return expected_crc == crc16_(response, 1, n - 3);
}

// The checks Recom's RemehaReceiver.ValidateResponse makes, plus the CRC (see
// mapping/pcu05_p3_protocol.md). Returns nullptr for a good frame, otherwise a
// short reason for the log.
//
// Requiring COMMAND and EXT_COMMAND to come back echoed is what stops a late or
// duplicated frame being credited to the wrong request. The eight parameter-block
// reads differ only in their EXT byte, so a passing CRC says nothing about which
// block the 16 bytes actually belong to - and once those bytes are being
// read-modify-written back to EEPROM, crediting them to the wrong block would
// rewrite sixteen unrelated parameters.
//
// Avanta (calenta_v1_p5) frames use an XOR checksum and a different header, so
// that variant keeps its own much weaker check.
const char *Dietrich::response_error_() const {
  if (this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5) {
    if (this->rx_len_ < 3 || this->rx_buf_[0] != 2 || this->rx_buf_[1] != 65 || this->rx_buf_[2] != 6)
      return "bad header";
    return nullptr;
  }

  const uint8_t *req = nullptr;
  size_t req_len = 0;
  this->command_for_(this->queue_[this->queue_pos_], &req, &req_len);

  if (this->rx_len_ < REMEHA_MIN_FRAME)
    return "frame too short";
  if (this->rx_buf_[0] != 0x02)
    return "bad start byte";
  if (this->rx_buf_[this->rx_len_ - 1] != 0x03)
    return "bad end byte";
  if (this->rx_buf_[3] != REMEHA_TYPE_RESPONSE)
    return "not a response frame";
  if (static_cast<size_t>(this->rx_buf_[4]) != this->rx_len_ - 2)
    return "length byte disagrees with frame";
  if (this->rx_buf_[1] != req[2] || this->rx_buf_[2] != req[1])
    return "addresses not swapped";
  if (this->rx_buf_[5] != req[5])
    return "command not echoed";
  if (this->rx_buf_[6] != req[6])
    return "ext command not echoed";
  if (!is_valid_crc_(this->rx_buf_, this->rx_len_))
    return "bad CRC";
  return nullptr;
}

size_t Dietrich::header_len_() const { return this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5 ? 6 : 7; }

size_t Dietrich::trailer_len_() const { return this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5 ? 2 : 3; }

bool Dietrich::have_(size_t off, size_t count) const { return this->data_len_ >= off + count; }

uint8_t Dietrich::d_(size_t off) const {
  const size_t idx = this->header_len_() + off;
  return idx < this->rx_len_ ? this->rx_buf_[idx] : 0;
}

uint16_t Dietrich::le16_(size_t off) const {
  return static_cast<uint16_t>(this->d_(off) | (this->d_(off + 1) << 8));
}

uint16_t Dietrich::be16_(size_t off) const {
  return static_cast<uint16_t>((this->d_(off) << 8) | this->d_(off + 1));
}

void Dietrich::publish_(sensor::Sensor *s, float value) {
  if (s != nullptr)
    s->publish_state(value);
}

void Dietrich::publish_text_(text_sensor::TextSensor *s, const char *text) {
  if (s != nullptr)
    s->publish_state(text);
}

void Dietrich::pub_temp_(sensor::Sensor *s, size_t off) {
  if (s == nullptr || !this->have_(off, 2))
    return;
  s->publish_state(temp_or_nan_(this->le16_(off)));
}

void Dietrich::pub_s16_(sensor::Sensor *s, size_t off, float scale) {
  if (s == nullptr || !this->have_(off, 2))
    return;
  s->publish_state(signed_float_(this->le16_(off)) * scale);
}

void Dietrich::pub_u16_(sensor::Sensor *s, size_t off) {
  if (s == nullptr || !this->have_(off, 2))
    return;
  s->publish_state(this->le16_(off));
}

void Dietrich::pub_u8_(sensor::Sensor *s, size_t off, float scale) {
  if (s == nullptr || !this->have_(off, 1))
    return;
  s->publish_state(this->d_(off) * scale);
}

void Dietrich::pub_s8_(sensor::Sensor *s, size_t off) {
  if (s == nullptr || !this->have_(off, 1))
    return;
  s->publish_state(static_cast<int8_t>(this->d_(off)));
}

void Dietrich::pub_bit_(binary_sensor::BinarySensor *s, size_t off, uint8_t bit, bool invert) {
  if (s == nullptr || !this->have_(off, 1))
    return;
  bool v = ((this->d_(off) >> bit) & 1) != 0;
  if (invert)
    v = !v;
  s->publish_state(v);
}

void Dietrich::pub_code_(sensor::Sensor *num, text_sensor::TextSensor *txt, size_t off, const CodeText *table,
                         size_t len) {
  if (!this->have_(off, 1))
    return;
  const uint8_t code = this->d_(off);
  this->publish_(num, code);
  if (txt != nullptr) {
    char fallback[24];
    this->publish_text_(txt, lookup_code_(table, len, code, fallback, sizeof(fallback)));
  }
}

void Dietrich::pub_counter_(sensor::Sensor *s, size_t off, float scale) {
  if (s == nullptr || !this->have_(off, 2))
    return;
  s->publish_state(this->be16_(off) * scale);
}

void Dietrich::command_for_(DietrichRequest req, const uint8_t **cmd, size_t *len) const {
  if (req >= DIETRICH_REQ_PARAM0) {
    const size_t blk = static_cast<size_t>(req) - DIETRICH_REQ_PARAM0;
    // inside a write transaction, address the PCU exactly as Recom does
    *cmd = this->txn_active_ ? CMD_PARAM_REMEHA_PCU[blk] : CMD_PARAM_REMEHA[blk];
    *len = sizeof(CMD_PARAM_REMEHA[blk]);
    return;
  }

  const bool calenta = this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5;
  switch (req) {
    case DIETRICH_REQ_SERVICE_ON:
      *cmd = CMD_SERVICE_ON_REMEHA;
      *len = sizeof(CMD_SERVICE_ON_REMEHA);
      break;
    case DIETRICH_REQ_SERVICE_OFF:
      *cmd = CMD_SERVICE_OFF_REMEHA;
      *len = sizeof(CMD_SERVICE_OFF_REMEHA);
      break;
    case DIETRICH_REQ_WRITE_BLOCK:
      // built per transaction by build_write_frame_(), just before it is sent
      *cmd = this->tx_buf_;
      *len = DIETRICH_WRITE_FRAME_LEN;
      break;
    case DIETRICH_REQ_COUNTER1:
      *cmd = calenta ? CMD_COUNTER1_CALENTA : CMD_COUNTER1_MCR3;
      *len = calenta ? sizeof(CMD_COUNTER1_CALENTA) : sizeof(CMD_COUNTER1_MCR3);
      break;
    case DIETRICH_REQ_COUNTER2:
      *cmd = calenta ? CMD_COUNTER2_CALENTA : CMD_COUNTER2_MCR3;
      *len = calenta ? sizeof(CMD_COUNTER2_CALENTA) : sizeof(CMD_COUNTER2_MCR3);
      break;
    case DIETRICH_REQ_SAMPLE:
    default:
      *cmd = calenta ? CMD_SAMPLE_CALENTA : CMD_SAMPLE_MCR3;
      *len = calenta ? sizeof(CMD_SAMPLE_CALENTA) : sizeof(CMD_SAMPLE_MCR3);
      break;
  }
}

void Dietrich::decode_sample_() {
  const bool p3 = this->variant_ == DIETRICH_VARIANT_PCU05_P3;
  // The four fields below used to be addressed by absolute frame index. On the
  // Avanta variant that lands one byte further into the data block than it does
  // on the Remeha variants; keep that behaviour so existing configs are stable.
  const size_t t = this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5 ? 1 : 0;

  this->pub_temp_(this->flow_temp_sensor_, 0);
  this->pub_temp_(this->return_temp_sensor_, 2);
  this->pub_temp_(this->dhw_in_temp_sensor_, 4);
  this->pub_temp_(this->outside_temp_sensor_, 6);
  this->pub_temp_(this->calorifier_temp_sensor_, 8);
  this->pub_temp_(this->boiler_control_temp_sensor_, 12);
  this->pub_temp_(this->room_temp_sensor_, 14);
  this->pub_s16_(this->ch_setpoint_sensor_, 16, 0.01f);
  this->pub_s16_(this->dhw_setpoint_sensor_, 18, 0.01f);
  this->pub_s16_(this->room_temp_setpoint_sensor_, 20, 0.01f);

  this->pub_u16_(this->fan_speed_setpoint_sensor_, 22);
  this->pub_u16_(this->fan_speed_sensor_, 24);
  this->pub_u8_(this->ionisation_current_sensor_, 26, 0.1f);
  this->pub_s16_(this->internal_setpoint_sensor_, 27, 0.01f);
  this->pub_u8_(this->available_power_sensor_, 29, 1.0f);
  this->pub_u8_(this->pump_percentage_sensor_, 30, 1.0f);
  this->pub_u8_(this->desired_max_power_sensor_, 32, 1.0f);
  this->pub_u8_(this->actual_power_sensor_, 33, 1.0f);

  // byte 36 - heat demand sources; bit 4 (DHW eco) is inverted in every Recom map
  this->pub_bit_(this->demand_source_bit0_binary_sensor_, 36, 0, false);
  this->pub_bit_(this->demand_source_bit1_binary_sensor_, 36, 1, false);
  this->pub_bit_(this->demand_source_bit2_binary_sensor_, 36, 2, false);
  this->pub_bit_(this->demand_source_bit3_binary_sensor_, 36, 3, false);
  this->pub_bit_(this->demand_source_bit4_binary_sensor_, 36, 4, true);
  this->pub_bit_(this->demand_source_bit5_binary_sensor_, 36, 5, false);
  this->pub_bit_(this->demand_source_bit6_binary_sensor_, 36, 6, false);
  this->pub_bit_(this->demand_source_bit7_binary_sensor_, 36, 7, false);

  // byte 37 - inputs; PCU-05 P3 inverts the shutdown and release inputs
  this->pub_bit_(this->input_bit0_binary_sensor_, 37, 0, p3);
  this->pub_bit_(this->input_bit1_binary_sensor_, 37, 1, p3);
  this->pub_bit_(this->input_bit2_binary_sensor_, 37, 2, false);
  this->pub_bit_(this->input_bit3_binary_sensor_, 37, 3, false);
  this->pub_bit_(this->input_bit5_binary_sensor_, 37, 5, false);
  this->pub_bit_(this->input_bit6_binary_sensor_, 37, 6, false);
  this->pub_bit_(this->input_bit7_binary_sensor_, 37, 7, false);

  // byte 38 - valves; bit 0 (gas valve) is inverted in every Recom map
  this->pub_bit_(this->valve_bit0_binary_sensor_, 38, 0, true);
  this->pub_bit_(this->valve_bit2_binary_sensor_, 38, 2, false);
  this->pub_bit_(this->valve_bit3_binary_sensor_, 38, 3, false);
  this->pub_bit_(this->valve_bit4_binary_sensor_, 38, 4, false);
  this->pub_bit_(this->valve_bit6_binary_sensor_, 38, 6, false);

  // byte 39 - pumps
  this->pub_bit_(this->pump_bit0_binary_sensor_, 39, 0, false);
  this->pub_bit_(this->pump_bit1_binary_sensor_, 39, 1, false);
  this->pub_bit_(this->pump_bit2_binary_sensor_, 39, 2, false);
  this->pub_bit_(this->pump_bit4_binary_sensor_, 39, 4, false);
  this->pub_bit_(this->pump_bit7_binary_sensor_, 39, 7, false);

  this->pub_code_(this->state_sensor_, this->state_text_sensor_, 40, STATUS_CODES, STATUS_CODES_LEN);
  this->pub_code_(this->lockout_sensor_, this->lockout_text_sensor_, 41, LOCKING_CODES, LOCKING_CODES_LEN);
  this->pub_code_(this->blocking_sensor_, this->blocking_text_sensor_, 42, BLOCKING_CODES, BLOCKING_CODES_LEN);
  this->pub_code_(this->sub_state_sensor_, this->sub_state_text_sensor_, 43, SUBSTATUS_CODES, SUBSTATUS_CODES_LEN);

  // PCU-05 P3 additions; have_() skips them when the frame is shorter
  this->pub_u16_(this->fan_speed_rpm_sensor_, 44);
  this->pub_u8_(this->su_state_sensor_, 46, 1.0f);
  this->pub_u8_(this->su_locking_sensor_, 47, 1.0f);
  this->pub_u8_(this->su_blocking_sensor_, 48, 1.0f);

  // Not defined in the PCU-05 P3 map (Recom ships it commented out) - see README
  this->pub_u8_(this->hydro_pressure_sensor_, 49 + t, 0.1f);
  this->pub_bit_(this->hru_binary_sensor_, 50 + t, 1, false);
  this->pub_bit_(this->ch_timer_enable_binary_sensor_, 50 + t, 6, false);
  this->pub_bit_(this->dhw_timer_enable_binary_sensor_, 50 + t, 7, false);
  this->pub_temp_(this->control_temp_sensor_, 51 + t);
  this->pub_s16_(this->dhw_flowrate_sensor_, 53 + t, 0.01f);

  this->pub_temp_(this->solar_temp_sensor_, 56);
  this->pub_u16_(this->hmi_active_sensor_, 58);
  this->pub_s8_(this->ch_setpoint_hmi_sensor_, 60);
  this->pub_s8_(this->dhw_setpoint_hmi_sensor_, 61);
  this->pub_u8_(this->service_mode_sensor_, 62, 1.0f);
  this->pub_u8_(this->rs232_mode_sensor_, 63, 1.0f);

  // A reset partway through a write transaction leaves service mode on
  // indefinitely, so check once at boot and re-lock if it is. Byte 62 reports the
  // current state. Gated on allow_writes: re-locking a board that somebody else
  // deliberately unlocked is not this component's business otherwise.
  // Reached only from inside a service mode test; see start_txn_().
  //
  // The P3 map labels byte 62 `service_mode` and byte 63 `rs232_mode`, but on a
  // live PCU-05 P3 it is **byte 63** that goes 0 -> 1 for exactly the duration of
  // the CODE_SERVICE_START / CODE_SERVICE_STOP window, while byte 62 stays 0
  // throughout. Diffing a sample taken inside the window against one taken three
  // seconds later shows byte 63 and nothing else but drifting temperatures. So
  // byte 63 is the flag to trust here, whatever the map calls it - plausibly the
  // board considers the service command to be putting it under RS232/PC control.
  if (this->txn_active_ && this->txn_kind_ == DIETRICH_TXN_SERVICE_TEST && this->have_(63, 1)) {
    const unsigned b62 = this->d_(62), b63 = this->d_(63);
    if (b63 != 0) {
      ESP_LOGI(TAG, "service mode readback: byte 62 = %u, byte 63 = %u - the unlock took effect", b62, b63);
    } else {
      ESP_LOGE(TAG,
               "service mode readback: byte 62 = %u, byte 63 = 0 - the unlock was ACKed but did NOT "
               "take effect, EEPROM writes will be ignored",
               b62);
    }
  }

  // Samples taken inside a transaction are deliberately excluded: the boot check
  // is about finding the boiler already unlocked, not about this component's own
  // unlocking.
  if (!this->seen_sample_ && !this->txn_active_ && this->have_(63, 1)) {
    this->seen_sample_ = true;
    // byte 63, for the reason given above
    if (this->allow_writes_ && this->d_(63) != 0) {
      ESP_LOGW(TAG, "boiler is in service mode at boot (sample byte 63 = %u), re-locking",
               static_cast<unsigned>(this->d_(63)));
      this->stage_txn_(DIETRICH_TXN_RELOCK, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, "boot re-lock");
    }
  }
}

void Dietrich::decode_counter1_() {
  // The Avanta variant returns the first counter block six bytes further in
  if (this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5) {
    this->pub_counter_(this->hours_run_pump_sensor_, 6, 2.0f);
    this->pub_counter_(this->hours_run_3way_sensor_, 8, 2.0f);
    this->pub_counter_(this->hours_run_ch_sensor_, 10, 2.0f);
    this->pub_counter_(this->hours_run_dhw_sensor_, 12, 1.0f);
    this->pub_counter_(this->power_supply_aval_hours_sensor_, 14, 2.0f);
    return;
  }

  this->pub_counter_(this->hours_run_pump_sensor_, 0, 2.0f);
  this->pub_counter_(this->hours_run_3way_sensor_, 2, 2.0f);
  this->pub_counter_(this->hours_run_ch_sensor_, 4, 2.0f);
  this->pub_counter_(this->hours_run_dhw_sensor_, 6, 1.0f);
  this->pub_counter_(this->power_supply_aval_hours_sensor_, 8, 2.0f);
  this->pub_counter_(this->pump_starts_sensor_, 10, 8.0f);
  this->pub_counter_(this->number_of_3way_valve_cycles_sensor_, 12, 8.0f);
  this->pub_counter_(this->burner_start_dhw_sensor_, 14, 8.0f);
}

void Dietrich::decode_counter2_() {
  if (this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5) {
    this->pub_counter_(this->pump_starts_sensor_, 0, 8.0f);
    this->pub_counter_(this->number_of_3way_valve_cycles_sensor_, 2, 8.0f);
    this->pub_counter_(this->burner_start_dhw_sensor_, 4, 8.0f);
    this->pub_counter_(this->total_burner_start_sensor_, 6, 8.0f);
    this->pub_counter_(this->failed_burner_start_sensor_, 8, 1.0f);
    this->pub_counter_(this->number_flame_loss_sensor_, 10, 1.0f);
    return;
  }

  this->pub_counter_(this->total_burner_start_sensor_, 0, 8.0f);
  this->pub_counter_(this->failed_burner_start_sensor_, 2, 1.0f);
  this->pub_counter_(this->number_flame_loss_sensor_, 4, 1.0f);
}

void Dietrich::pub_param_(sensor::Sensor *s, size_t off, float scale) {
  if (s == nullptr || off >= DIETRICH_PARAM_BYTES)
    return;
  s->publish_state(this->params_[off] * scale);
}

void Dietrich::pub_param_s8_(sensor::Sensor *s, size_t off) {
  if (s == nullptr || off >= DIETRICH_PARAM_BYTES)
    return;
  // p27 and p30 are stored as two's complement; Recom shows them signed, the
  // front panel does not (the manual tells installers to subtract 256 by hand)
  s->publish_state(static_cast<int8_t>(this->params_[off]));
}

bool Dietrich::want_params_() const {
  return this->param_ch_max_flow_sensor_ != nullptr || this->param_dhw_setpoint_sensor_ != nullptr ||
         this->param_pump_post_run_sensor_ != nullptr || this->param_max_flow_system_sensor_ != nullptr ||
         this->param_curve_foot_outside_sensor_ != nullptr || this->param_curve_foot_flow_sensor_ != nullptr ||
         this->param_curve_cold_outside_sensor_ != nullptr || this->param_pump_ch_min_sensor_ != nullptr ||
         this->param_pump_ch_max_sensor_ != nullptr || this->param_dhw_hysteresis_sensor_ != nullptr;
}

void Dietrich::decode_params_() {
  // the whole block, so any parameter can be read off the log without a rebuild
  ESP_LOGD(TAG, "parameter block: %s", hex_str_(this->params_, DIETRICH_PARAM_BYTES).c_str());

  this->pub_param_(this->param_ch_max_flow_sensor_, 0, 1.0f);         // p1
  this->pub_param_(this->param_dhw_setpoint_sensor_, 1, 1.0f);        // p2
  this->pub_param_(this->param_pump_post_run_sensor_, 4, 1.0f);       // p5
  this->pub_param_(this->param_max_flow_system_sensor_, 22, 1.0f);    // p23
  this->pub_param_(this->param_curve_foot_outside_sensor_, 24, 1.0f); // p25
  this->pub_param_(this->param_curve_foot_flow_sensor_, 25, 1.0f);    // p26
  this->pub_param_s8_(this->param_curve_cold_outside_sensor_, 26);    // p27
  this->pub_param_(this->param_pump_ch_min_sensor_, 27, 10.0f);       // p28, stored x10 %
  this->pub_param_(this->param_pump_ch_max_sensor_, 28, 10.0f);       // p29, stored x10 %
  this->pub_param_(this->param_dhw_hysteresis_sensor_, 32, 1.0f);     // p33
}

uint32_t Dietrich::timeout_for_(DietrichRequest req) {
  return req == DIETRICH_REQ_WRITE_BLOCK ? WRITE_TIMEOUT_MS : RESPONSE_TIMEOUT_MS;
}

// 02 | FE | 01 | 05 | 18 | 11 | blk | 16 data bytes | CRClo CRChi | 03
//
// The payload is the block as the boiler just reported it, with the staged edit
// applied on top. Fifteen of those sixteen bytes are unrelated parameters going
// back verbatim, which is why the read has to belong to this transaction rather
// than to the hourly sweep.
void Dietrich::build_write_frame_(uint8_t block) {
  const size_t base = (static_cast<size_t>(block) - DIETRICH_PARAM_FIRST_BLOCK) * DIETRICH_PARAM_BLOCK_SIZE;

  this->tx_buf_[0] = 0x02;
  this->tx_buf_[1] = 0xFE;  // sender: the PC
  this->tx_buf_[2] = 0x01;  // recipient: the PCU
  this->tx_buf_[3] = 0x05;  // request
  this->tx_buf_[4] = static_cast<uint8_t>(DIETRICH_WRITE_FRAME_LEN - 2);
  this->tx_buf_[5] = 0x11;  // WRITE_EPROM_BLOCK
  this->tx_buf_[6] = block;

  for (size_t i = 0; i < DIETRICH_PARAM_BLOCK_SIZE; i++)
    this->tx_buf_[7 + i] = this->params_[base + i];

  // an identity write sends the block back untouched; only a parameter write edits
  if (this->txn_kind_ == DIETRICH_TXN_PARAM)
    this->tx_buf_[7 + this->pending_offset_] = this->pending_value_;

  const uint16_t crc = crc16_(this->tx_buf_, 1, DIETRICH_WRITE_FRAME_LEN - 3);
  this->tx_buf_[DIETRICH_WRITE_FRAME_LEN - 3] = static_cast<uint8_t>(crc & 0xFF);
  this->tx_buf_[DIETRICH_WRITE_FRAME_LEN - 2] = static_cast<uint8_t>(crc >> 8);
  this->tx_buf_[DIETRICH_WRITE_FRAME_LEN - 1] = 0x03;
}

bool Dietrich::stage_txn_(DietrichTxn txn, uint8_t block, uint8_t offset, uint8_t value, const char *what) {
  if (!this->allow_writes_) {
    ESP_LOGW(TAG, "%s refused: allow_writes is not set on the dietrich component", what);
    return false;
  }
  // The 128 byte parameter map belongs to the PCU-05 P3 parameter set. On another
  // board the same offset is a different parameter, so refuse outright rather
  // than write something unknown.
  if (this->variant_ != DIETRICH_VARIANT_PCU05_P3) {
    ESP_LOGW(TAG, "%s refused: writing is only supported on variant pcu05_p3", what);
    return false;
  }
  if (this->pending_txn_ != DIETRICH_TXN_NONE || this->txn_active_) {
    ESP_LOGW(TAG, "%s refused: another write is already in progress", what);
    return false;
  }

  this->pending_txn_ = txn;
  this->pending_block_ = block;
  this->pending_offset_ = offset;
  this->pending_value_ = value;
  ESP_LOGI(TAG, "%s queued", what);
  return true;
}

bool Dietrich::test_service_mode() { return this->stage_txn_(DIETRICH_TXN_SERVICE_TEST, 0, 0, 0, "service mode test"); }

bool Dietrich::write_block_unchanged(uint8_t block) {
  char what[48];
  snprintf(what, sizeof(what), "identity write of block 0x%02X", static_cast<unsigned>(block));
  if (block < DIETRICH_PARAM_FIRST_BLOCK || block > DIETRICH_PARAM_LAST_BLOCK) {
    ESP_LOGW(TAG, "%s refused: outside the parameter block 0x%02X..0x%02X", what,
             static_cast<unsigned>(DIETRICH_PARAM_FIRST_BLOCK), static_cast<unsigned>(DIETRICH_PARAM_LAST_BLOCK));
    return false;
  }
  return this->stage_txn_(DIETRICH_TXN_IDENTITY, block, 0, 0, what);
}

bool Dietrich::write_param(uint8_t param, uint8_t value) {
  const ParamLimit *lim = nullptr;
  for (size_t i = 0; i < PARAM_LIMITS_LEN; i++) {
    if (PARAM_LIMITS[i].param == param) {
      lim = &PARAM_LIMITS[i];
      break;
    }
  }
  if (lim == nullptr) {
    ESP_LOGW(TAG, "write of p%u refused: not one of the parameters this component will write",
             static_cast<unsigned>(param));
    return false;
  }

  uint8_t clamped = value;
  if (clamped < lim->min)
    clamped = lim->min;
  if (clamped > lim->max)
    clamped = lim->max;
  if (clamped != value) {
    ESP_LOGW(TAG, "p%u: %u is outside %u..%u, clamped to %u", static_cast<unsigned>(param),
             static_cast<unsigned>(value), static_cast<unsigned>(lim->min), static_cast<unsigned>(lim->max),
             static_cast<unsigned>(clamped));
  }

  char what[48];
  snprintf(what, sizeof(what), "write of p%u = %u", static_cast<unsigned>(param), static_cast<unsigned>(clamped));
  const uint8_t block = static_cast<uint8_t>(DIETRICH_PARAM_FIRST_BLOCK + lim->offset / DIETRICH_PARAM_BLOCK_SIZE);
  const uint8_t offset = static_cast<uint8_t>(lim->offset % DIETRICH_PARAM_BLOCK_SIZE);
  return this->stage_txn_(DIETRICH_TXN_PARAM, block, offset, clamped, what);
}

void Dietrich::start_txn_() {
  this->txn_kind_ = this->pending_txn_;
  this->txn_block_ = this->pending_block_;
  this->pending_txn_ = DIETRICH_TXN_NONE;
  this->txn_active_ = true;
  this->txn_failed_ = false;
  this->txn_read_ok_ = false;
  this->txn_wrote_ = false;
  this->queue_pos_ = 0;

  const DietrichRequest block_read =
      static_cast<DietrichRequest>(DIETRICH_REQ_PARAM0 + (this->txn_block_ - DIETRICH_PARAM_FIRST_BLOCK));

  uint8_t n = 0;
  switch (this->txn_kind_) {
    case DIETRICH_TXN_RELOCK:
      this->queue_[n++] = DIETRICH_REQ_SERVICE_OFF;
      break;
    case DIETRICH_TXN_SERVICE_TEST:
      this->queue_[n++] = DIETRICH_REQ_SERVICE_ON;
      // A sample taken while unlocked is the only way to find out whether the
      // unlock did anything: CODE_SERVICE_START's ACK is a bare echo with no
      // payload, so it proves the frame was understood, not that service mode
      // is on. Sample byte 62 reports the actual state.
      this->queue_[n++] = DIETRICH_REQ_SAMPLE;
      this->queue_[n++] = DIETRICH_REQ_SERVICE_OFF;
      break;
    default:
      this->queue_[n++] = DIETRICH_REQ_SERVICE_ON;
      this->queue_[n++] = block_read;  // fresh copy to modify
      this->queue_[n++] = DIETRICH_REQ_WRITE_BLOCK;
      this->queue_[n++] = block_read;  // read back to verify
      this->queue_[n++] = DIETRICH_REQ_SERVICE_OFF;
      break;
  }
  // the re-lock has to be last: a failed step jumps straight to queue_len_ - 1
  this->queue_len_ = n;

  this->next_send_time_ = millis();
  this->state_machine_ = DIETRICH_SEND;
}

void Dietrich::finish_txn_() {
  const char *kind = "write";
  switch (this->txn_kind_) {
    case DIETRICH_TXN_SERVICE_TEST:
      kind = "service mode test";
      break;
    case DIETRICH_TXN_IDENTITY:
      kind = "identity write";
      break;
    case DIETRICH_TXN_PARAM:
      kind = "parameter write";
      break;
    case DIETRICH_TXN_RELOCK:
      kind = "re-lock";
      break;
    default:
      break;
  }

  if (this->txn_failed_) {
    ESP_LOGE(TAG, "%s failed; service mode was re-locked, nothing was retried", kind);
  } else if (this->txn_wrote_) {
    // params_ now holds the verify read, tx_buf_ holds what actually went out
    const size_t base =
        (static_cast<size_t>(this->txn_block_) - DIETRICH_PARAM_FIRST_BLOCK) * DIETRICH_PARAM_BLOCK_SIZE;
    bool same = true;
    for (size_t i = 0; i < DIETRICH_PARAM_BLOCK_SIZE; i++) {
      if (this->params_[base + i] != this->tx_buf_[7 + i])
        same = false;
    }
    if (same) {
      ESP_LOGI(TAG, "%s of block 0x%02X verified", kind, static_cast<unsigned>(this->txn_block_));
    } else {
      ESP_LOGE(TAG, "%s of block 0x%02X was ACKed but reads back different: sent %s, got %s", kind,
               static_cast<unsigned>(this->txn_block_), hex_str_(this->tx_buf_ + 7, DIETRICH_PARAM_BLOCK_SIZE).c_str(),
               hex_str_(this->params_ + base, DIETRICH_PARAM_BLOCK_SIZE).c_str());
    }
  } else {
    ESP_LOGI(TAG, "%s finished, nothing was written", kind);
  }

  this->txn_active_ = false;
  this->txn_failed_ = false;
  this->txn_read_ok_ = false;
  this->txn_wrote_ = false;
  this->txn_kind_ = DIETRICH_TXN_NONE;
}

bool Dietrich::handle_response_() {
  const DietrichRequest req = this->queue_[this->queue_pos_];
  char param_what[16];
  const char *what;
  if (req >= DIETRICH_REQ_PARAM0) {
    snprintf(param_what, sizeof(param_what), "param 0x%02X",
             static_cast<unsigned>(DIETRICH_PARAM_FIRST_BLOCK + (req - DIETRICH_REQ_PARAM0)));
    what = param_what;
  } else {
    switch (req) {
      case DIETRICH_REQ_COUNTER1:
        what = "counter1";
        break;
      case DIETRICH_REQ_COUNTER2:
        what = "counter2";
        break;
      case DIETRICH_REQ_SERVICE_ON:
        what = "service mode on";
        break;
      case DIETRICH_REQ_SERVICE_OFF:
        what = "service mode off";
        break;
      case DIETRICH_REQ_WRITE_BLOCK:
        what = "eeprom write";
        break;
      default:
        what = "sample";
        break;
    }
  }

  ESP_LOGD(TAG, "%s data (%u bytes): %s", what, static_cast<unsigned>(this->rx_len_),
           hex_str_(this->rx_buf_, this->rx_len_).c_str());

  if (this->rx_len_ == 0) {
    // Recom treats a timed-out write as a success; see the note in
    // mapping/pcu05_p3_protocol.md. No response is a failure here.
    ESP_LOGW(TAG, "no response to %s request", what);
    return false;
  }

  const char *err = this->response_error_();
  if (err != nullptr) {
    ESP_LOGW(TAG, "%s response rejected: %s", what, err);
    return false;
  }

  const size_t overhead = this->header_len_() + this->trailer_len_();
  this->data_len_ = this->rx_len_ > overhead ? this->rx_len_ - overhead : 0;

  if (req >= DIETRICH_REQ_PARAM0) {
    const size_t blk = static_cast<size_t>(req) - DIETRICH_REQ_PARAM0;
    // A short block used to be copied as far as it went, leaving the tail of
    // params_ holding the previous sweep's bytes. That is a cosmetic problem for a
    // sensor and a real one for read-modify-write, so take the block only whole.
    if (this->data_len_ != DIETRICH_PARAM_BLOCK_SIZE) {
      ESP_LOGW(TAG, "%s returned %u data bytes, expected %u", what, static_cast<unsigned>(this->data_len_),
               static_cast<unsigned>(DIETRICH_PARAM_BLOCK_SIZE));
      return false;
    }
    for (size_t i = 0; i < DIETRICH_PARAM_BLOCK_SIZE; i++)
      this->params_[blk * DIETRICH_PARAM_BLOCK_SIZE + i] = this->d_(i);
    this->param_blocks_seen_ |= static_cast<uint8_t>(1u << blk);
    // a whole block, for the block this transaction is about to write
    if (this->txn_active_ && this->txn_block_ == DIETRICH_PARAM_FIRST_BLOCK + blk)
      this->txn_read_ok_ = true;
    // publish only once the whole sweep is in, so the values are consistent
    if (this->param_blocks_seen_ == 0xFF)
      this->decode_params_();
    return true;
  }

  if (req == DIETRICH_REQ_WRITE_BLOCK) {
    // An ACK is an ordinary response frame carrying no data, and
    // response_error_() has already checked the type byte, the swapped
    // addresses and the echoed COMMAND/EXT_COMMAND - so there is nothing left
    // to decode, and getting this far is the acknowledgement.
    ESP_LOGI(TAG, "block 0x%02X write ACKed", static_cast<unsigned>(this->txn_block_));
    this->txn_wrote_ = true;
    return true;
  }

  switch (req) {
    case DIETRICH_REQ_SAMPLE:
      this->decode_sample_();
      break;
    case DIETRICH_REQ_COUNTER1:
      this->decode_counter1_();
      break;
    case DIETRICH_REQ_COUNTER2:
      this->decode_counter2_();
      break;
    default:
      // service mode on/off carry no data; the validated ACK is the whole result
      break;
  }
  return true;
}

void Dietrich::send_request_() {
  // signed difference so the comparison survives the millis() wrap at ~49 days
  if (static_cast<int32_t>(millis() - this->next_send_time_) < 0)
    return;

  const DietrichRequest req = this->queue_[this->queue_pos_];

  if (req == DIETRICH_REQ_WRITE_BLOCK) {
    // Nothing is written unless this transaction's own read of the block came
    // back whole. Without it, fifteen unrelated parameters would go to EEPROM as
    // whatever happened to be sitting in params_.
    if (!this->txn_read_ok_) {
      ESP_LOGE(TAG, "refusing to write block 0x%02X: its read did not land",
               static_cast<unsigned>(this->txn_block_));
      this->txn_failed_ = true;
      this->skip_to_relock_();
      return;
    }
    // A parameter that already holds the wanted value is not worth an EEPROM
    // cycle, and EEPROM endurance is finite. The check waits until here because
    // it needs the block this transaction just read, not a stale copy.
    if (this->txn_kind_ == DIETRICH_TXN_PARAM) {
      const size_t off =
          (static_cast<size_t>(this->txn_block_) - DIETRICH_PARAM_FIRST_BLOCK) * DIETRICH_PARAM_BLOCK_SIZE +
          this->pending_offset_;
      if (this->params_[off] == this->pending_value_) {
        ESP_LOGI(TAG, "parameter already reads %u, skipping the write", static_cast<unsigned>(this->pending_value_));
        this->skip_to_relock_();
        return;
      }
    }
    this->build_write_frame_(this->txn_block_);
  }

  // drop anything left over from a previous exchange
  while (this->available())
    this->read();

  const uint8_t *cmd = nullptr;
  size_t len = 0;
  this->command_for_(req, &cmd, &len);
  if (req == DIETRICH_REQ_WRITE_BLOCK)
    ESP_LOGI(TAG, "writing block 0x%02X: %s", static_cast<unsigned>(this->txn_block_), hex_str_(cmd, len).c_str());
  this->write_array(cmd, len);

  this->rx_len_ = 0;
  this->data_len_ = 0;
  this->request_time_ = this->last_byte_time_ = millis();
  this->state_machine_ = DIETRICH_WAIT;
}

void Dietrich::poll_response_() {
  while (this->available() && this->rx_len_ < sizeof(this->rx_buf_)) {
    this->rx_buf_[this->rx_len_++] = this->read();
    this->last_byte_time_ = millis();
  }

  const uint32_t now = millis();
  bool done = false;

  if (this->rx_len_ == sizeof(this->rx_buf_)) {
    done = true;  // buffer full, nothing more will fit
  } else if (this->rx_len_ > 0 && now - this->last_byte_time_ >= RX_QUIET_MS) {
    done = true;  // the boiler stopped talking
  } else if (now - this->request_time_ >= timeout_for_(this->queue_[this->queue_pos_])) {
    done = true;  // nothing came back at all
  }

  if (!done)
    return;

  const bool ok = this->handle_response_();
  if (this->txn_active_ && !ok)
    this->txn_failed_ = true;
  this->advance_();
}

// Jump to the re-lock, which start_txn_() always puts last in the queue. Used
// both for a step that failed and for a write that turned out to be unnecessary.
void Dietrich::skip_to_relock_() {
  if (this->queue_len_ > 0)
    this->queue_pos_ = static_cast<uint8_t>(this->queue_len_ - 1);
  this->next_send_time_ = millis() + INTER_REQUEST_MS;
  this->state_machine_ = DIETRICH_SEND;
}

void Dietrich::advance_() {
  // A failed step in a write transaction goes straight to the re-lock instead of
  // carrying on. Recom leaves service mode on when a write fails - it re-locks
  // only in the success branch, with no try/finally - and that is not a bug
  // worth copying. See mapping/pcu05_p3_protocol.md.
  if (this->txn_active_ && this->txn_failed_ && this->queue_pos_ + 1 < this->queue_len_) {
    this->skip_to_relock_();
    return;
  }

  this->queue_pos_++;
  if (this->queue_pos_ >= this->queue_len_) {
    if (this->txn_active_)
      this->finish_txn_();
    this->state_machine_ = DIETRICH_IDLE;
    return;
  }
  this->next_send_time_ = millis() + INTER_REQUEST_MS;
  this->state_machine_ = DIETRICH_SEND;
}

void Dietrich::loop() {
  switch (this->state_machine_) {
    case DIETRICH_SEND:
      this->send_request_();
      break;
    case DIETRICH_WAIT:
      this->poll_response_();
      break;
    case DIETRICH_IDLE:
    default:
      // A write staged from a lambda starts as soon as the bus is free rather
      // than waiting out the rest of the poll interval. Starting only from IDLE
      // is what keeps it from ever landing inside an exchange in flight.
      if (this->pending_txn_ != DIETRICH_TXN_NONE)
        this->start_txn_();
      break;
  }
}

void Dietrich::update() {
  if (this->state_machine_ != DIETRICH_IDLE) {
    // A write holding the bus is expected, not a fault - it just borrows one
    // poll interval. Only a poll overrunning its own interval is worth a warning.
    if (this->txn_active_)
      ESP_LOGD(TAG, "write transaction in progress, skipping this poll interval");
    else
      ESP_LOGW(TAG, "previous poll still running, skipping this interval");
    return;
  }

  if (this->pending_txn_ != DIETRICH_TXN_NONE) {
    this->start_txn_();
    return;
  }

  this->counter_timer_++;
  this->param_timer_++;
  this->queue_pos_ = 0;

  // A parameter sweep is 8 requests, so give it a whole poll interval of its own
  // rather than appending it to the sample or counter cycle.
  if (this->variant_ == DIETRICH_VARIANT_PCU05_P3 && this->want_params_() &&
      this->param_timer_ >= PARAM_REFRESH_CYCLES) {
    this->param_timer_ = 0;
    this->param_blocks_seen_ = 0;
    for (uint8_t i = 0; i < DIETRICH_PARAM_BLOCKS; i++)
      this->queue_[i] = static_cast<DietrichRequest>(DIETRICH_REQ_PARAM0 + i);
    this->queue_len_ = DIETRICH_PARAM_BLOCKS;
  } else if (this->counter_timer_ >= 8) {
    this->counter_timer_ = 0;
    this->queue_[0] = DIETRICH_REQ_COUNTER1;
    this->queue_[1] = DIETRICH_REQ_COUNTER2;
    this->queue_len_ = 2;
  } else {
    this->queue_[0] = DIETRICH_REQ_SAMPLE;
    this->queue_len_ = 1;
  }

  this->next_send_time_ = millis();
  this->state_machine_ = DIETRICH_SEND;
}

void Dietrich::dump_config() {
  const char *variant = "mcr3";
  if (this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5)
    variant = "calenta_v1_p5";
  else if (this->variant_ == DIETRICH_VARIANT_PCU05_P3)
    variant = "pcu05_p3";

  ESP_LOGCONFIG(TAG, "Dietrich boiler:");
  ESP_LOGCONFIG(TAG, "  Variant: %s", variant);
  ESP_LOGCONFIG(TAG, "  Writing: %s", this->allow_writes_ ? "enabled" : "disabled");
  if (this->variant_ == DIETRICH_VARIANT_PCU05_P3 && this->hydro_pressure_sensor_ != nullptr) {
    ESP_LOGW(TAG, "  hydro_pressure is not part of the PCU-05 P3 map - verify it against the boiler display");
  }
  LOG_UPDATE_INTERVAL(this);
  this->check_uart_settings(9600);
}

}  // namespace dietrich
}  // namespace esphome
