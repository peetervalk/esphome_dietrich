// Drives the dietrich component's write path against a simulated PCU-05 P3.
// Built with the stub ESPHome headers; no hardware and no ESPHome involved.
#include "dietrich.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// the bits of ESPHome the component leans on
// ---------------------------------------------------------------------------
static uint32_t g_millis = 1000;
static std::vector<std::string> g_log;
static bool g_verbose = false;

namespace esphome {
uint32_t millis() { return g_millis; }

void esph_log(const char *tag, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  (void) tag;
  g_log.emplace_back(buf);
  if (g_verbose)
    printf("    log: %s\n", buf);
}

namespace sensor {
void Sensor::publish_state(float state) { (void) state; }
}  // namespace sensor
namespace binary_sensor {
void BinarySensor::publish_state(bool state) { (void) state; }
}  // namespace binary_sensor
namespace text_sensor {
void TextSensor::publish_state(std::string state) { (void) state; }
}  // namespace text_sensor
}  // namespace esphome

// ---------------------------------------------------------------------------
// the simulated boiler
// ---------------------------------------------------------------------------

// The 128 byte parameter block as read off a live PCU-05 P3 (blocks 0x14..0x1B).
// Synthetic filler will not do: the component sanity-checks a freshly read image
// against the documented parameter ranges before writing any of it back, and
// filler is - correctly - refused. p1 = 61, p2 = 56, p33 = 4.
static const uint8_t REAL_PARAM_IMAGE[128] = {
    0x3D, 0x38, 0x01, 0x00, 0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x2F, 0x2F, 0x0B, 0x50, 0x17, 0xFF, 0x5A, 0x23, 0x1E, 0x19, 0xFA, 0x03, 0x0A, 0xF6, 0x00, 0x18,
    0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0xAF, 0x1E, 0x02, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x32, 0x3C, 0x50, 0x00, 0xC8, 0x05, 0x00, 0x12, 0x8F, 0x05,
    0xFF, 0x28, 0x46, 0x5A, 0x32, 0x19, 0x05, 0x1E, 0x0A, 0x1E, 0x03, 0x09, 0x41, 0x41, 0x14, 0x64,
    0x64, 0x64, 0x07, 0x46, 0x05, 0x05, 0x1E, 0x02, 0x05, 0x00, 0x28, 0x00, 0x00, 0x0F, 0xBC, 0x09,
    0x00, 0x78, 0x00, 0x0A, 0x05, 0x1E, 0x65, 0x4B, 0x00, 0xFE, 0x0A, 0x00, 0x0A, 0x64, 0xFF, 0x28,
    0x1A, 0x02, 0x05, 0x05, 0x02, 0x02, 0x05, 0x14, 0x14, 0x19, 0x05, 0xFF, 0xFF, 0xFF, 0x85, 0x9A,
};

struct FakeBoiler {
  uint8_t eeprom[12][16]{};  // blocks 0x14..0x1F
  bool service_mode{false};
  bool answer_reads{true};
  bool answer_writes{true};
  bool answer_service{true};
  // enforce what Recom's sequence implies: no EEPROM write while locked
  bool require_service_for_write{true};
  // CODE_SERVICE_START is ACKed but does not actually unlock
  bool service_mode_engages{true};
  // WRITE_EPROM_BLOCK is ACKed but the bytes are not stored - what a PCU-05 P3
  // was observed doing for a single-block write on 2026-09-16
  bool writes_take_effect{true};

  int reads{0}, writes{0}, service_on{0}, service_off{0}, rejected_writes{0};
  std::vector<uint8_t> read_dests;  // the address each READ_EPROM_BLOCK was sent to
  std::vector<std::vector<uint8_t>> written_frames;
  std::deque<uint8_t> tx;  // bytes heading for the ESP

  void reset() {
    for (int b = 0; b < 8; b++)
      for (int i = 0; i < 16; i++)
        eeprom[b][i] = REAL_PARAM_IMAGE[b * 16 + i];
    for (int b = 8; b < 12; b++)
      for (int i = 0; i < 16; i++)
        eeprom[b][i] = static_cast<uint8_t>(0xA0 + b);  // counter blocks, filler
    service_mode = false;
    answer_reads = answer_writes = answer_service = true;
    require_service_for_write = true;
    service_mode_engages = true;
    writes_take_effect = true;
    reads = writes = service_on = service_off = rejected_writes = 0;
    read_dests.clear();
    written_frames.clear();
    tx.clear();
  }

  static uint16_t crc16(const uint8_t *d, size_t from, size_t to) {
    uint16_t c = 0xFFFF;
    for (size_t i = from; i < to; i++) {
      c ^= d[i];
      for (int j = 0; j < 8; j++)
        c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0xA001) : static_cast<uint16_t>(c >> 1);
    }
    return c;
  }

  void respond(uint8_t to_addr, uint8_t from_addr, uint8_t cmd, uint8_t ext, const uint8_t *data, size_t n) {
    std::vector<uint8_t> f;
    f.push_back(0x02);
    f.push_back(from_addr);  // sender and recipient swap against the request
    f.push_back(to_addr);
    f.push_back(0x06);  // response / ACK
    f.push_back(static_cast<uint8_t>(7 + n + 3 - 2));
    f.push_back(cmd);
    f.push_back(ext);
    for (size_t i = 0; i < n; i++)
      f.push_back(data[i]);
    const uint16_t c = crc16(f.data(), 1, f.size());
    f.push_back(static_cast<uint8_t>(c & 0xFF));
    f.push_back(static_cast<uint8_t>(c >> 8));
    f.push_back(0x03);
    for (uint8_t b : f)
      tx.push_back(b);
  }

  void on_frame(const uint8_t *f, size_t n) {
    if (n < 10 || f[0] != 0x02)
      return;
    const uint8_t src = f[1], dst = f[2], cmd = f[5], ext = f[6];

    if (cmd == 0x02) {  // SAMPLES
      uint8_t sample[64]{};
      sample[0] = 0x47; sample[1] = 0x0D;  // flow ~33.99 C
      sample[40] = 8;                       // state
      sample[62] = 0;                       // stays 0 on a real PCU-05 P3
      sample[63] = service_mode ? 1 : 0;    // what actually tracks service mode
      respond(src, dst, cmd, ext, sample, sizeof(sample));
      return;
    }
    if (cmd == 0x10) {  // READ_EPROM_BLOCK
      reads++;
      read_dests.push_back(dst);
      if (!answer_reads)
        return;
      if (ext < 0x14 || ext > 0x1F)
        return;
      respond(src, dst, cmd, ext, eeprom[ext - 0x14], 16);
      return;
    }
    if (cmd == 0x11) {  // WRITE_EPROM_BLOCK
      if (require_service_for_write && !service_mode) {
        rejected_writes++;
        return;  // silence, which is one of the two plausible refusals
      }
      writes++;
      written_frames.emplace_back(f, f + n);
      if (writes_take_effect && ext >= 0x14 && ext <= 0x1F && n == 26)
        memcpy(eeprom[ext - 0x14], f + 7, 16);
      if (!answer_writes)
        return;
      respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x08) {  // CODE_SERVICE_START
      service_on++;
      if (service_mode_engages)
        service_mode = true;
      if (answer_service)
        respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x1F) {  // CODE_SERVICE_STOP
      service_off++;
      service_mode = false;
      if (answer_service)
        respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
  }
};

static FakeBoiler g_boiler;

namespace esphome {
namespace uart {
int UARTDevice::available() { return static_cast<int>(g_boiler.tx.size()); }
uint8_t UARTDevice::read() {
  if (g_boiler.tx.empty())
    return 0;
  const uint8_t b = g_boiler.tx.front();
  g_boiler.tx.pop_front();
  return b;
}
void UARTDevice::write_array(const uint8_t *data, size_t len) { g_boiler.on_frame(data, len); }
bool UARTDevice::check_uart_settings(uint32_t baud_rate) { return true; }
}  // namespace uart
}  // namespace esphome

// ---------------------------------------------------------------------------
// test scaffolding
// ---------------------------------------------------------------------------
static int g_failures = 0;

static void check(bool cond, const std::string &what) {
  printf("  [%s] %s\n", cond ? "ok" : "FAIL", what.c_str());
  if (!cond)
    g_failures++;
}

static bool logged(const char *needle) {
  for (const auto &l : g_log)
    if (l.find(needle) != std::string::npos)
      return true;
  return false;
}

// a full parameter write is 26 exchanges, so give it room
static void pump(esphome::dietrich::Dietrich &d, int steps = 700) {
  for (int i = 0; i < steps; i++) {
    d.loop();
    g_millis += 10;
  }
}

static void begin(const char *name) {
  printf("\n%s\n", name);
  g_log.clear();
  g_boiler.reset();
}

static esphome::dietrich::Dietrich *make() {
  auto *d = new esphome::dietrich::Dietrich();
  d->set_variant(esphome::dietrich::DIETRICH_VARIANT_PCU05_P3);
  d->set_allow_writes(true);
  return d;
}

int main() {
  using namespace esphome::dietrich;

  // -- 1. service mode test: unlock, re-lock, write nothing ------------------
  {
    begin("service mode test");
    auto *d = make();
    check(d->test_service_mode(), "request accepted");
    pump(*d);
    check(g_boiler.service_on == 1, "CODE_SERVICE_START sent once");
    check(g_boiler.service_off == 1, "CODE_SERVICE_STOP sent once");
    check(!g_boiler.service_mode, "boiler left locked");
    check(g_boiler.writes == 0, "nothing written to EEPROM");
    check(logged("the unlock took effect"), "service mode confirmed on from sample byte 62");
    check(logged("service mode test finished, nothing was written"), "reported as a clean no-write run");
    delete d;
  }

  // -- 1b. the unlock is ACKed but does nothing ------------------------------
  {
    begin("service mode is ACKed but does not engage");
    auto *d = make();
    g_boiler.service_mode_engages = false;
    check(d->test_service_mode(), "request accepted");
    pump(*d);
    check(logged("did NOT take effect"), "detected from sample byte 62, not from the ACK");
    delete d;
  }

  // -- 1c. a write is ACKed but ignored, as a PCU-05 P3 was seen doing --------
  {
    begin("write is ACKed but the boiler ignores it");
    auto *d = make();
    g_boiler.writes_take_effect = false;
    check(d->write_param(33, 6), "request accepted");
    pump(*d);
    check(g_boiler.writes == 8, "the whole parameter block was written");
    check(g_boiler.eeprom[2][0] == 4, "EEPROM unchanged, as the boiler chose");
    check(logged("read back different"), "caught by the verify read, not trusted from the ACK");
    check(!g_boiler.service_mode, "boiler left locked");
    delete d;
  }

  // -- 2. identity write: block goes back unchanged --------------------------
  {
    begin("identity write of block 0x16");
    auto *d = make();
    uint8_t before[16];
    memcpy(before, g_boiler.eeprom[2], 16);
    check(d->write_block_unchanged(0x16), "request accepted");
    pump(*d);
    check(g_boiler.writes == 1, "exactly one write frame sent - identity write stays single-block");
    check(memcmp(before, g_boiler.eeprom[2], 16) == 0, "EEPROM byte-for-byte unchanged");
    check(!g_boiler.service_mode, "boiler left locked");
    check(logged("identity write verified: 1 block(s) from 0x16"), "verified by read-back");
    if (!g_boiler.written_frames.empty()) {
      const auto &f = g_boiler.written_frames[0];
      check(f.size() == 26, "write frame is 26 bytes");
      check(f[3] == 0x05 && f[4] == 0x18 && f[5] == 0x11 && f[6] == 0x16, "type/len/COMMAND/EXT correct");
      check(f[1] == 0xFE && f[2] == 0x01, "addressed PC -> PCU");
      const uint16_t c = FakeBoiler::crc16(f.data(), 1, f.size() - 3);
      check((f[23] | (f[24] << 8)) == c && f[25] == 0x03, "CRC and ETX correct");
    }
    delete d;
  }

  // -- 3. parameter write: one byte changes, fifteen do not ------------------
  {
    begin("write p33 (hysteresis calorifier) 4 -> 6");
    auto *d = make();
    check(d->write_param(33, 6), "request accepted");
    pump(*d);
    check(g_boiler.writes == 8, "all eight parameter blocks written, as Recom does");
    check(g_boiler.eeprom[2][0] == 6, "p33 now reads 6");
    size_t changed = 0;
    for (int b = 0; b < 8; b++)
      for (int i = 0; i < 16; i++)
        if (g_boiler.eeprom[b][i] != REAL_PARAM_IMAGE[b * 16 + i])
          changed++;
    check(changed == 1, "exactly one byte of the whole 128 byte image changed");
    check(!g_boiler.service_mode, "boiler left locked");
    check(logged("parameter write verified: 8 block(s) from 0x14"), "verified by read-back");
    bool all_to_pcu = !g_boiler.read_dests.empty();
    for (uint8_t a : g_boiler.read_dests)
      if (a != 0x01)
        all_to_pcu = false;
    check(all_to_pcu, "every read in the transaction was addressed to the PCU (0x01), as Recom does");
    delete d;
  }

  // -- 4. a value the boiler already holds costs no EEPROM cycle -------------
  {
    begin("write p33 = 4 when it already reads 4");
    auto *d = make();
    check(d->write_param(33, 4), "request accepted");
    pump(*d);
    check(g_boiler.writes == 0, "no write frame sent at all");
    check(g_boiler.service_on == 1 && g_boiler.service_off == 1, "still unlocked and re-locked");
    check(!g_boiler.service_mode, "boiler left locked");
    check(logged("parameter already reads 4, skipping the write"), "skip reported");
    delete d;
  }

  // -- 5. clamping and the whitelist ----------------------------------------
  {
    begin("out-of-range and non-writable parameters");
    auto *d = make();
    check(d->write_param(33, 99), "p33 = 99 accepted (to be clamped)");
    pump(*d);
    check(g_boiler.eeprom[2][0] == 15, "clamped to the documented max of 15");
    check(logged("p33: 99 is outside 2..15, clamped to 15"), "clamp reported");

    g_log.clear();
    check(!d->write_param(17, 50), "p17 (full load HTG, gas/air) refused");
    check(logged("not one of the parameters this component will write"), "refusal reported");

    g_log.clear();
    check(!d->write_param(57, 90), "p57 (max controller temp) refused");

    g_log.clear();
    check(!d->write_block_unchanged(0x1C), "identity write outside 0x14..0x1B refused");
    delete d;
  }

  // -- 5b. an implausible read is refused rather than written back -----------
  {
    begin("a block that reads back implausibly is not written back");
    auto *d = make();
    // p17 (Full load HTG) is stored divided by 100, so 200 would mean 20000 rpm
    // against a documented 1000..10000. A real block never looks like this, so
    // this stands in for a corrupted read that still had a valid CRC.
    g_boiler.eeprom[1][0] = 200;
    check(d->write_param(33, 6), "request accepted");
    pump(*d);
    check(g_boiler.writes == 0, "nothing written at all");
    check(g_boiler.eeprom[2][0] == 4, "p33 untouched");
    check(logged("p17 (byte 16) reads 200, outside its documented 10..100"), "the offending byte is named");
    check(!g_boiler.service_mode, "boiler left locked");
    delete d;
  }

  // -- 6. the read fails: nothing must be written ----------------------------
  {
    begin("boiler goes silent on the in-transaction read");
    auto *d = make();
    uint8_t before[16];
    memcpy(before, g_boiler.eeprom[2], 16);
    g_boiler.answer_reads = false;
    check(d->write_param(33, 9), "request accepted");
    pump(*d, 800);
    check(g_boiler.writes == 0, "NO write frame sent");
    check(memcmp(before, g_boiler.eeprom[2], 16) == 0, "EEPROM untouched");
    check(g_boiler.service_off == 1, "re-lock still sent");
    check(!g_boiler.service_mode, "boiler left locked after the failure");
    check(logged("failed; service mode was re-locked"), "failure reported");
    delete d;
  }

  // -- 7. the write is not ACKed --------------------------------------------
  {
    begin("boiler does not ACK the write");
    auto *d = make();
    g_boiler.answer_writes = false;
    check(d->write_param(33, 9), "request accepted");
    pump(*d, 800);
    check(g_boiler.service_off == 1, "re-lock still sent");
    check(!g_boiler.service_mode, "boiler left locked after the failure");
    check(logged("failed; service mode was re-locked"), "treated as a failure, not a success");
    delete d;
  }

  // -- 8. unlock itself fails ------------------------------------------------
  {
    begin("boiler does not ACK the unlock");
    auto *d = make();
    g_boiler.answer_service = false;
    check(d->write_param(33, 9), "request accepted");
    pump(*d, 800);
    check(g_boiler.writes == 0, "no write attempted");
    check(g_boiler.service_off == 1, "re-lock attempted anyway");
    delete d;
  }

  // -- 9. gates ---------------------------------------------------------------
  {
    begin("allow_writes and variant gates");
    auto *d = new Dietrich();
    d->set_variant(DIETRICH_VARIANT_PCU05_P3);
    check(!d->write_param(33, 5), "refused with allow_writes off");
    check(logged("allow_writes is not set"), "refusal explains why");
    delete d;

    g_log.clear();
    auto *e = new Dietrich();
    e->set_variant(DIETRICH_VARIANT_MCR3);
    e->set_allow_writes(true);
    check(!e->write_param(33, 5), "refused on a non-pcu05_p3 variant");
    check(logged("only supported on variant pcu05_p3"), "refusal explains why");
    delete e;
  }

  // -- 10. only one transaction at a time ------------------------------------
  {
    begin("a second write while one is in flight");
    auto *d = make();
    check(d->write_param(33, 7), "first accepted");
    check(!d->write_param(2, 50), "second refused");
    check(logged("another write is already in progress"), "refusal reported");
    pump(*d);
    check(g_boiler.eeprom[2][0] == 7, "the first one still completed");
    check(g_boiler.eeprom[0][1] == 56, "the second one did not happen");
    delete d;
  }

  // -- 11. a boiler found unlocked at boot gets re-locked --------------------
  {
    begin("boiler is already in service mode at boot");
    auto *d = make();
    g_boiler.service_mode = true;
    // the first poll after boot is a counter poll (counter_timer_ starts at 99),
    // so it takes a second interval before a sample carries byte 62
    for (int i = 0; i < 3; i++) {
      d->update();
      pump(*d, 150);
    }
    check(logged("boiler is in service mode at boot"), "noticed");
    check(g_boiler.service_off >= 1, "CODE_SERVICE_STOP sent");
    check(!g_boiler.service_mode, "boiler re-locked");
    check(g_boiler.writes == 0, "nothing written");
    delete d;
  }

  // -- 12. ordinary polling still works --------------------------------------
  {
    begin("polling is undisturbed");
    auto *d = make();
    for (int i = 0; i < 3; i++) {
      d->update();
      pump(*d, 100);
    }
    check(!logged("rejected"), "no sample response was rejected");
    check(g_boiler.writes == 0 && g_boiler.service_on == 0, "polling never unlocks or writes");
    delete d;
  }

  printf("\n%s (%d failed)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
  return g_failures == 0 ? 0 : 1;
}
