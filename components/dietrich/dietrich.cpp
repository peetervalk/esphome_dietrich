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
// both the MCR3 and the PCU-05: 02 | DEST | SRC | LEN | FUNC | BLOCK | SUB |
// CRC16-lo | CRC16-hi | 03. Responses repeat that 7 byte header, so the data
// block starts at index 7.
static const uint8_t CMD_SAMPLE_MCR3[10] = {0x02, 0xFE, 0x01, 0x05, 0x08, 0x02, 0x01, 0x69, 0xAB, 0x03};
static const uint8_t CMD_COUNTER1_MCR3[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x1C, 0x98, 0xC2, 0x03};
static const uint8_t CMD_COUNTER2_MCR3[10] = {0x02, 0xFE, 0x00, 0x05, 0x08, 0x10, 0x1D, 0x59, 0x02, 0x03};

// Avanta protocol (protocol.nr 2), XOR checksum, 6 byte response header
static const uint8_t CMD_SAMPLE_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x02, 0x00, 0x53, 0x03};
static const uint8_t CMD_COUNTER1_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x10, 0x01, 0x40, 0x03};
static const uint8_t CMD_COUNTER2_CALENTA[8] = {0x02, 0x52, 0x05, 0x06, 0x10, 0x02, 0x43, 0x03};

// give up on a response after this long
static const uint32_t RESPONSE_TIMEOUT_MS = 600;
// a gap this long after the last byte ends the frame
static const uint32_t RX_QUIET_MS = 40;
// settle time between two requests of the same cycle
static const uint32_t INTER_REQUEST_MS = 60;

struct CodeText {
  uint16_t code;
  const char *text;
};

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

// CRC16 (poly 0xA001, init 0xFFFF) over bytes 1..n-4; the frame ends with CRC (LSB, MSB) and 0x03
bool Dietrich::is_valid_crc_(const uint8_t *response, size_t n) {
  if (n < 5)
    return false;

  uint16_t expected_crc = response[n - 3] | (response[n - 2] << 8);

  uint16_t calculated_crc = 0xFFFF;
  for (size_t i = 1; i < n - 3; i++) {
    calculated_crc ^= response[i];
    for (uint8_t j = 0; j < 8; j++) {
      if ((calculated_crc & 0x0001) != 0) {
        calculated_crc >>= 1;
        calculated_crc ^= 0xA001;
      } else {
        calculated_crc >>= 1;
      }
    }
  }

  return expected_crc == calculated_crc;
}

// Remeha frames carry a CRC16; Avanta (calenta_v1_p5) frames use an XOR
// checksum instead, so for that variant we validate the response header
bool Dietrich::frame_valid_() const {
  if (this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5)
    return this->rx_len_ >= 3 && this->rx_buf_[0] == 2 && this->rx_buf_[1] == 65 && this->rx_buf_[2] == 6;
  return is_valid_crc_(this->rx_buf_, this->rx_len_);
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
  const bool calenta = this->variant_ == DIETRICH_VARIANT_CALENTA_V1_P5;
  switch (req) {
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

void Dietrich::handle_response_() {
  const DietrichRequest req = this->queue_[this->queue_pos_];
  const char *what = req == DIETRICH_REQ_SAMPLE ? "sample" : (req == DIETRICH_REQ_COUNTER1 ? "counter1" : "counter2");

  ESP_LOGD(TAG, "%s data (%u bytes): %s", what, static_cast<unsigned>(this->rx_len_),
           hex_str_(this->rx_buf_, this->rx_len_).c_str());

  if (this->rx_len_ == 0) {
    ESP_LOGW(TAG, "no response to %s request", what);
    return;
  }

  if (!this->frame_valid_()) {
    ESP_LOGW(TAG, "%s response failed validation", what);
    return;
  }

  const size_t overhead = this->header_len_() + this->trailer_len_();
  this->data_len_ = this->rx_len_ > overhead ? this->rx_len_ - overhead : 0;

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
  }
}

void Dietrich::send_request_() {
  // signed difference so the comparison survives the millis() wrap at ~49 days
  if (static_cast<int32_t>(millis() - this->next_send_time_) < 0)
    return;

  // drop anything left over from a previous exchange
  while (this->available())
    this->read();

  const uint8_t *cmd = nullptr;
  size_t len = 0;
  this->command_for_(this->queue_[this->queue_pos_], &cmd, &len);
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
  } else if (now - this->request_time_ >= RESPONSE_TIMEOUT_MS) {
    done = true;  // nothing came back at all
  }

  if (!done)
    return;

  this->handle_response_();
  this->advance_();
}

void Dietrich::advance_() {
  this->queue_pos_++;
  if (this->queue_pos_ >= this->queue_len_) {
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
      break;
  }
}

void Dietrich::update() {
  if (this->state_machine_ != DIETRICH_IDLE) {
    ESP_LOGW(TAG, "previous poll still running, skipping this interval");
    return;
  }

  this->counter_timer_++;
  this->queue_pos_ = 0;

  if (this->counter_timer_ >= 8) {
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
  if (this->variant_ == DIETRICH_VARIANT_PCU05_P3 && this->hydro_pressure_sensor_ != nullptr) {
    ESP_LOGW(TAG, "  hydro_pressure is not part of the PCU-05 P3 map - verify it against the boiler display");
  }
  LOG_UPDATE_INTERVAL(this);
  this->check_uart_settings(9600);
}

}  // namespace dietrich
}  // namespace esphome
