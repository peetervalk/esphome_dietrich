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
// 16 bytes per reply. Byte 2 is 0x00, the same address the counter reads use.
//
// 0x00 is nominally the PSU and 0x01 the PCU, and both answer READ_EPROM_BLOCK
// echoing back whichever address they were sent - but not with the same bytes.
// On a PCU-05 P3, 0x00 returns the real parameter image for all eight blocks,
// while 0x01 returns sixteen FF bytes for every block except 0x16, whose contents
// are the image this component's own earlier single-block writes left there. The
// parameter EEPROM the boiler actually runs on is the one at 0x00, so the whole
// EEPROM path - read, write and verify - is addressed there. Service mode stays
// at 0x01, which is where it is observed to engage. See
// mapping/pcu05_p3_protocol.md.
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
// Recom addresses these to the PCU at 0x01, and that is where the unlock is
// observable: sample byte 63 tracks it. The EEPROM, though, is at 0x00, and the two
// do not share a service-mode flag - unlocking 0x01 alone and writing to 0x00 is
// answered with a NAK. So a write transaction unlocks both and re-locks both.
static const uint8_t CMD_SERVICE_ON_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x08, 0x0C, 0xAE, 0xCE, 0x03};
static const uint8_t CMD_SERVICE_OFF_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x1F, 0x0C, 0xA1, 0x3E, 0x03};

// The same pair addressed to 0x00, where the parameter EEPROM answers. Unlocking
// only 0x01 and then writing to 0x00 got the write frame parsed in full and
// refused with a NAK, so the two addresses keep their own service state and each
// one has to be unlocked for its own sake.
static const uint8_t CMD_SERVICE_ON_REMEHA_EE[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x08, 0x0C, 0x93, 0x0E, 0x03};
static const uint8_t CMD_SERVICE_OFF_REMEHA_EE[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x1F, 0x0C, 0x9C, 0xFE, 0x03};

// CODE_FACTORY_COMMANDO, COMMAND 0x09 with EXT_COMMAND 0x52 (CODE_FACTORY = 82),
// and the re-lock built the same way CODE_SERVICE_STOP is - COMMAND 0x1F with the
// factory EXT byte instead of the service one.
//
// This is the unlock level Recom reaches after the 0012 PIN, the one where dF/dU
// become editable. No version of this component has ever sent it: every write it
// has made used CODE_SERVICE (0x08/0x0C), which this board ACKs, stores and then
// declines to adopt, leaving the PCU running its old set and raising Blocking 0
// at the next identification. Recom writing p25..p28 to this same appliance from
// factory level changed them cleanly, with no blocking at all.
//
// COMMAND 0x09 and EXT 0x52 are both out of Recom's own tables. Their pairing is
// inferred from the symmetry with 0x08/0x0C, and the re-lock is a guess; the
// board may well NAK one or both. That is why test_factory_mode() exists and why
// send_command() will build any other combination you want to try.
static const uint8_t CMD_FACTORY_ON_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x09, 0x52, 0x2E, 0xA6, 0x03};
static const uint8_t CMD_FACTORY_OFF_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x1F, 0x52, 0x20, 0xC6, 0x03};
static const uint8_t CMD_FACTORY_ON_REMEHA_EE[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x09, 0x52, 0x13, 0x66, 0x03};
static const uint8_t CMD_FACTORY_OFF_REMEHA_EE[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x1F, 0x52, 0x1D, 0x06, 0x03};

// RESET, COMMAND 0x31 with EXT_COMMAND NONE, addressed to the PCU. Recom's
// command table has it; nothing in the decompiled write path sends it, and this
// board has never been asked for it, so what it does is unverified - a warm
// restart of the control unit is what the name and the 5 second settle times in
// PCU-05_P3.xml suggest. It exists here because a parameter write leaves a
// PCU-05 P3 blocking until it is restarted, and a mains power cycle is currently
// the only known cure. Sent alone, by reset_board() only, never as part of a
// write. See mapping/pcu05_p3_protocol.md, *What cleared it*.
static const uint8_t CMD_RESET_REMEHA[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x31, 0x00, 0xBC, 0x9B, 0x03};

// IDENTIFICATION, COMMAND 0x01 with EXT_COMMAND 0x0B. Recom sends this when it
// connects, before anything else. It is a plain read: no service mode, no payload,
// nothing written.
//
// The `identification` node of PCU-05_P3.xml holds four layouts. Groups 2, 3 and 4
// are one device's own identity - device type, versions, operating hours, connected
// device types, last blocking and locking codes - and a PCU-05 P3 answers at 0x01
// with exactly that, 16 data bytes, group 2's shape. Group 1 is the appliance
// identity instead, 64 bytes, and it is the only one carrying the dF/dU codes, the
// serial number and the boiler name. Nothing says which device serves it, so both
// addresses are asked and the reply is decoded by its length. See
// mapping/pcu05_p3_protocol.md.
static const uint8_t CMD_IDENT_REMEHA_PCU[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x01, 0x0B, 0xE9, 0x5C, 0x03};
static const uint8_t CMD_IDENT_REMEHA_PSU[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x01, 0x0B, 0xD4, 0x9C, 0x03};

// Avanta protocol (protocol.nr 2), XOR checksum, 6 byte response header
static const uint8_t CMD_SAMPLE_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x02, 0x00, 0x53, 0x03};
static const uint8_t CMD_COUNTER1_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x10, 0x01, 0x40, 0x03};
static const uint8_t CMD_COUNTER2_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x10, 0x02, 0x43, 0x03};

// byte 3 of a Remeha frame: 0x05 in a request, 0x06 in a response
static const uint8_t REMEHA_TYPE_RESPONSE = 0x06;
// and 0x15 when the board understood the frame and is refusing it. Recom only
// ever asks IsAcknowledged() ([3] == 6) and treats everything else alike, but the
// difference matters here: a NAK means the frame reached the right device, parsed,
// and was turned down - quite unlike silence or a mangled reply.
static const uint8_t REMEHA_TYPE_NAK = 0x15;
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
// Settle time after a block write, rather than the 60 ms a read gets. Eight
// EEPROM program cycles back to back inside half a second is this component's
// invention, not Recom's: Recom allows a full second for each write's ACK and is
// driven by a human in a dialog box on top of that. The board has never been
// asked to take them any faster than this.
static const uint32_t WRITE_SETTLE_MS = 250;
// Resends allowed for a re-lock step that goes unanswered. Only the re-lock gets
// them: CODE_SERVICE_STOP carries no payload and re-locking an address that is
// already locked is a no-op, so a repeat costs nothing, while walking away from
// an unanswered one leaves the boiler unlocked. A read or a write is not retried
// - see the note on Recom's timed-out writes in mapping/pcu05_p3_protocol.md.
static const uint8_t RELOCK_RETRIES = 1;
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

// Every documented parameter of the P3 map with a range narrow enough to be worth
// checking, used to sanity-check a freshly read image before any of it is handed
// back to the boiler. This is the opposite list from PARAM_LIMITS above: it exists
// precisely to cover the parameters this component will never write - the gas/air
// settings and the controller-protection limits - because a full-block write
// returns them verbatim and a corrupted read would otherwise carry them into
// EEPROM.
//
// `A x 100` parameters are stored divided by 100, so a documented 1000..10000 is a
// stored 10..100. Verified against a live PCU-05 P3 image: all 58 entries fall
// inside their range.
static const ParamLimit PARAM_SANITY[] = {
    {1, 0, 20, 90},      {2, 1, 40, 65},      {3, 2, 0, 3},        {4, 3, 0, 2},
    {5, 4, 0, 99},       {17, 16, 10, 100},   {18, 17, 10, 100},   {19, 18, 10, 50},
    {20, 19, 0, 99},     {21, 20, 10, 50},    {23, 22, 20, 90},    {24, 23, 1, 255},
    {25, 24, 0, 30},     {26, 25, 0, 90},     {28, 27, 2, 10},     {29, 28, 2, 10},
    {31, 30, 0, 2},      {32, 31, 0, 25},     {33, 32, 2, 15},     {34, 33, 0, 1},
    {35, 34, 0, 3},      {36, 35, 1, 3},      {37, 36, 0, 1},      {38, 37, 0, 1},
    {40, 39, 0, 2},      {41, 40, 0, 2},      {44, 43, 0, 2},      {55, 54, 20, 100},
    {56, 55, 20, 100},   {57, 56, 20, 100},   {58, 57, 0, 1},      {69, 68, 0, 60},
    {70, 69, 10, 40},    {72, 71, 0, 100},    {73, 72, 1, 10},     {74, 73, 10, 180},
    {75, 74, 1, 15},     {76, 75, 3, 15},     {77, 76, 10, 100},   {78, 77, 10, 100},
    {79, 78, 5, 40},     {80, 79, 0, 100},    {81, 80, 0, 100},    {82, 81, 0, 100},
    {85, 84, 0, 20},     {88, 87, 1, 10},     {89, 88, 0, 20},     {90, 89, 0, 20},
    {91, 90, 0, 100},    {92, 91, 0, 100},    {94, 93, 1, 99},     {95, 94, 1, 255},
    {97, 96, 0, 1},      {101, 100, 0, 99},   {105, 104, 0, 10},   {107, 106, 1, 255},
    {110, 109, 0, 110},  {112, 111, 0, 100},
};
static const size_t PARAM_SANITY_LEN = sizeof(PARAM_SANITY) / sizeof(PARAM_SANITY[0]);

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
  if (this->rx_buf_[3] == REMEHA_TYPE_NAK)
    return "the boiler refused it (NAK)";
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
  if (is_write_req_(req)) {
    // built per block by build_write_frame_(), just before it is sent
    *cmd = this->tx_buf_;
    *len = DIETRICH_WRITE_FRAME_LEN;
    return;
  }

  if (req >= DIETRICH_REQ_PARAM0) {
    const size_t blk = static_cast<size_t>(req) - DIETRICH_REQ_PARAM0;
    *cmd = CMD_PARAM_REMEHA[blk];
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
    case DIETRICH_REQ_SERVICE_ON_EE:
      *cmd = CMD_SERVICE_ON_REMEHA_EE;
      *len = sizeof(CMD_SERVICE_ON_REMEHA_EE);
      break;
    case DIETRICH_REQ_SERVICE_OFF_EE:
      *cmd = CMD_SERVICE_OFF_REMEHA_EE;
      *len = sizeof(CMD_SERVICE_OFF_REMEHA_EE);
      break;
    case DIETRICH_REQ_RESET:
      *cmd = CMD_RESET_REMEHA;
      *len = sizeof(CMD_RESET_REMEHA);
      break;
    case DIETRICH_REQ_FACTORY_ON:
      *cmd = CMD_FACTORY_ON_REMEHA;
      *len = sizeof(CMD_FACTORY_ON_REMEHA);
      break;
    case DIETRICH_REQ_FACTORY_OFF:
      *cmd = CMD_FACTORY_OFF_REMEHA;
      *len = sizeof(CMD_FACTORY_OFF_REMEHA);
      break;
    case DIETRICH_REQ_FACTORY_ON_EE:
      *cmd = CMD_FACTORY_ON_REMEHA_EE;
      *len = sizeof(CMD_FACTORY_ON_REMEHA_EE);
      break;
    case DIETRICH_REQ_FACTORY_OFF_EE:
      *cmd = CMD_FACTORY_OFF_REMEHA_EE;
      *len = sizeof(CMD_FACTORY_OFF_REMEHA_EE);
      break;
    case DIETRICH_REQ_RAW:
      *cmd = this->raw_buf_;
      *len = this->raw_len_;
      break;
    case DIETRICH_REQ_IDENT_PCU:
      *cmd = CMD_IDENT_REMEHA_PCU;
      *len = sizeof(CMD_IDENT_REMEHA_PCU);
      break;
    case DIETRICH_REQ_IDENT_PSU:
      *cmd = CMD_IDENT_REMEHA_PSU;
      *len = sizeof(CMD_IDENT_REMEHA_PSU);
      break;
    case DIETRICH_REQ_DUMP:
      // built per block by build_read_frame_(), just before it is sent, the same
      // way a write frame is - so response_error_() checks the echoed block index
      // against it for free
      *cmd = this->tx_buf_;
      *len = DIETRICH_READ_FRAME_LEN;
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

  // The two samples a write transaction takes for itself, first and last in its
  // queue; see start_txn_(). The first decides whether the write goes out at all,
  // the second is what reports a blocking code the write brought on.
  if (this->txn_active_ && (this->txn_kind_ == DIETRICH_TXN_PARAM || this->txn_kind_ == DIETRICH_TXN_IDENTITY) &&
      this->have_(43, 1)) {
    if (!this->txn_saw_preflight_) {
      this->txn_saw_preflight_ = true;
      this->txn_blocking_before_ = this->d_(42);
      const char *why = "";
      if (!this->boiler_is_quiet_(&why)) {
        ESP_LOGW(TAG, "write refused: %s - state %u, sub state %u, fan %u rpm, ionisation %u", why,
                 static_cast<unsigned>(this->d_(40)), static_cast<unsigned>(this->d_(43)),
                 static_cast<unsigned>(this->have_(45, 1) ? (this->d_(44) << 8) | this->d_(45) : 0),
                 static_cast<unsigned>(this->d_(26)));
        this->txn_refused_busy_ = true;
      }
    } else {
      this->txn_saw_preflight_ = true;
      this->txn_blocking_after_ = this->d_(42);
      this->txn_have_blocking_after_ = true;
    }
  }

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

  // The same readback for the factory-level unlock, and deliberately without a
  // verdict. Byte 63 is known to track CODE_SERVICE; nothing is known to track
  // CODE_FACTORY, and byte 62 - which the P3 map calls service_mode and which has
  // never been seen to move - is the obvious candidate but only a candidate. So
  // report both bytes and let the reading decide what the command did. If byte 62
  // moves here, that is the finding.
  if (this->txn_active_ && this->txn_kind_ == DIETRICH_TXN_FACTORY_TEST && this->have_(63, 1)) {
    ESP_LOGI(TAG,
             "factory mode readback: byte 62 = %u, byte 63 = %u (62 is the candidate flag, 63 is the one "
             "CODE_SERVICE moves)",
             static_cast<unsigned>(this->d_(62)), static_cast<unsigned>(this->d_(63)));
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
      this->stage_txn_(DIETRICH_TXN_RELOCK, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, 0, "boot re-lock");
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

// A run of bytes read as text; the map marks these `number="4" argument="16"`,
// sixteen characters. Unset ones come back as 0x00 or 0xFF padding.
std::string Dietrich::text_(size_t off, size_t len) const {
  std::string out;
  for (size_t i = 0; i < len; i++) {
    const uint8_t c = this->d_(off + i);
    if (c == 0x00 || c == 0xFF)
      break;
    out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

// The `identification` node of PCU-05_P3.xml. Which of its four layouts a reply
// carries is decided by how long the reply is, because nothing in the map says
// which device serves which group, and a PCU-05 P3 turned out not to serve the one
// the field names suggest:
//
//   groups 2-4, 16 data bytes: one device's own identity. What 0x01 actually
//     answers with - device type, versions, operating hours, connected device
//     types, last blocking and locking codes, and a 5 byte serial number.
//   group 1, 64 data bytes: the appliance identity - dF/dU codes, a 16 character
//     serial number and the boiler name. What 0x00 answers with; it is EEPROM
//     blocks 0x10..0x13 of that address verbatim.
//
// Logged rather than published: commissioning data, read once against the
// identification plate, not a measurement.
void Dietrich::decode_identification_(uint8_t addr) {
  // group 1 is the only layout with anything at byte 48, so its length gives it away
  if (this->have_(48, 16)) {
    ESP_LOGI(TAG, "identification 0x%02X: dF-code %u, dU-code %u (compare these with the identification plate)",
             static_cast<unsigned>(addr), static_cast<unsigned>(this->d_(1)), static_cast<unsigned>(this->d_(2)));
    ESP_LOGI(TAG, "  software version %u, parameter version %u, parameter type %u (raw bytes)",
             static_cast<unsigned>(this->d_(5)), static_cast<unsigned>(this->d_(6)),
             static_cast<unsigned>(this->d_(7)));
    ESP_LOGI(TAG, "  next service code %u, connected PSU type %u, connected PCU type %u, SCU-C %u",
             static_cast<unsigned>(this->d_(10)), static_cast<unsigned>(this->d_(16)),
             static_cast<unsigned>(this->d_(17)), static_cast<unsigned>(this->d_(18)));
    ESP_LOGI(TAG, "  serial number: %s", this->text_(32, 16).c_str());
    ESP_LOGI(TAG, "  boiler name: %s", this->text_(48, 16).c_str());
    return;
  }

  if (!this->have_(10, 1)) {
    ESP_LOGW(TAG, "identification 0x%02X carries only %u data bytes, too short to decode",
             static_cast<unsigned>(addr), static_cast<unsigned>(this->data_len_));
    return;
  }

  // Operating hours is `(A.1 + B.0) x N`, the same big-endian pair the counter
  // blocks use, and N is the one difference between group 2 and group 3: the PCU
  // counts in twos, the SU and PSU in eights. The address is what tells them apart.
  const bool pcu = addr == 0x01;
  const uint32_t hours = static_cast<uint32_t>((this->d_(4) << 8) | this->d_(5)) * (pcu ? 2u : 8u);
  ESP_LOGI(TAG, "identification 0x%02X: device type %u, software version %u, parameter version %u, type %u",
           static_cast<unsigned>(addr), static_cast<unsigned>(this->d_(0)), static_cast<unsigned>(this->d_(1)),
           static_cast<unsigned>(this->d_(2)), static_cast<unsigned>(this->d_(3)));
  ESP_LOGI(TAG, "  operating hours %u, connected %s type %u, connected PSU type %u",
           static_cast<unsigned>(hours), pcu ? "SU" : "PCU", static_cast<unsigned>(this->d_(6)),
           static_cast<unsigned>(this->d_(7)));
  // "Last" as the map names them: the previous fault, not the one in the sample.
  ESP_LOGI(TAG, "  last blocking code %u, last locking code %u", static_cast<unsigned>(this->d_(8)),
           static_cast<unsigned>(this->d_(9)));
  if (this->have_(16, 0)) {
    // Serial number is `number="5"` at byte 11 - five bytes in a format the IL does
    // not give up, so it goes out as hex rather than guessed at. All FF means unset.
    ESP_LOGI(TAG, "  serial number (raw): %02X %02X %02X %02X %02X", this->d_(11), this->d_(12), this->d_(13),
             this->d_(14), this->d_(15));
  }
}

// Hex and ASCII, one line per block. The ASCII column is the point: a boiler name
// or a serial number is what an unmapped block would give itself away by.
void Dietrich::log_eeprom_block_() const {
  size_t n = this->data_len_;
  if (n > DIETRICH_PARAM_BLOCK_SIZE)
    n = DIETRICH_PARAM_BLOCK_SIZE;

  char ascii[DIETRICH_PARAM_BLOCK_SIZE + 1];
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = this->d_(i);
    ascii[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
  }
  ascii[n] = 0;

  ESP_LOGI(TAG, "eeprom %02X:%02X %s |%s|", static_cast<unsigned>(this->dump_addr_),
           static_cast<unsigned>(this->dump_block_),
           hex_str_(this->rx_buf_ + this->header_len_(), n).c_str(), ascii);
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
  // A raw frame may well be a write, and an unknown command may take as long as
  // one, so give it the longer budget rather than calling it a timeout early.
  if (req == DIETRICH_REQ_RAW)
    return WRITE_TIMEOUT_MS;
  return is_write_req_(req) ? WRITE_TIMEOUT_MS : RESPONSE_TIMEOUT_MS;
}

bool Dietrich::is_write_req_(DietrichRequest req) {
  return req >= DIETRICH_REQ_WRITE0 && req < DIETRICH_REQ_PARAM0;
}

uint8_t Dietrich::block_of_(DietrichRequest req) {
  const uint8_t base = is_write_req_(req) ? DIETRICH_REQ_WRITE0 : DIETRICH_REQ_PARAM0;
  return static_cast<uint8_t>(DIETRICH_PARAM_FIRST_BLOCK + (req - base));
}

// 02 | FE | 00 | 05 | 18 | 11 | blk | 16 data bytes | CRClo CRChi | 03
//
// The payload comes from txn_image_, which begin_write_phase_() assembled out of
// the blocks this transaction read for itself plus the one staged edit.
// 02 | FE | addr | 05 | 08 | 10 | blk | CRClo CRChi | 03
void Dietrich::build_read_frame_(uint8_t addr, uint8_t block) {
  this->tx_buf_[0] = 0x02;
  this->tx_buf_[1] = 0xFE;  // sender: the PC
  this->tx_buf_[2] = addr;
  this->tx_buf_[3] = 0x05;  // request
  this->tx_buf_[4] = static_cast<uint8_t>(DIETRICH_READ_FRAME_LEN - 2);
  this->tx_buf_[5] = 0x10;  // READ_EPROM_BLOCK
  this->tx_buf_[6] = block;

  const uint16_t crc = crc16_(this->tx_buf_, 1, DIETRICH_READ_FRAME_LEN - 3);
  this->tx_buf_[DIETRICH_READ_FRAME_LEN - 3] = static_cast<uint8_t>(crc & 0xFF);
  this->tx_buf_[DIETRICH_READ_FRAME_LEN - 2] = static_cast<uint8_t>(crc >> 8);
  this->tx_buf_[DIETRICH_READ_FRAME_LEN - 1] = 0x03;
}

void Dietrich::build_write_frame_(uint8_t block) {
  const size_t base = (static_cast<size_t>(block) - DIETRICH_PARAM_FIRST_BLOCK) * DIETRICH_PARAM_BLOCK_SIZE;

  this->tx_buf_[0] = 0x02;
  this->tx_buf_[1] = 0xFE;  // sender: the PC
  this->tx_buf_[2] = 0x00;  // recipient: whoever answers the parameter reads
  this->tx_buf_[3] = 0x05;  // request
  this->tx_buf_[4] = static_cast<uint8_t>(DIETRICH_WRITE_FRAME_LEN - 2);
  this->tx_buf_[5] = 0x11;  // WRITE_EPROM_BLOCK
  this->tx_buf_[6] = block;

  for (size_t i = 0; i < DIETRICH_PARAM_BLOCK_SIZE; i++)
    this->tx_buf_[7 + i] = this->txn_image_[base + i];

  const uint16_t crc = crc16_(this->tx_buf_, 1, DIETRICH_WRITE_FRAME_LEN - 3);
  this->tx_buf_[DIETRICH_WRITE_FRAME_LEN - 3] = static_cast<uint8_t>(crc & 0xFF);
  this->tx_buf_[DIETRICH_WRITE_FRAME_LEN - 2] = static_cast<uint8_t>(crc >> 8);
  this->tx_buf_[DIETRICH_WRITE_FRAME_LEN - 1] = 0x03;
}

// Every documented parameter inside the blocks about to be written must read
// inside its documented range. This is not about validating what the user asked
// for - write_param() already clamped that - it is about refusing to hand back an
// image that cannot have come off a working boiler.
//
// It matters most for a full-block write, where 127 of the 128 bytes are being
// returned verbatim and several of them are combustion settings. A block that
// arrived mangled but with a valid CRC, or a params_ buffer that never got filled,
// shows up here as a parameter outside its range.
bool Dietrich::image_is_sane_() const {
  const size_t lo =
      (static_cast<size_t>(this->txn_first_block_) - DIETRICH_PARAM_FIRST_BLOCK) * DIETRICH_PARAM_BLOCK_SIZE;
  const size_t hi = lo + static_cast<size_t>(this->txn_block_count_) * DIETRICH_PARAM_BLOCK_SIZE;

  for (size_t i = 0; i < PARAM_SANITY_LEN; i++) {
    const ParamLimit &s = PARAM_SANITY[i];
    if (s.offset < lo || s.offset >= hi)
      continue;
    const uint8_t v = this->txn_read_[s.offset];
    if (v < s.min || v > s.max) {
      // the first bad byte is enough: report it and stop, rather than walking the
      // rest of the table logging a line per parameter and blocking the loop
      ESP_LOGE(TAG, "refusing to write: p%u (byte %u) reads %u, outside its documented %u..%u",
               static_cast<unsigned>(s.param), static_cast<unsigned>(s.offset), static_cast<unsigned>(v),
               static_cast<unsigned>(s.min), static_cast<unsigned>(s.max));
      return false;
    }
  }
  return true;
}

// The same check for one freshly arrived block, so a transaction gives up on the
// block that came back wrong instead of reading all eight and only then finding
// out. Sixteen FF bytes where a parameter block should be is exactly what a read
// aimed at the wrong device address looks like.
bool Dietrich::block_is_sane_(size_t blk) const {
  const size_t lo = blk * DIETRICH_PARAM_BLOCK_SIZE;
  const size_t hi = lo + DIETRICH_PARAM_BLOCK_SIZE;

  for (size_t i = 0; i < PARAM_SANITY_LEN; i++) {
    const ParamLimit &s = PARAM_SANITY[i];
    if (s.offset < lo || s.offset >= hi)
      continue;
    const uint8_t v = this->txn_read_[s.offset];
    if (v < s.min || v > s.max) {
      ESP_LOGE(TAG, "block 0x%02X read back implausible: p%u (byte %u) is %u, outside its documented %u..%u",
               static_cast<unsigned>(DIETRICH_PARAM_FIRST_BLOCK + blk), static_cast<unsigned>(s.param),
               static_cast<unsigned>(s.offset), static_cast<unsigned>(v), static_cast<unsigned>(s.min),
               static_cast<unsigned>(s.max));
      return false;
    }
  }
  return true;
}

bool Dietrich::begin_write_phase_() {
  const uint8_t first = static_cast<uint8_t>(this->txn_first_block_ - DIETRICH_PARAM_FIRST_BLOCK);
  uint8_t need = 0;
  for (uint8_t i = 0; i < this->txn_block_count_; i++)
    need = static_cast<uint8_t>(need | (1u << (first + i)));

  // Nothing is written unless this transaction read every block it is about to
  // write, whole and CRC-valid. Otherwise the bytes going back would be whatever
  // happened to be left in params_.
  if ((this->txn_blocks_read_ & need) != need) {
    ESP_LOGE(TAG, "refusing to write: blocks read mask 0x%02X, needed 0x%02X",
             static_cast<unsigned>(this->txn_blocks_read_), static_cast<unsigned>(need));
    this->txn_failed_ = true;
    this->skip_to_relock_();
    return false;
  }

  if (!this->image_is_sane_()) {
    this->txn_failed_ = true;
    this->skip_to_relock_();
    return false;
  }

  memcpy(this->txn_image_, this->txn_read_, DIETRICH_PARAM_BYTES);

  if (this->txn_kind_ == DIETRICH_TXN_PARAM) {
    // A parameter that already holds the wanted value is not worth an EEPROM
    // cycle, and endurance is finite. The check waits until here because it needs
    // the block this transaction just read, not a stale copy from the sweep.
    if (this->txn_read_[this->pending_byte_] == this->pending_value_) {
      ESP_LOGI(TAG, "parameter already reads %u, skipping the write", static_cast<unsigned>(this->pending_value_));
      this->skip_to_relock_();
      return false;
    }
    this->txn_image_[this->pending_byte_] = this->pending_value_;
  }

  return true;
}

bool Dietrich::stage_txn_(DietrichTxn txn, uint8_t first_block, uint8_t block_count, uint8_t byte_offset,
                          uint8_t value, const char *what) {
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
  this->pending_first_block_ = first_block;
  this->pending_block_count_ = block_count;
  this->pending_byte_ = byte_offset;
  this->pending_value_ = value;
  ESP_LOGI(TAG, "%s queued", what);
  return true;
}

bool Dietrich::dump_eeprom(uint8_t addr, uint8_t first, uint8_t count) {
  if (this->variant_ != DIETRICH_VARIANT_PCU05_P3) {
    ESP_LOGW(TAG, "eeprom dump is only supported on variant pcu05_p3");
    return false;
  }
  if (addr != 0x00 && addr != 0x01) {
    ESP_LOGW(TAG, "eeprom dump: 0x%02X is not a device address, use 0x00 or 0x01",
             static_cast<unsigned>(addr));
    return false;
  }
  if (this->dump_active_ || this->txn_active_ || this->pending_txn_ != DIETRICH_TXN_NONE) {
    ESP_LOGW(TAG, "eeprom dump: the bus is busy, try again when it is not");
    return false;
  }
  if (count == 0)
    count = 1;

  this->dump_addr_ = addr;
  this->dump_block_ = first;
  uint16_t end = static_cast<uint16_t>(first) + count;
  if (end > DIETRICH_EEPROM_BLOCKS)
    end = DIETRICH_EEPROM_BLOCKS;
  this->dump_end_ = end;
  this->dump_active_ = true;
  ESP_LOGI(TAG, "eeprom dump of 0x%02X queued: blocks 0x%02X..0x%02X, read-only",
           static_cast<unsigned>(addr), static_cast<unsigned>(first),
           static_cast<unsigned>(end - 1));
  return true;
}

bool Dietrich::read_identification() {
  if (this->variant_ != DIETRICH_VARIANT_PCU05_P3) {
    ESP_LOGW(TAG, "identification is only supported on variant pcu05_p3");
    return false;
  }
  // Read-only, so unlike the write API this is not gated behind allow_writes and
  // does not need the bus to itself - it just takes the next poll interval.
  this->pending_ident_ = true;
  ESP_LOGI(TAG, "identification queued for the next poll interval");
  return true;
}

// A parameter write rewrites all 128 bytes of the parameter block, including the
// setpoints and control factors the board is using at that moment. Doing that to
// a boiler in the middle of a burn is asking for trouble on general principle,
// and on 2026-09-16 the one write that went out during a DHW charge left a
// PCU-05 P3 in Blocking 0 that two hours and a power cycle would not clear, while
// the one sent to a stopped boiler produced a blocking that a power cycle did
// clear. That is one observation of each and not a controlled experiment, so this
// is a precaution, not a proven fix - see mapping/pcu05_p3_protocol.md.
bool Dietrich::boiler_is_quiet_(const char **why) const {
  // Standby, a controlled stop, and the two fault modes. A board that is already
  // blocking or locked is not going to light, and writing to one in that state is
  // exactly how the p33 write was undone.
  static const uint8_t QUIET_STATUS[] = {0, 8, 9, 10};
  // Standby, the anti-cycle wait, and the reset wait. Everything else - purging,
  // igniting, burning, stopping, pump post-run - is part-way through a cycle.
  static const uint8_t QUIET_SUBSTATUS[] = {0, 1, 255};

  if (!this->have_(45, 1)) {
    *why = "no sample has said what the boiler is doing";
    return false;
  }

  bool ok = false;
  for (size_t i = 0; i < sizeof(QUIET_STATUS); i++)
    ok = ok || this->d_(40) == QUIET_STATUS[i];
  if (!ok) {
    *why = "the boiler is running";
    return false;
  }

  ok = false;
  for (size_t i = 0; i < sizeof(QUIET_SUBSTATUS); i++)
    ok = ok || this->d_(43) == QUIET_SUBSTATUS[i];
  if (!ok) {
    *why = "the boiler is part-way through a cycle";
    return false;
  }

  if (this->d_(44) != 0 || this->d_(45) != 0) {
    *why = "the fan is still turning";
    return false;
  }
  if (this->d_(26) != 0) {
    *why = "there is still a flame";
    return false;
  }
  return true;
}

bool Dietrich::reset_board() {
  return this->stage_txn_(DIETRICH_TXN_RESET, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, 0, "board reset (COMMAND 0x31)");
}

bool Dietrich::test_service_mode() {
  return this->stage_txn_(DIETRICH_TXN_SERVICE_TEST, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, 0, "service mode test");
}

bool Dietrich::test_factory_mode() {
  return this->stage_txn_(DIETRICH_TXN_FACTORY_TEST, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, 0,
                          "factory mode test (COMMAND 0x09 / EXT 0x52)");
}

// Hex text to bytes. Spaces, colons and dashes are skipped so a frame can be
// pasted in whatever shape it was written down in; anything else is an error,
// and so is an odd number of digits.
bool Dietrich::parse_hex_(const std::string &hex, uint8_t *out, size_t max, size_t *len) {
  size_t n = 0;
  int hi = -1;
  for (size_t i = 0; i < hex.size(); i++) {
    const char c = hex[i];
    if (c == ' ' || c == ':' || c == '-' || c == '\t')
      continue;
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      return false;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= max)
        return false;
      out[n++] = static_cast<uint8_t>((hi << 4) | v);
      hi = -1;
    }
  }
  if (hi >= 0)
    return false;
  *len = n;
  return true;
}

// 02 | FE | dst | 05 | len | cmd | ext | data.. | CRClo CRChi | 03
//
// The length byte and the CRC are computed here, so the only things that can be
// wrong are the ones being tested: the recipient, the command and its payload.
bool Dietrich::build_and_stage_command_(uint8_t dst, uint8_t cmd, uint8_t ext, const uint8_t *data,
                                        size_t data_len) {
  const size_t len = 7 + data_len + 3;  // header + data + CRC16 + ETX
  if (len > DIETRICH_RAW_FRAME_MAX) {
    ESP_LOGW(TAG, "send_command refused: %u data bytes makes a frame longer than the %u byte limit",
             static_cast<unsigned>(data_len), static_cast<unsigned>(DIETRICH_RAW_FRAME_MAX));
    return false;
  }

  uint8_t f[DIETRICH_RAW_FRAME_MAX];
  f[0] = 0x02;
  f[1] = 0xFE;  // sender: the PC
  f[2] = dst;
  f[3] = 0x05;  // request
  f[4] = static_cast<uint8_t>(len - 2);
  f[5] = cmd;
  f[6] = ext;
  memcpy(f + 7, data, data_len);
  const uint16_t crc = crc16_(f, 1, len - 3);
  f[len - 3] = static_cast<uint8_t>(crc & 0xFF);
  f[len - 2] = static_cast<uint8_t>(crc >> 8);
  f[len - 1] = 0x03;

  char what[72];
  snprintf(what, sizeof(what), "raw COMMAND 0x%02X / EXT 0x%02X -> 0x%02X", static_cast<unsigned>(cmd),
           static_cast<unsigned>(ext), static_cast<unsigned>(dst));
  if (!this->stage_txn_(DIETRICH_TXN_RAW, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, 0, what))
    return false;

  // Only after stage_txn_() has agreed to it, so a refused call cannot overwrite
  // the frame a transaction already in flight is sending.
  memcpy(this->raw_buf_, f, len);
  this->raw_len_ = len;
  ESP_LOGI(TAG, "  frame: %s", hex_str_(f, len).c_str());
  return true;
}

bool Dietrich::send_command(uint8_t dst, uint8_t cmd, uint8_t ext, const std::string &data_hex) {
  uint8_t data[DIETRICH_RAW_FRAME_MAX];
  size_t data_len = 0;
  if (!parse_hex_(data_hex, data, sizeof(data), &data_len)) {
    ESP_LOGW(TAG, "send_command refused: \"%s\" is not an even-length run of hex bytes", data_hex.c_str());
    return false;
  }
  return this->build_and_stage_command_(dst, cmd, ext, data, data_len);
}

// "01 09 52" or "01 37 00 0C 00" - recipient, command, ext, then the payload.
bool Dietrich::send_command_hex(const std::string &spec) {
  uint8_t b[DIETRICH_RAW_FRAME_MAX];
  size_t n = 0;
  if (!parse_hex_(spec, b, sizeof(b), &n)) {
    ESP_LOGW(TAG, "send_command refused: \"%s\" is not an even-length run of hex bytes", spec.c_str());
    return false;
  }
  if (n < 3) {
    ESP_LOGW(TAG, "send_command refused: \"%s\" needs at least recipient, command and ext - e.g. \"01 09 52\"",
             spec.c_str());
    return false;
  }
  return this->build_and_stage_command_(b[0], b[1], b[2], b + 3, n - 3);
}

// The bytes exactly as given, for replaying something captured off Recom. A bad
// CRC or bad framing is reported and sent anyway - a frame the board refuses is
// a result, and refusing to send it here would only hide that.
bool Dietrich::send_raw(const std::string &hex) {
  uint8_t f[DIETRICH_RAW_FRAME_MAX];
  size_t len = 0;
  if (!parse_hex_(hex, f, sizeof(f), &len)) {
    ESP_LOGW(TAG, "send_raw refused: \"%s\" is not an even-length run of hex bytes, or is over %u bytes",
             hex.c_str(), static_cast<unsigned>(DIETRICH_RAW_FRAME_MAX));
    return false;
  }
  if (len < REMEHA_MIN_FRAME) {
    ESP_LOGW(TAG, "send_raw refused: %u bytes is shorter than the %u byte minimum frame",
             static_cast<unsigned>(len), static_cast<unsigned>(REMEHA_MIN_FRAME));
    return false;
  }

  if (!this->stage_txn_(DIETRICH_TXN_RAW, DIETRICH_PARAM_FIRST_BLOCK, 0, 0, 0, "raw frame"))
    return false;

  if (f[0] != 0x02 || f[len - 1] != 0x03)
    ESP_LOGW(TAG, "  note: this frame does not start with 0x02 and end with 0x03");
  if (crc16_(f, 1, len - 3) != static_cast<uint16_t>(f[len - 3] | (f[len - 2] << 8)))
    ESP_LOGW(TAG, "  note: the CRC in this frame is not the one this component would compute - sending it as given");

  memcpy(this->raw_buf_, f, len);
  this->raw_len_ = len;
  ESP_LOGI(TAG, "  frame: %s", hex_str_(f, len).c_str());
  return true;
}

// Everything response_error_() would have thrown the frame out for, reported
// instead. Each line is a fact about the reply, so a guessed command that comes
// back NAKed still tells you the board parsed it and knew what it was.
void Dietrich::log_raw_reply_() const {
  if (this->rx_len_ == 0) {
    ESP_LOGW(TAG, "raw frame: no reply at all - the board did not answer inside %u ms",
             static_cast<unsigned>(WRITE_TIMEOUT_MS));
    return;
  }
  if (this->rx_len_ < REMEHA_MIN_FRAME) {
    ESP_LOGW(TAG, "raw frame: reply is %u bytes, short of the %u byte minimum frame - nothing to decode",
             static_cast<unsigned>(this->rx_len_), static_cast<unsigned>(REMEHA_MIN_FRAME));
    return;
  }

  const uint8_t type = this->rx_buf_[3];
  const char *verdict = type == REMEHA_TYPE_RESPONSE
                            ? "a response (type 0x06)"
                            : (type == REMEHA_TYPE_NAK ? "a NAK (type 0x15) - parsed and refused"
                                                       : "neither a response nor a NAK");
  const bool framed = this->rx_buf_[0] == 0x02 && this->rx_buf_[this->rx_len_ - 1] == 0x03;
  const bool crc_ok = is_valid_crc_(this->rx_buf_, this->rx_len_);
  const bool swapped =
      this->raw_len_ >= 3 && this->rx_buf_[1] == this->raw_buf_[2] && this->rx_buf_[2] == this->raw_buf_[1];
  const bool echoed =
      this->raw_len_ >= 7 && this->rx_buf_[5] == this->raw_buf_[5] && this->rx_buf_[6] == this->raw_buf_[6];

  ESP_LOGI(TAG, "raw frame: %s; framing %s, CRC %s, addresses %s, COMMAND/EXT %s", verdict, framed ? "ok" : "BAD",
           crc_ok ? "ok" : "BAD", swapped ? "swapped" : "NOT swapped", echoed ? "echoed" : "NOT echoed");

  const size_t overhead = this->header_len_() + this->trailer_len_();
  if (this->rx_len_ > overhead) {
    ESP_LOGI(TAG, "  %u data byte(s): %s", static_cast<unsigned>(this->rx_len_ - overhead),
             hex_str_(this->rx_buf_ + this->header_len_(), this->rx_len_ - overhead).c_str());
  } else {
    ESP_LOGI(TAG, "  no data bytes - a bare acknowledgement");
  }
}

bool Dietrich::write_block_unchanged(uint8_t block) {
  char what[48];
  snprintf(what, sizeof(what), "identity write of block 0x%02X", static_cast<unsigned>(block));
  if (block < DIETRICH_PARAM_FIRST_BLOCK || block > DIETRICH_PARAM_LAST_BLOCK) {
    ESP_LOGW(TAG, "%s refused: outside the parameter block 0x%02X..0x%02X", what,
             static_cast<unsigned>(DIETRICH_PARAM_FIRST_BLOCK), static_cast<unsigned>(DIETRICH_PARAM_LAST_BLOCK));
    return false;
  }
  return this->stage_txn_(DIETRICH_TXN_IDENTITY, block, 1, 0, 0, what);
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
  // The whole parameter block, as Recom does: SetParameterModel never writes a
  // single block, it always sends 0x14..0x1B in one service-mode session, and a
  // PCU-05 P3 ACKs a lone block write and then ignores it. See
  // mapping/pcu05_p3_protocol.md.
  return this->stage_txn_(DIETRICH_TXN_PARAM, DIETRICH_PARAM_FIRST_BLOCK, DIETRICH_PARAM_BLOCKS, lim->offset,
                          clamped, what);
}

void Dietrich::start_txn_() {
  this->txn_kind_ = this->pending_txn_;
  this->txn_first_block_ = this->pending_first_block_;
  this->txn_block_count_ = this->pending_block_count_;
  this->pending_txn_ = DIETRICH_TXN_NONE;
  this->txn_active_ = true;
  this->txn_failed_ = false;
  this->txn_relock_failed_ = false;
  this->txn_relock_tries_ = 0;
  this->txn_wrote_ = false;
  this->txn_blocks_read_ = 0;
  this->txn_saw_preflight_ = false;
  this->txn_refused_busy_ = false;
  this->txn_blocking_before_ = 0xFF;
  this->txn_have_blocking_after_ = false;
  this->txn_blocking_after_ = 0xFF;
  this->queue_pos_ = 0;

  const uint8_t first = static_cast<uint8_t>(this->txn_first_block_ - DIETRICH_PARAM_FIRST_BLOCK);
  const uint8_t count = this->txn_block_count_;

  uint8_t n = 0;
  this->txn_relock_pos_ = 0;
  switch (this->txn_kind_) {
    case DIETRICH_TXN_RELOCK:
      this->txn_relock_pos_ = n;
      this->queue_[n++] = DIETRICH_REQ_SERVICE_OFF_EE;
      this->queue_[n++] = DIETRICH_REQ_SERVICE_OFF;
      // A boot that finds the board unlocked cannot tell which level left it
      // that way - sample byte 63 tracks the service unlock and nothing is known
      // to track the factory one. When writes go out at factory level, put both
      // levels back on both addresses rather than guess.
      if (this->use_factory_mode_) {
        this->queue_[n++] = DIETRICH_REQ_FACTORY_OFF_EE;
        this->queue_[n++] = DIETRICH_REQ_FACTORY_OFF;
      }
      break;
    case DIETRICH_TXN_FACTORY_TEST:
      // The exact shape of TXN_SERVICE_TEST, one level up, and both addresses:
      // which of them answers COMMAND 0x09 at all is the first thing worth
      // knowing. The sample in the middle is the only way to see whether the
      // unlock did anything - byte 62 is the candidate flag, byte 63 being the
      // one that already tracks the service unlock.
      this->queue_[n++] = DIETRICH_REQ_FACTORY_ON;
      this->queue_[n++] = DIETRICH_REQ_FACTORY_ON_EE;
      this->queue_[n++] = DIETRICH_REQ_SAMPLE;
      this->txn_relock_pos_ = n;
      this->queue_[n++] = DIETRICH_REQ_FACTORY_OFF_EE;
      this->queue_[n++] = DIETRICH_REQ_FACTORY_OFF;
      break;
    case DIETRICH_TXN_RAW:
      // One frame, nothing unlocked, nothing to put back. txn_relock_pos_ stays
      // 0, and the reply is never counted as a failure, so there is nothing to
      // skip to and nothing to retry.
      this->queue_[n++] = DIETRICH_REQ_RAW;
      break;
    case DIETRICH_TXN_SERVICE_TEST:
      this->queue_[n++] = DIETRICH_REQ_SERVICE_ON;
      // A sample taken while unlocked is the only way to find out whether the
      // unlock did anything: CODE_SERVICE_START's ACK is a bare echo with no
      // payload, so it proves the frame was understood, not that service mode is
      // on. On a PCU-05 P3 the flag is sample byte 63.
      this->queue_[n++] = DIETRICH_REQ_SAMPLE;
      this->txn_relock_pos_ = n;
      this->queue_[n++] = DIETRICH_REQ_SERVICE_OFF;
      break;
    case DIETRICH_TXN_RESET:
      // Nothing to unlock and nothing to put back: one frame, and whatever the
      // board makes of it. txn_relock_pos_ stays 0 so a failure has nowhere to
      // skip to.
      this->queue_[n++] = DIETRICH_REQ_RESET;
      break;
    default:
      // Read every block, then write every block, then read them all back. The
      // reads belong to this transaction because the bytes they return go
      // straight back to the boiler.
      //
      // Both addresses are unlocked. 0x01 is where the service flag is visible in
      // the sample, and 0x00 is where the EEPROM is; unlocking only 0x01 and then
      // writing to 0x00 earns a NAK.
      //
      // The sample that opens the queue is the write's pre-flight check: a
      // parameter write goes out only when the boiler is quiet, and a sample from
      // the last poll can be 15 seconds stale, which is long enough for a burner
      // to have started. The one that closes it is the post-mortem - a PCU-05 P3
      // answers a parameter write by going into blocking mode within 15 seconds,
      // and a transaction that reported "verified" and said nothing about that is
      // how this component came to brick a boiler for three hours on 2026-09-16.
      // See mapping/pcu05_p3_protocol.md, *What cleared it*.
      this->queue_[n++] = DIETRICH_REQ_SAMPLE;
      // Service level or factory level, never both: Recom asks for the PIN and is
      // then in factory level, it does not hold two unlocks at once. Which one
      // this is comes from use_factory_mode on the component.
      this->queue_[n++] = this->use_factory_mode_ ? DIETRICH_REQ_FACTORY_ON : DIETRICH_REQ_SERVICE_ON;
      this->queue_[n++] = this->use_factory_mode_ ? DIETRICH_REQ_FACTORY_ON_EE : DIETRICH_REQ_SERVICE_ON_EE;
      for (uint8_t i = 0; i < count; i++)
        this->queue_[n++] = static_cast<DietrichRequest>(DIETRICH_REQ_PARAM0 + first + i);
      for (uint8_t i = 0; i < count; i++)
        this->queue_[n++] = static_cast<DietrichRequest>(DIETRICH_REQ_WRITE0 + first + i);
      for (uint8_t i = 0; i < count; i++)
        this->queue_[n++] = static_cast<DietrichRequest>(DIETRICH_REQ_PARAM0 + first + i);
      this->txn_relock_pos_ = n;
      this->queue_[n++] = this->use_factory_mode_ ? DIETRICH_REQ_FACTORY_OFF_EE : DIETRICH_REQ_SERVICE_OFF_EE;
      this->queue_[n++] = this->use_factory_mode_ ? DIETRICH_REQ_FACTORY_OFF : DIETRICH_REQ_SERVICE_OFF;
      this->queue_[n++] = DIETRICH_REQ_SAMPLE;
      break;
  }
  // the re-locks have to be last, and skip_to_relock_() jumps to the first of
  // them, so a failure part-way through still re-locks everything it unlocked
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
    case DIETRICH_TXN_RESET:
      kind = "board reset";
      break;
    case DIETRICH_TXN_FACTORY_TEST:
      kind = "factory mode test";
      break;
    case DIETRICH_TXN_RAW:
      kind = "raw frame";
      break;
    default:
      break;
  }

  if (this->txn_refused_busy_) {
    ESP_LOGW(TAG, "%s refused: the boiler was not quiet enough to write to. Nothing was unlocked and nothing "
                  "was written - try again once it is in standby",
             kind);
  } else if (this->txn_failed_) {
    ESP_LOGE(TAG, "%s failed; service mode was re-locked, nothing was retried", kind);
  } else if (this->txn_wrote_) {
    // txn_read_ now holds the verify reads, txn_image_ what the boiler was told
    const size_t lo =
        (static_cast<size_t>(this->txn_first_block_) - DIETRICH_PARAM_FIRST_BLOCK) * DIETRICH_PARAM_BLOCK_SIZE;
    const size_t hi = lo + static_cast<size_t>(this->txn_block_count_) * DIETRICH_PARAM_BLOCK_SIZE;
    size_t differing = 0, first_diff = 0;
    for (size_t i = lo; i < hi; i++) {
      if (this->txn_read_[i] != this->txn_image_[i]) {
        if (differing == 0)
          first_diff = i;
        differing++;
      }
    }
    if (differing == 0) {
      ESP_LOGI(TAG, "%s verified: %u block(s) from 0x%02X read back exactly as written", kind,
               static_cast<unsigned>(this->txn_block_count_), static_cast<unsigned>(this->txn_first_block_));
      // The verify reads are the freshest truth there is about these blocks, so
      // fold them into the published image instead of waiting out the next sweep.
      memcpy(this->params_ + lo, this->txn_read_ + lo, hi - lo);
      if (this->param_blocks_seen_ == 0xFF)
        this->decode_params_();
    } else {
      ESP_LOGE(TAG, "%s was ACKed but %u byte(s) read back different; first is byte %u: wrote %u, read %u", kind,
               static_cast<unsigned>(differing), static_cast<unsigned>(first_diff),
               static_cast<unsigned>(this->txn_image_[first_diff]),
               static_cast<unsigned>(this->txn_read_[first_diff]));
    }
  } else {
    ESP_LOGI(TAG, "%s finished, nothing was written", kind);
  }

  // The post-write sample, reported after the verdict because it is a different
  // question: the write can verify byte-for-byte and still leave the boiler in
  // blocking mode. A PCU-05 P3 did exactly that on 2026-09-16, twice, and stayed
  // there until the mains were cycled - see mapping/pcu05_p3_protocol.md.
  if (this->txn_have_blocking_after_ && this->txn_blocking_after_ != this->txn_blocking_before_) {
    ESP_LOGE(TAG,
             "%s: the boiler's blocking code went %u -> %u while this ran. On a PCU-05 P3 that is cleared by a "
             "mains power cycle; reset_board() is the untried alternative",
             kind, static_cast<unsigned>(this->txn_blocking_before_),
             static_cast<unsigned>(this->txn_blocking_after_));
  } else if (this->txn_have_blocking_after_) {
    ESP_LOGI(TAG, "%s: blocking code unchanged at %u", kind, static_cast<unsigned>(this->txn_blocking_after_));
  }

  // Reported separately, and after the verdict above, because it says nothing
  // about the write: a re-lock is the last thing in the queue. What it does say
  // is that an address may still be unlocked, so re-arm the boot check. That
  // check only watches sample byte 63, which tracks 0x01 - an unanswered re-lock
  // at 0x00 leaves nothing behind that this component can see.
  if (this->txn_relock_failed_) {
    ESP_LOGE(TAG, "%s: a re-lock went unanswered after %u resend(s); an address may still be unlocked", kind,
             static_cast<unsigned>(RELOCK_RETRIES));
    this->seen_sample_ = false;
  }

  this->txn_active_ = false;
  this->txn_failed_ = false;
  this->txn_relock_failed_ = false;
  this->txn_relock_tries_ = 0;
  this->txn_wrote_ = false;
  this->txn_blocks_read_ = 0;
  this->txn_saw_preflight_ = false;
  this->txn_refused_busy_ = false;
  this->txn_have_blocking_after_ = false;
  this->txn_kind_ = DIETRICH_TXN_NONE;
}

bool Dietrich::handle_response_() {
  const DietrichRequest req = this->queue_[this->queue_pos_];
  char param_what[16];
  const char *what;
  if (req >= DIETRICH_REQ_PARAM0) {
    snprintf(param_what, sizeof(param_what), "param 0x%02X", static_cast<unsigned>(block_of_(req)));
    what = param_what;
  } else if (is_write_req_(req)) {
    snprintf(param_what, sizeof(param_what), "write 0x%02X", static_cast<unsigned>(block_of_(req)));
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
      // Without these two the EEPROM address's own unlock and re-lock fell
      // through to "sample" below, so the log named the wrong request at exactly
      // the moment one of them went unanswered.
      case DIETRICH_REQ_SERVICE_ON_EE:
        what = "service mode on (EEPROM)";
        break;
      case DIETRICH_REQ_SERVICE_OFF_EE:
        what = "service mode off (EEPROM)";
        break;
      case DIETRICH_REQ_RESET:
        what = "reset";
        break;
      case DIETRICH_REQ_FACTORY_ON:
        what = "factory mode on";
        break;
      case DIETRICH_REQ_FACTORY_OFF:
        what = "factory mode off";
        break;
      case DIETRICH_REQ_FACTORY_ON_EE:
        what = "factory mode on (EEPROM)";
        break;
      case DIETRICH_REQ_FACTORY_OFF_EE:
        what = "factory mode off (EEPROM)";
        break;
      case DIETRICH_REQ_RAW:
        what = "raw frame";
        break;
      case DIETRICH_REQ_IDENT_PCU:
        what = "identification 0x01";
        break;
      case DIETRICH_REQ_IDENT_PSU:
        what = "identification 0x00";
        break;
      case DIETRICH_REQ_DUMP:
        snprintf(param_what, sizeof(param_what), "eeprom 0x%02X", static_cast<unsigned>(this->dump_block_));
        what = param_what;
        break;
      default:
        what = "sample";
        break;
    }
  }

  ESP_LOGD(TAG, "%s data (%u bytes): %s", what, static_cast<unsigned>(this->rx_len_),
           hex_str_(this->rx_buf_, this->rx_len_).c_str());

  // A raw probe has no notion of failure. The frame being sent is usually a
  // guess, so a NAK, a truncated reply or silence are the answer rather than an
  // error - report what came back and let the transaction end cleanly.
  if (req == DIETRICH_REQ_RAW) {
    this->log_raw_reply_();
    return true;
  }

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
    if (this->txn_active_) {
      // A transaction's reads are working material, not published state: they go
      // to txn_read_ and reach params_ only once a write has verified. A refused
      // or failed transaction then leaves the parameter sensors exactly as the
      // last good sweep left them.
      for (size_t i = 0; i < DIETRICH_PARAM_BLOCK_SIZE; i++)
        this->txn_read_[blk * DIETRICH_PARAM_BLOCK_SIZE + i] = this->d_(i);
      // Only the read phase is checked. After the write, a block that disagrees
      // is the verify's business to report, not a reason to abort something that
      // has already gone out.
      if (!this->txn_wrote_ && !this->block_is_sane_(blk))
        return false;
      this->txn_blocks_read_ |= static_cast<uint8_t>(1u << blk);
      return true;
    }

    for (size_t i = 0; i < DIETRICH_PARAM_BLOCK_SIZE; i++)
      this->params_[blk * DIETRICH_PARAM_BLOCK_SIZE + i] = this->d_(i);
    this->param_blocks_seen_ |= static_cast<uint8_t>(1u << blk);
    // publish only once the whole sweep is in, so the values are consistent
    if (this->param_blocks_seen_ == 0xFF)
      this->decode_params_();
    return true;
  }

  if (is_write_req_(req)) {
    // An ACK is an ordinary response frame carrying no data, and
    // response_error_() has already checked the type byte, the swapped
    // addresses and the echoed COMMAND/EXT_COMMAND - so there is nothing left
    // to decode, and getting this far is the acknowledgement. It is not,
    // however, evidence that the boiler applied anything; that is what the
    // verify reads are for.
    ESP_LOGD(TAG, "block 0x%02X write ACKed", static_cast<unsigned>(block_of_(req)));
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
    case DIETRICH_REQ_IDENT_PCU:
      this->decode_identification_(0x01);
      break;
    case DIETRICH_REQ_IDENT_PSU:
      this->decode_identification_(0x00);
      break;
    case DIETRICH_REQ_DUMP:
      this->log_eeprom_block_();
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

  if (is_write_req_(req)) {
    const uint8_t blk = block_of_(req);
    // everything is decided once, at the first block of the write phase
    if (blk == this->txn_first_block_ && !this->begin_write_phase_())
      return;
    this->build_write_frame_(blk);
  } else if (req == DIETRICH_REQ_DUMP) {
    this->build_read_frame_(this->dump_addr_, this->dump_block_);
  }

  // drop anything left over from a previous exchange
  while (this->available())
    this->read();

  const uint8_t *cmd = nullptr;
  size_t len = 0;
  this->command_for_(req, &cmd, &len);
  if (is_write_req_(req))
    ESP_LOGI(TAG, "writing block 0x%02X: %s", static_cast<unsigned>(block_of_(req)), hex_str_(cmd, len).c_str());
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
  if (this->txn_active_ && !ok) {
    if (this->queue_pos_ >= this->txn_relock_pos_) {
      if (this->txn_relock_tries_ < RELOCK_RETRIES) {
        this->txn_relock_tries_++;
        ESP_LOGW(TAG, "re-lock got no usable reply, sending it again");
        this->next_send_time_ = millis() + INTER_REQUEST_MS;
        this->state_machine_ = DIETRICH_SEND;
        return;
      }
      this->txn_relock_failed_ = true;
    } else {
      this->txn_failed_ = true;
    }
  }
  this->advance_();
}

// Jump to the re-lock, which start_txn_() always puts last in the queue. Used
// both for a step that failed and for a write that turned out to be unnecessary.
void Dietrich::skip_to_relock_() {
  this->txn_relock_tries_ = 0;
  if (this->queue_len_ > 0)
    this->queue_pos_ = this->txn_relock_pos_ < this->queue_len_ ? this->txn_relock_pos_
                                                                : static_cast<uint8_t>(this->queue_len_ - 1);
  this->next_send_time_ = millis() + INTER_REQUEST_MS;
  this->state_machine_ = DIETRICH_SEND;
}

void Dietrich::advance_() {
  // A dump is one request re-armed, not a queue - 128 blocks would not fit in one.
  // A block that failed is stepped over rather than abandoning the sweep: which
  // blocks a device declines is itself part of what a sweep is for.
  if (this->dump_active_ && this->queue_[this->queue_pos_] == DIETRICH_REQ_DUMP) {
    this->dump_block_++;
    if (this->dump_block_ < this->dump_end_) {
      this->next_send_time_ = millis() + INTER_REQUEST_MS;
      this->state_machine_ = DIETRICH_SEND;
      return;
    }
    this->dump_active_ = false;
    ESP_LOGI(TAG, "eeprom dump of 0x%02X finished", static_cast<unsigned>(this->dump_addr_));
  }

  // The pre-flight sample said the boiler is busy. It is the first thing in the
  // queue, so nothing has been unlocked and there is nothing to put back: stop
  // here rather than walking through a re-lock of something never unlocked.
  if (this->txn_active_ && this->txn_refused_busy_) {
    this->finish_txn_();
    this->state_machine_ = DIETRICH_IDLE;
    return;
  }

  // A failed step in a write transaction goes straight to the re-lock instead of
  // carrying on. Recom leaves service mode on when a write fails - it re-locks
  // only in the success branch, with no try/finally - and that is not a bug
  // worth copying. See mapping/pcu05_p3_protocol.md.
  if (this->txn_active_ && this->txn_failed_ && this->queue_pos_ + 1 < this->queue_len_ &&
      this->queue_pos_ < this->txn_relock_pos_) {
    this->skip_to_relock_();
    return;
  }

  // An EEPROM program cycle is slower than a read, and eight of them back to back
  // at read speed is this component's idea, not Recom's.
  const bool just_wrote = is_write_req_(this->queue_[this->queue_pos_]);

  this->txn_relock_tries_ = 0;  // the resend budget is per step, not per transaction
  this->queue_pos_++;
  if (this->queue_pos_ >= this->queue_len_) {
    if (this->txn_active_)
      this->finish_txn_();
    this->state_machine_ = DIETRICH_IDLE;
    return;
  }
  this->next_send_time_ = millis() + (just_wrote ? WRITE_SETTLE_MS : INTER_REQUEST_MS);
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
    else if (this->dump_active_)
      ESP_LOGD(TAG, "eeprom dump in progress, skipping this poll interval");
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

  // Asked first, and once, the way Recom opens a connection with it. It borrows
  // one poll interval; the counter and parameter timers above keep counting, so
  // nothing else is skipped, only delayed by one.
  if (this->dump_active_) {
    this->queue_[0] = DIETRICH_REQ_DUMP;
    this->queue_len_ = 1;
  } else if (this->pending_ident_) {
    this->pending_ident_ = false;
    this->queue_[0] = DIETRICH_REQ_IDENT_PCU;
    this->queue_[1] = DIETRICH_REQ_IDENT_PSU;
    this->queue_len_ = 2;
    // A parameter sweep is 8 requests, so give it a whole poll interval of its own
    // rather than appending it to the sample or counter cycle.
  } else if (this->variant_ == DIETRICH_VARIANT_PCU05_P3 && this->want_params_() &&
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
