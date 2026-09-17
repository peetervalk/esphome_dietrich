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
// one writable parameter and its documented range; the table lives in dietrich.cpp
struct ParamLimit;

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
  // Read-only, and nothing to do with the write path - they are here because
  // every request kind has to sort below DIETRICH_REQ_WRITE0. Both addresses are
  // asked: they answer the same command with different payloads.
  DIETRICH_REQ_IDENT_PCU,
  DIETRICH_REQ_IDENT_PSU,
  // one block of an EEPROM sweep; the block index lives in dump_block_, not in
  // the request, so a 128 block dump does not need a 128 entry queue
  DIETRICH_REQ_DUMP,
  // COMMAND 0x31, the protocol's own restart. Never sent as part of a write -
  // only by reset_board(), on its own. See start_txn_().
  DIETRICH_REQ_RESET,
  // COMMAND 0x09 (CODE_FACTORY_COMMANDO) with EXT_COMMAND 0x52 (CODE_FACTORY =
  // 82), and the matching re-lock. This is the unlock level Recom reaches after
  // the 0012 PIN - the one where dF/dU become editable - and no version of this
  // component has ever sent it. Every parameter write this component has made
  // used CODE_SERVICE (0x08/0x0C) instead, which the board ACKs and stores and
  // then refuses to adopt. See mapping/pcu05_p3_protocol.md, *Service mode is
  // not the commissioning unlock*.
  DIETRICH_REQ_FACTORY_ON,
  DIETRICH_REQ_FACTORY_OFF,
  DIETRICH_REQ_FACTORY_ON_EE,
  DIETRICH_REQ_FACTORY_OFF_EE,
  // One arbitrary frame, built by send_raw() or send_command() and sent exactly
  // once. It is the only request kind whose reply is never rejected: a NAK, a
  // truncated frame or silence are all results worth having when probing a
  // command whose shape is a guess.
  DIETRICH_REQ_RAW,
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
// The image protects itself with a CRC16 per 64 byte half: bytes 62..63 cover
// bytes 0..61 and bytes 126..127 cover bytes 64..125, same poly and init as the
// frame CRC, stored LSB first. The PCU checks them, and a set whose CRC does not
// match is stored but never adopted - that is Blocking 0. Captured off Recom
// making two parameter writes, mapping/260916_2303.pcapng.
static const size_t DIETRICH_PARAM_HALF = 64;
static const size_t DIETRICH_PARAM_CRC_SPAN = 62;
// STX + 6 header bytes + 16 data bytes + CRC16 + ETX
static const size_t DIETRICH_WRITE_FRAME_LEN = 26;
static const size_t DIETRICH_READ_FRAME_LEN = 10;
// EEPROMSize in PCU-05_P3.xml, in 16 byte blocks: 0x00..0x7F
static const uint16_t DIETRICH_EEPROM_BLOCKS = 128;
// A full parameter write is 1 pre-flight sample + 2 unlocks + 8 reads + 8 writes
// + 8 verify reads + 2 re-locks + 1 post-write sample. Rounded up so an unlock
// pair can be added without the queue silently overflowing.
static const size_t DIETRICH_QUEUE_LEN = 40;
// Longest frame send_raw() will accept. A write frame is 26; this leaves room to
// replay something longer off a capture without letting a typo run away.
static const size_t DIETRICH_RAW_FRAME_MAX = 40;
// How many parameter edits can be staged before one write carries them all to the
// boiler. A parameter write already reads, rewrites and verifies the whole 128
// byte image whatever it is changing, so N edits cost exactly what one does: the
// same thirty-odd frames on the bus and the same single EEPROM cycle. That is the
// reason the queue exists - not the typing it saves, but that changing six
// parameters one at a time would be six EEPROM cycles and six chances to provoke
// the blocking a write can bring on. See begin_write_phase_().
static const size_t DIETRICH_EDIT_QUEUE_MAX = 8;

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
  DIETRICH_TXN_RESET,         // COMMAND 0x31 on its own, unlocking nothing
  DIETRICH_TXN_FACTORY_TEST,  // factory unlock, one sample, re-lock; writes nothing
  DIETRICH_TXN_RAW,           // one arbitrary frame, unlocking nothing
};

// One staged parameter edit, as queue_param() accepts it: the pNN number, kept
// only so the queue and the result can name it; the byte it occupies in the 128
// byte image; and the value. The range check happens when the edit is queued, so
// an out of range value never reaches this struct.
struct PendingEdit {
  uint8_t param;
  uint8_t offset;
  uint8_t value;
};

class Dietrich : public PollingComponent, public uart::UARTDevice {
 public:
  void set_variant(DietrichVariant variant) {
    this->variant_ = variant;
    // IDENTIFICATION is decoded to the PCU-05 P3 layout, so only that variant
    // asks for it; anything else must not send a request it cannot read back.
    this->pending_ident_ = variant == DIETRICH_VARIANT_PCU05_P3;
  }
  void set_allow_writes(bool allow) { this->allow_writes_ = allow; }
  // The UI gate, and only that: a switch in Home Assistant so a write cannot be
  // set off by a stray press or a misfiring automation. The real gate is
  // allow_writes, which is compiled in and cannot be changed from Home Assistant
  // at all. This defaults to true so a configuration that drives writes purely
  // from lambdas behaves exactly as it did before the switch existed; a YAML that
  // does declare the switch turns it off at boot and is then the authority.
  void set_write_enabled(bool on) { this->write_enabled_ = on; }
  // When set, a parameter write unlocks with CODE_FACTORY (0x09/0x52) instead of
  // CODE_SERVICE (0x08/0x0C) - a level, not an addition, so the two are never
  // sent together. Recom was captured writing parameters from two different
  // menu levels and sent CODE_SERVICE both times, so this buys nothing; it is
  // kept only to put COMMAND 0x09 to a board and see what it says.
  void set_use_factory_mode(bool use) { this->use_factory_mode_ = use; }

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

  // The write path's two reporting entities. write_queue lists the staged edits
  // ("p2=55, p33=6", or "empty"); write_result carries the verdict of whatever
  // the write path last did, refusals included. Everything they say is in the log
  // too - they exist so a write driven from Home Assistant reports back there
  // rather than only to a console nobody has open.
  SUB_TEXT_SENSOR(write_result)
  SUB_TEXT_SENSOR(write_queue)

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

  // --- identification -----------------------------------------------------
  // Ask both device addresses who they think they are: device type, software
  // and parameter version, operating hours, connected device types and the last
  // blocking and locking codes. A plain read - no service mode, nothing written
  // - so it is not gated behind allow_writes, only behind variant pcu05_p3. The
  // result goes to the log.
  //
  // Sent once on the first poll after boot, as Recom does when it connects. Call
  // this to ask again; the request goes out on the next poll interval.
  bool read_identification();

  // Read EEPROM blocks and log them, hex and ASCII, one line each. A plain read -
  // READ_EPROM_BLOCK needs no service mode - so nothing is unlocked and nothing is
  // written, and a block that fails is logged and stepped over rather than
  // abandoning the sweep. addr is the device (0x00 or 0x01; they answer with
  // different contents), first is the block index, count how many to walk.
  //
  // The whole EEPROM is 0x00..0x7F, of which only 0x14..0x1B (parameters) and
  // 0x1C..0x1F (counters) are mapped. The rest is where the appliance
  // identification and the fault history records have to be.
  bool dump_eeprom(uint8_t addr, uint8_t first, uint8_t count);

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

  // Stage one parameter edit. param is the pNN number from the map in
  // mapping/pcu05_p3_protocol.md; value must be inside that parameter's
  // documented range - out of range is refused, not clamped, so a mistyped 30 for
  // p33 writes nothing rather than quietly writing 15. Queueing a parameter that
  // is already staged replaces its value rather than adding a second edit for the
  // same byte. Nothing reaches the boiler until write_queue() is called, so this
  // touches RAM only and is not gated behind allow_writes.
  bool queue_param(uint8_t param, uint8_t value);

  // Throw the staged edits away, writing nothing.
  void clear_param_queue();

  // Write every staged edit in one transaction, then empty the queue if - and
  // only if - the write verified. A run that was refused or failed leaves the
  // queue intact, so retrying it costs no retyping. An edit whose value the
  // boiler already holds is dropped on the way past; a queue where every edit is
  // already satisfied sends no write frame at all.
  bool write_queue();

  // Stage one edit and write it immediately: clear_param_queue(), queue_param()
  // and write_queue() in one call, for a lambda that only ever changes one thing.
  bool write_param(uint8_t param, uint8_t value);

  // COMMAND 0x31, RESET, addressed to the PCU and sent on its own: no service
  // mode, no EEPROM, nothing read or written. This is the protocol's version of
  // the mains power cycle that is the only thing known to clear the blocking a
  // parameter write provokes on a PCU-05 P3 - see mapping/pcu05_p3_protocol.md,
  // *What cleared it*. It is deliberately NOT part of write_param(): the command
  // has never been answered by this board, so it stays something you press
  // yourself, with the log open, on a boiler that is already blocked.
  bool reset_board();

  // Factory level, COMMAND 0x09 with EXT_COMMAND 0x52: unlock both addresses,
  // take one sample, re-lock. The exact parallel of test_service_mode(), and the
  // safe way to find out whether the board answers the command at all - nothing
  // is read from EEPROM and nothing is written. Watch sample byte 62, which this
  // map calls service_mode and which has never moved while byte 63 tracked the
  // service-level unlock; if byte 62 is the factory flag, this is what moves it.
  bool test_factory_mode();

  // --- raw frames ---------------------------------------------------------
  // The escape hatch. Both send one frame and log whatever comes back without
  // judging it: a NAK, a short frame or silence are all reported rather than
  // rejected, because when the frame being tried is a guess the refusal is the
  // finding. Neither touches params_, neither unlocks anything, and neither is
  // part of any transaction.
  //
  // send_command() builds the frame: STX, sender 0xFE, recipient dst, request
  // type, length, cmd, ext, the data bytes, CRC16 and ETX. Use this for anything
  // being tried for the first time - the length byte and the CRC cannot be got
  // wrong. data_hex may be empty, and may contain spaces.
  //
  //   id(boiler).send_command(0x01, 0x09, 0x52, "");   // factory unlock -> PCU
  //
  // send_raw() sends the bytes exactly as given, STX and CRC and ETX included,
  // and is for replaying a frame captured off Recom verbatim. It checks nothing
  // beyond the length, so a bad CRC goes out as a bad CRC - which is sometimes
  // the point.
  //
  //   id(boiler).send_raw("02FE01050809522EA603");
  bool send_command(uint8_t dst, uint8_t cmd, uint8_t ext, const std::string &data_hex);
  // The same thing from one string: recipient, command, ext, then any payload
  // bytes. Deliberately not wired to anything in the YAML, and not to be - a text
  // box in Home Assistant reaching this is raw frames crossing the HA boundary,
  // which is the thing the parameter queue exists to avoid. It is a lambda-only
  // escape hatch for probing a command whose shape is a guess.
  //
  //   "01 09 52"          factory unlock -> PCU
  //   "01 37 00 0C 00"    SERVICE_CODE with a two byte payload
  bool send_command_hex(const std::string &spec);
  bool send_raw(const std::string &hex);

  void setup() override;
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
  void decode_identification_(uint8_t addr);
  void log_eeprom_block_() const;
  // a run of data bytes read as text, stopping at the first 0x00 or 0xFF pad
  std::string text_(size_t off, size_t len) const;
  // true when at least one param_* sensor is configured; nothing is requested
  // from the boiler otherwise
  bool want_params_() const;

  // write path
  // The edits are read from edit_queue_ rather than passed in: a parameter write
  // carries however many are staged, and every other kind of transaction none.
  bool stage_txn_(DietrichTxn txn, uint8_t first_block, uint8_t block_count, const char *what);
  // nullptr when this component will not write that parameter
  static const ParamLimit *find_limit_(uint8_t param);
  // Format a line, log it at the given level (0 info, 1 warning, 2 error) and
  // publish it to the write_result text sensor. Every verdict the write path
  // reaches goes through here, so the entity and the log cannot disagree.
  void set_result_(uint8_t level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
  void publish_write_queue_();
  void start_txn_();
  void finish_txn_();
  // jump to the re-lock, which start_txn_() leaves last but for the post-write
  // sample
  void skip_to_relock_();
  // True when the last sample shows a boiler that is not burning, not purging and
  // not finishing a charge. Reads the sample currently in data_, so it is only
  // meaningful straight after one has been decoded.
  //
  // This used to gate the write, and no longer does. Both p33 writes that put a
  // PCU-05 P3 into Blocking 0 on 2026-09-16 went out to a boiler in
  // 8:Controlled stop / 0:Standby - the gate passed on each occasion, correctly -
  // and the cause turned out to be the image CRC the write was not maintaining.
  // So it never prevented the fault it was added for, and the write goes out
  // whatever the boiler is doing. What is left is worth recording rather than
  // acting on: if a write ever does go wrong, the state it went out in is the
  // first thing anybody will want from the log. See
  // mapping/pcu05_p3_protocol.md, *What cleared it*.
  bool boiler_is_quiet_(const char **why) const;
  // Runs once, at the first write of a transaction: checks that every block was
  // read back cleanly, sanity-checks the image against the documented ranges and
  // applies the staged edit. False means the transaction has already been failed
  // or skipped and nothing should be written.
  bool begin_write_phase_();
  // true when every documented parameter inside the blocks about to be written
  // reads inside its range - i.e. the image is plausibly a real one
  bool image_is_sane_() const;
  bool block_is_sane_(size_t blk) const;
  // Recompute both half-image CRCs in place. Needs the whole 128 byte image, so
  // it is only ever called on a transaction that read all eight blocks.
  static void apply_param_crcs_(uint8_t *image);
  // true when an image as read off the boiler matches its own two CRCs
  static bool param_crcs_ok_(const uint8_t *image);
  // Report a raw reply without rejecting it: length, what the type byte says,
  // whether the addresses came back swapped and the COMMAND/EXT echoed, and
  // whether the CRC is good. Everything response_error_() would have refused on,
  // said out loud instead.
  void log_raw_reply_() const;
  // hex text (spaces allowed) into out; false when it is not valid hex or does
  // not fit
  static bool parse_hex_(const std::string &hex, uint8_t *out, size_t max, size_t *len);
  // build a well-formed request frame around cmd/ext/data and stage it as the
  // one frame of a DIETRICH_TXN_RAW transaction
  bool build_and_stage_command_(uint8_t dst, uint8_t cmd, uint8_t ext, const uint8_t *data, size_t data_len);
  // fills tx_buf_ with a READ_EPROM_BLOCK request for one block
  void build_read_frame_(uint8_t addr, uint8_t block);
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
  bool write_enabled_{true};
  bool use_factory_mode_{false};

  // the one frame a DIETRICH_TXN_RAW transaction sends
  uint8_t raw_buf_[DIETRICH_RAW_FRAME_MAX]{};
  size_t raw_len_{0};

  // staged by the public write methods, consumed by start_txn_()
  DietrichTxn pending_txn_{DIETRICH_TXN_NONE};
  uint8_t pending_first_block_{DIETRICH_PARAM_FIRST_BLOCK};
  uint8_t pending_block_count_{1};
  // The staged edits, filled by queue_param() and applied by begin_write_phase_()
  // once the image has been read. They deliberately outlive a failed transaction;
  // see write_queue().
  PendingEdit edit_queue_[DIETRICH_EDIT_QUEUE_MAX]{};
  uint8_t edit_len_{0};

  // live for as long as a transaction is on the bus
  bool txn_active_{false};
  bool txn_failed_{false};
  // Which step went wrong, recorded where it went wrong: by the time finish_txn_()
  // composes the verdict the step is long gone, and "no reply to write 0x16" is a
  // different problem from "no reply to service mode on". Empty when nothing has.
  char txn_fail_step_[64]{};
  // Kept apart from txn_failed_ on purpose. The re-lock steps are last in the
  // queue, so by the time one of them goes wrong the write and its read-back
  // have already happened and their verdict still stands. Folding the two
  // together reported a verified write as a failure and threw the verify away.
  bool txn_relock_failed_{false};
  // resends of the re-lock step currently in hand; reset on every step
  uint8_t txn_relock_tries_{0};
  bool txn_wrote_{false};  // at least one write was ACKed, so a verify is meaningful
  // The pre-flight sample, first in the queue of every write transaction, and the
  // post-write one, last. Between them they answer the question the old sequence
  // could not: did a blocking code appear because the boiler was written to. A
  // code read from the last ordinary poll can be 15 seconds stale, which is long
  // enough to pin a pre-existing blocking on this write or miss one it caused.
  // See start_txn_() and decode_sample_().
  bool txn_saw_preflight_{false};
  uint8_t txn_blocking_before_{0xFF};
  bool txn_have_blocking_after_{false};
  uint8_t txn_blocking_after_{0xFF};
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
  // Recom issues IDENTIFICATION when it connects; this asks once at boot and
  // whenever read_identification() sets it again. set_variant() decides whether
  // it starts set at all - the default variant is mcr3, which never asks.
  bool pending_ident_{false};

  // --- EEPROM dump: read-only, and outside the transaction machinery entirely --
  bool dump_active_{false};
  uint8_t dump_addr_{0x00};
  uint8_t dump_block_{0};
  uint16_t dump_end_{0};  // exclusive
};

}  // namespace dietrich
}  // namespace esphome
