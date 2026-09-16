#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace dietrich {

// lookup table entry for the status/locking/blocking code registers
struct CodeText;

enum DietrichVariant : uint8_t {
  DIETRICH_VARIANT_MCR3 = 0,
  DIETRICH_VARIANT_CALENTA_V1_P5,
  DIETRICH_VARIANT_PCU05_P3,
};

// One request/response exchange with the boiler. The PARAM entries read the
// 128 byte parameter block out of EEPROM blocks 0x14..0x1B, 16 bytes at a time;
// see mapping/pcu05_p3_protocol.md. They must stay last and contiguous - the
// block index is recovered as (req - DIETRICH_REQ_PARAM0), and every other kind
// is identified by sorting below DIETRICH_REQ_PARAM0. New request kinds
// therefore go above it, never after it.
enum DietrichRequest : uint8_t {
  DIETRICH_REQ_SAMPLE = 0,
  DIETRICH_REQ_COUNTER1,
  DIETRICH_REQ_COUNTER2,
  // the write path; see start_txn_() for the sequence these make up
  DIETRICH_REQ_SERVICE_ON,
  DIETRICH_REQ_SERVICE_OFF,
  // the same unlock and re-lock addressed to 0x00, where the parameter EEPROM is
  DIETRICH_REQ_SERVICE_ON_EE,
  DIETRICH_REQ_SERVICE_OFF_EE,
  // EEPROM block writes, one per parameter block. Contiguous like the PARAM
  // entries and immediately below them, so the block is (req - WRITE0) and the
  // range test is WRITE0 <= req < PARAM0.
  DIETRICH_REQ_WRITE0,
  DIETRICH_REQ_WRITE1,
  DIETRICH_REQ_WRITE2,
  DIETRICH_REQ_WRITE3,
  DIETRICH_REQ_WRITE4,
  DIETRICH_REQ_WRITE5,
  DIETRICH_REQ_WRITE6,
  DIETRICH_REQ_WRITE7,
  DIETRICH_REQ_PARAM0,
  DIETRICH_REQ_PARAM1,
  DIETRICH_REQ_PARAM2,
  DIETRICH_REQ_PARAM3,
  DIETRICH_REQ_PARAM4,
  DIETRICH_REQ_PARAM5,
  DIETRICH_REQ_PARAM6,
  DIETRICH_REQ_PARAM7,
};

// 8 blocks of 16 bytes
static const size_t DIETRICH_PARAM_BLOCKS = 8;
static const size_t DIETRICH_PARAM_BLOCK_SIZE = 16;
static const size_t DIETRICH_PARAM_BYTES = DIETRICH_PARAM_BLOCKS * DIETRICH_PARAM_BLOCK_SIZE;
// EEPROM block indices the parameter block occupies, used as the EXT_COMMAND byte
static const uint8_t DIETRICH_PARAM_FIRST_BLOCK = 0x14;
static const uint8_t DIETRICH_PARAM_LAST_BLOCK = 0x1B;
// STX + 6 header bytes + 16 data bytes + CRC16 + ETX
static const size_t DIETRICH_WRITE_FRAME_LEN = 26;
// A full parameter write is 2 unlocks + 8 reads + 8 writes + 8 verify reads +
// 2 re-locks
static const size_t DIETRICH_QUEUE_LEN = 32;

enum DietrichState : uint8_t {
  DIETRICH_IDLE = 0,
  DIETRICH_SEND,
  DIETRICH_WAIT,
};

// What the write path has been asked to do. A request is staged by one of the
// public write methods and picked up by loop() as soon as the bus is idle, so
// nothing is ever injected into an exchange that is already in flight.
enum DietrichTxn : uint8_t {
  DIETRICH_TXN_NONE = 0,
  DIETRICH_TXN_SERVICE_TEST,  // unlock then re-lock, touching nothing
  DIETRICH_TXN_IDENTITY,      // read a block and write it straight back unchanged
  DIETRICH_TXN_PARAM,         // read-modify-write one parameter byte
  DIETRICH_TXN_RELOCK,        // re-lock only; used when the boiler boots unlocked
};

class Dietrich : public PollingComponent, public uart::UARTDevice {
 public:
  void set_variant(DietrichVariant variant) { this->variant_ = variant; }
  void set_allow_writes(bool allow) { this->allow_writes_ = allow; }

  // frame status/state
  SUB_SENSOR(state)
  SUB_SENSOR(sub_state)
  SUB_SENSOR(lockout)
  SUB_SENSOR(blocking)

  // decoded text for the four code registers above
  SUB_TEXT_SENSOR(state)
  SUB_TEXT_SENSOR(sub_state)
  SUB_TEXT_SENSOR(lockout)
  SUB_TEXT_SENSOR(blocking)

  // sample data - temperatures
  SUB_SENSOR(flow_temp)
  SUB_SENSOR(return_temp)
  SUB_SENSOR(dhw_in_temp)
  SUB_SENSOR(outside_temp)
  SUB_SENSOR(calorifier_temp)
  SUB_SENSOR(boiler_control_temp)
  SUB_SENSOR(room_temp)
  SUB_SENSOR(ch_setpoint)   // co
  SUB_SENSOR(dhw_setpoint)  // cwu
  SUB_SENSOR(room_temp_setpoint)

  // airflow setpoint / airflow (data offsets 22 and 24); kept under the historic
  // fan_speed* names so existing configurations keep working
  SUB_SENSOR(fan_speed_setpoint)
  SUB_SENSOR(fan_speed)

  // power / misc
  SUB_SENSOR(ionisation_current)
  SUB_SENSOR(internal_setpoint)
  SUB_SENSOR(available_power)
  SUB_SENSOR(pump_percentage)
  SUB_SENSOR(desired_max_power)
  SUB_SENSOR(actual_power)

  SUB_BINARY_SENSOR(demand_source_bit0)  // BIT0=Mod.Controller Connected
  SUB_BINARY_SENSOR(demand_source_bit1)  // BIT1=Heat demand from Mod.Controller
  SUB_BINARY_SENSOR(demand_source_bit2)  // BIT2=Heat demand from on/off controller
  SUB_BINARY_SENSOR(demand_source_bit3)  // BIT3=Frost Protection
  SUB_BINARY_SENSOR(demand_source_bit4)  // BIT4=DHW Eco (inverted)
  SUB_BINARY_SENSOR(demand_source_bit5)  // BIT5=DHW Blocking
  SUB_BINARY_SENSOR(demand_source_bit6)  // BIT6=Anti Legionella
  SUB_BINARY_SENSOR(demand_source_bit7)  // BIT7=DHW Heat Demand

  SUB_BINARY_SENSOR(input_bit0)  // BIT0=Shutdown Input (inverted on pcu05_p3)
  SUB_BINARY_SENSOR(input_bit1)  // BIT1=Release Input (inverted on pcu05_p3)
  SUB_BINARY_SENSOR(input_bit2)  // BIT2=Ionisation
  SUB_BINARY_SENSOR(input_bit3)  // BIT3=Flow Switch detecting DHW
  SUB_BINARY_SENSOR(input_bit5)  // BIT5=Min Gas Pressure
  SUB_BINARY_SENSOR(input_bit6)  // BIT6=CH Enable
  SUB_BINARY_SENSOR(input_bit7)  // BIT7=DHW Enable

  SUB_BINARY_SENSOR(valve_bit0)  // BIT0=Gas Valve (inverted)
  SUB_BINARY_SENSOR(valve_bit2)  // BIT2=Ignition
  SUB_BINARY_SENSOR(valve_bit3)  // BIT3=3-Way valve position
  SUB_BINARY_SENSOR(valve_bit4)  // BIT4=Ext.3-Way Valve
  SUB_BINARY_SENSOR(valve_bit6)  // BIT6=Ext. Gas Valve

  SUB_BINARY_SENSOR(pump_bit0)  // BIT0=Pump
  SUB_BINARY_SENSOR(pump_bit1)  // BIT1=Calorifier Pump
  SUB_BINARY_SENSOR(pump_bit2)  // BIT2=Ext.CH Pump
  SUB_BINARY_SENSOR(pump_bit4)  // BIT4=Status Report
  SUB_BINARY_SENSOR(pump_bit7)  // BIT7=Opentherm SmartPower

  SUB_SENSOR(hydro_pressure)
  SUB_BINARY_SENSOR(hru)
  SUB_SENSOR(control_temp)
  SUB_SENSOR(dhw_flowrate)

  // pcu05_p3 only - fields the Avanta/Calenta maps do not define
  SUB_SENSOR(fan_speed_rpm)      // data 44, real fan speed in rpm
  SUB_SENSOR(su_state)           // data 46
  SUB_SENSOR(su_locking)         // data 47
  SUB_SENSOR(su_blocking)        // data 48
  SUB_BINARY_SENSOR(ch_timer_enable)    // data 50 BIT6
  SUB_BINARY_SENSOR(dhw_timer_enable)   // data 50 BIT7
  SUB_SENSOR(solar_temp)         // data 56
  SUB_SENSOR(hmi_active)         // data 58
  SUB_SENSOR(ch_setpoint_hmi)    // data 60
  SUB_SENSOR(dhw_setpoint_hmi)   // data 61
  SUB_SENSOR(service_mode)       // data 62
  SUB_SENSOR(rs232_mode)         // data 63

  // counter data 1
  SUB_SENSOR(hours_run_pump)
  SUB_SENSOR(hours_run_3way)
  SUB_SENSOR(hours_run_ch)
  SUB_SENSOR(hours_run_dhw)
  SUB_SENSOR(power_supply_aval_hours)
  SUB_SENSOR(pump_starts)
  SUB_SENSOR(number_of_3way_valve_cycles)
  SUB_SENSOR(burner_start_dhw)

  // counter data 2
  SUB_SENSOR(total_burner_start)
  SUB_SENSOR(failed_burner_start)
  SUB_SENSOR(number_flame_loss)

  // Stored parameters, read from EEPROM rather than the sample block. These are
  // the boiler's configuration, not live measurements: they only change when
  // somebody edits them with a service tool, so they are polled once an hour.
  // Offsets are into the 128 byte parameter block (see the map in
  // mapping/pcu05_p3_protocol.md), NOT into the sample block.
  SUB_SENSOR(param_ch_max_flow)          // p1,  byte 0
  SUB_SENSOR(param_dhw_setpoint)         // p2,  byte 1
  SUB_SENSOR(param_pump_post_run)        // p5,  byte 4
  SUB_SENSOR(param_max_flow_system)      // p23, byte 22
  SUB_SENSOR(param_curve_foot_outside)   // p25, byte 24
  SUB_SENSOR(param_curve_foot_flow)      // p26, byte 25
  SUB_SENSOR(param_curve_cold_outside)   // p27, byte 26, signed
  SUB_SENSOR(param_pump_ch_min)          // p28, byte 27, x10 %
  SUB_SENSOR(param_pump_ch_max)          // p29, byte 28, x10 %
  SUB_SENSOR(param_dhw_hysteresis)       // p33, byte 32

  // --- writing ------------------------------------------------------------
  // These are the whole write API, and they are meant to be called from a YAML
  // lambda. Each returns true when the request was *accepted*, not when it
  // completed: the exchange runs asynchronously in loop() and reports its result
  // to the log. All of them refuse unless allow_writes is set on the component
  // and the variant is pcu05_p3, and only one can be in flight at a time.

  // Unlock service mode and immediately re-lock it, writing nothing at all.
  // Sample byte 62 reports the state, so this tests the unlock path at no risk.
  bool test_service_mode();

  // Read one EEPROM block and write it back byte-for-byte unchanged, which
  // exercises the write frame and the ACK without changing a setting. block is
  // an EEPROM block index in 0x14..0x1B.
  bool write_block_unchanged(uint8_t block);

  // Read-modify-write a single parameter. param is the pNN number from the map
  // in mapping/pcu05_p3_protocol.md; value is clamped to that parameter's
  // documented range, and the write is skipped when the boiler already holds it.
  bool write_param(uint8_t param, uint8_t value);

  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void send_request_();
  void poll_response_();
  void advance_();
  // false when the frame was missing, rejected or unusable
  bool handle_response_();
  void decode_sample_();
  void decode_counter1_();
  void decode_counter2_();
  void decode_params_();
  // true when at least one param_* sensor is configured; nothing is requested
  // from the boiler otherwise
  bool want_params_() const;

  // write path
  bool stage_txn_(DietrichTxn txn, uint8_t first_block, uint8_t block_count, uint8_t byte_offset, uint8_t value,
                  const char *what);
  void start_txn_();
  void finish_txn_();
  // jump to the re-lock, which start_txn_() always leaves last in the queue
  void skip_to_relock_();
  // Runs once, at the first write of a transaction: checks that every block was
  // read back cleanly, sanity-checks the image against the documented ranges and
  // applies the staged edit. False means the transaction has already been failed
  // or skipped and nothing should be written.
  bool begin_write_phase_();
  // true when every documented parameter inside the blocks about to be written
  // reads inside its range - i.e. the image is plausibly a real one
  bool image_is_sane_() const;
  bool block_is_sane_(size_t blk) const;
  // fills tx_buf_ from txn_image_; only valid after begin_write_phase_()
  void build_write_frame_(uint8_t block);
  static bool is_write_req_(DietrichRequest req);
  // EEPROM block index for a PARAM or WRITE request
  static uint8_t block_of_(DietrichRequest req);

  void command_for_(DietrichRequest req, const uint8_t **cmd, size_t *len) const;
  // number of header bytes before the data block in a response frame
  size_t header_len_() const;
  // number of checksum/ETX bytes after the data block
  size_t trailer_len_() const;
  // true when the received frame carries at least count bytes from data offset off
  bool have_(size_t off, size_t count) const;
  // raw data byte at a data-block offset (0 when out of range)
  uint8_t d_(size_t off) const;
  uint16_t le16_(size_t off) const;   // little-endian pair, sample block
  uint16_t be16_(size_t off) const;   // big-endian pair, counter block

  void publish_(sensor::Sensor *s, float value);
  void publish_text_(text_sensor::TextSensor *s, const char *text);

  // each of these is a no-op when the sensor is unset or the frame is too short
  void pub_temp_(sensor::Sensor *s, size_t off);                 // signed 16-bit x 0.01 degC
  void pub_s16_(sensor::Sensor *s, size_t off, float scale);     // signed 16-bit x scale
  void pub_u16_(sensor::Sensor *s, size_t off);                  // unsigned 16-bit
  void pub_u8_(sensor::Sensor *s, size_t off, float scale);      // byte x scale
  void pub_s8_(sensor::Sensor *s, size_t off);                   // signed byte
  void pub_bit_(binary_sensor::BinarySensor *s, size_t off, uint8_t bit, bool invert);
  void pub_code_(sensor::Sensor *num, text_sensor::TextSensor *txt, size_t off, const CodeText *table, size_t len);
  void pub_counter_(sensor::Sensor *s, size_t off, float scale);  // big-endian 16-bit x scale
  // parameter-block publishers; these read params_, not the received frame
  void pub_param_(sensor::Sensor *s, size_t off, float scale);   // byte x scale
  void pub_param_s8_(sensor::Sensor *s, size_t off);             // signed byte

  // nullptr when the received frame is a valid response to the request currently
  // in flight, otherwise a short reason for the log
  const char *response_error_() const;
  static bool is_valid_crc_(const uint8_t *response, size_t n);
  static uint16_t crc16_(const uint8_t *data, size_t from, size_t to);
  // a write gets longer than a read; see WRITE_TIMEOUT_MS
  static uint32_t timeout_for_(DietrichRequest req);
  static float signed_float_(float value);
  static float temp_or_nan_(uint16_t raw);
  static std::string hex_str_(const uint8_t *data, size_t len);

  DietrichVariant variant_{DIETRICH_VARIANT_MCR3};

  DietrichState state_machine_{DIETRICH_IDLE};
  // long enough for a full parameter write; see DIETRICH_QUEUE_LEN
  DietrichRequest queue_[DIETRICH_QUEUE_LEN]{};
  uint8_t queue_len_{0};
  uint8_t queue_pos_{0};

  uint8_t params_[DIETRICH_PARAM_BYTES]{};
  // bit n set once block n has been received in the current sweep
  uint8_t param_blocks_seen_{0};
  int param_timer_{99999};

  uint8_t rx_buf_[96]{};
  size_t rx_len_{0};
  size_t data_len_{0};
  uint32_t request_time_{0};
  uint32_t last_byte_time_{0};
  uint32_t next_send_time_{0};
  int counter_timer_{99};

  // --- write path ---------------------------------------------------------
  bool allow_writes_{false};

  // staged by the public write methods, consumed by start_txn_()
  DietrichTxn pending_txn_{DIETRICH_TXN_NONE};
  uint8_t pending_first_block_{DIETRICH_PARAM_FIRST_BLOCK};
  uint8_t pending_block_count_{1};
  uint8_t pending_byte_{0};  // offset into the 128 byte parameter block
  uint8_t pending_value_{0};

  // live for as long as a transaction is on the bus
  bool txn_active_{false};
  bool txn_failed_{false};
  bool txn_wrote_{false};  // at least one write was ACKed, so a verify is meaningful
  DietrichTxn txn_kind_{DIETRICH_TXN_NONE};
  uint8_t txn_first_block_{DIETRICH_PARAM_FIRST_BLOCK};
  uint8_t txn_block_count_{1};
  // bit n set once block n has been read back whole inside this transaction
  uint8_t txn_blocks_read_{0};
  // where the trailing re-lock steps start, so a failure part-way through jumps
  // to the first of them rather than to the last
  uint8_t txn_relock_pos_{0};
  // Every block a transaction reads lands here, never in params_. A transaction
  // that ends up refusing to write must not have moved the published parameter
  // sensors on its way there, and a read that came back wrong must not become the
  // source for the next read-modify-write. params_ is updated from here only
  // after a write has verified.
  uint8_t txn_read_[DIETRICH_PARAM_BYTES]{};
  // the image this transaction intends the boiler to end up holding: the blocks
  // it read for itself, plus the one staged edit. The verify reads are compared
  // against this.
  uint8_t txn_image_[DIETRICH_PARAM_BYTES]{};
  uint8_t tx_buf_[DIETRICH_WRITE_FRAME_LEN]{};

  // the boot service-mode check runs on the first long-enough sample only
  bool seen_sample_{false};
};

}  // namespace dietrich
}  // namespace esphome
