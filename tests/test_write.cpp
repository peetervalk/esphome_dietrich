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
  uint8_t eeprom[12][16]{};  // blocks 0x14..0x1F, at the address in eeprom_addr
  // The real board answers READ_EPROM_BLOCK on both 0x00 and 0x01, echoing back
  // whichever it was sent - but only one of them holds the parameter image the
  // boiler runs on. The other answers sixteen FF bytes for every block it has
  // never been written, and quietly keeps whatever is written to it. That is what
  // made a write ACK and change nothing; see mapping/pcu05_p3_protocol.md.
  uint8_t stray[12][16]{};
  uint8_t eeprom_addr{0x00};
  // Service mode is per device address, as the board turned out to work: the
  // unlock at 0x01 is the one visible in the sample, and the EEPROM at 0x00 has
  // its own and NAKs a write until it is given one.
  bool service_mode{false};      // at 0x01, reported by sample byte 63
  bool service_mode_ee{false};   // at eeprom_addr
  bool answer_reads{true};
  bool answer_writes{true};
  bool answer_service{true};
  // CODE_SERVICE_STOP frames whose reply is swallowed on the way back. The board
  // acts on them; only the ACK is lost. A PCU-05 P3 did exactly this to the
  // re-lock at 0x00 on 2026-09-16, after a write that had already verified.
  int drop_service_off{0};
  // enforce what Recom's sequence implies: no EEPROM write while locked
  bool require_service_for_write{true};
  // CODE_SERVICE_START is ACKed but does not actually unlock
  bool service_mode_engages{true};
  // WRITE_EPROM_BLOCK is ACKed but the bytes are not stored - what a PCU-05 P3
  // was observed doing for a single-block write on 2026-09-16
  bool writes_take_effect{true};

  // What the sample says the boiler is doing. The defaults are the quiet boiler
  // every existing test assumes; the write-gate tests set them to a running one.
  uint8_t status{8};       // 8: controlled stop
  uint8_t substatus{0};    // 0: standby
  uint8_t blocking{0xFF};  // 255: none
  uint16_t fan{0};
  uint8_t ionisation{0};
  // A PCU-05 P3 answers a parameter write by going into blocking mode within
  // fifteen seconds; -1 is a board that does not. See
  // mapping/pcu05_p3_protocol.md, *What cleared it*.
  int blocking_after_write{-1};

  // COMMAND 0x09 / EXT 0x52, the factory-level unlock. No real board has been
  // asked for it yet, so the simulator can answer either way: answer_factory
  // false is a board that parses the frame and turns it down.
  bool factory_mode{false};
  bool factory_mode_ee{false};
  bool answer_factory{true};
  int factory_on{0}, factory_off{0};

  int reads{0}, writes{0}, service_on{0}, service_off{0}, rejected_writes{0};
  int resets{0};
  int idents{0};
  std::vector<uint8_t> ident_dests;  // the address each IDENTIFICATION was sent to
  // Data bytes each address answers IDENTIFICATION with: 16 is the per-device
  // layout a PCU-05 P3 really returned at 0x01, 64 the appliance layout that
  // carries dF/dU, 0 silence. 0x00 was silent on the live board.
  int ident_len_pcu{16};
  int ident_len_psu{0};
  // Everything outside 0x14..0x1F, which no map covers. 0xFF except where a test
  // plants something; block 0x05 carries a string so the dump's ASCII column has
  // something to render.
  uint8_t rest[128][16]{};
  int silent_block{-1};  // this block answers nothing, as an absent one would
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
    memset(stray, 0xFF, sizeof(stray));
    eeprom_addr = 0x00;
    service_mode = service_mode_ee = false;
    answer_reads = answer_writes = answer_service = true;
    drop_service_off = 0;
    require_service_for_write = true;
    service_mode_engages = true;
    writes_take_effect = true;
    factory_mode = factory_mode_ee = false;
    answer_factory = true;
    factory_on = factory_off = 0;
    reads = writes = service_on = service_off = rejected_writes = 0;
    resets = 0;
    status = 8;
    substatus = 0;
    blocking = 0xFF;
    fan = 0;
    ionisation = 0;
    blocking_after_write = -1;
    idents = 0;
    ident_dests.clear();
    ident_len_pcu = 16;
    ident_len_psu = 0;
    memset(rest, 0xFF, sizeof(rest));
    memcpy(rest[0x05], "PCU05P3TESTBLOCK", 16);
    silent_block = -1;
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
      sample[26] = ionisation;
      sample[40] = status;
      sample[42] = blocking;
      sample[43] = substatus;
      sample[44] = static_cast<uint8_t>(fan >> 8);
      sample[45] = static_cast<uint8_t>(fan & 0xFF);
      // Byte 62 stays 0 on a real PCU-05 P3 through a service-level unlock. It is
      // the candidate flag for the factory one purely because it is the byte the
      // map calls service_mode and the only one left that never moves; no board
      // has confirmed it. The simulator makes it track factory mode so the
      // component's readback has something to report - that is an assumption
      // under test, not a fact.
      sample[62] = factory_mode ? 1 : 0;
      sample[63] = service_mode ? 1 : 0;    // what actually tracks service mode
      respond(src, dst, cmd, ext, sample, sizeof(sample));
      return;
    }
    if (cmd == 0x01) {  // IDENTIFICATION
      idents++;
      ident_dests.push_back(dst);
      const int len = dst == 0x00 ? ident_len_psu : ident_len_pcu;
      if (len == 16) {
        // Byte for byte what a PCU-05 P3 answered at 0x01 on 2026-09-16 15:28:
        // groups 2-4's per-device layout, with the serial number unset.
        static const uint8_t REAL[16] = {0x05, 0x17, 0xFF, 0x03, 0x6E, 0x8C, 0x01, 0x04,
                                         0x01, 0x24, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        respond(src, dst, cmd, ext, REAL, sizeof(REAL));
      } else if (len == 64) {
        // group 1, the appliance layout. Synthetic - no board has served it yet -
        // but the offsets are the map's.
        uint8_t id[64]{};
        id[1] = 7;      // dF-code
        id[2] = 12;     // dU-code
        id[5] = 0x1A;   // software version
        id[6] = 0x03;   // parameter version
        id[7] = 0x01;   // parameter type
        id[10] = 4;     // next service code
        id[16] = 0x02;  // connected PSU type
        id[17] = 0x05;  // connected PCU type
        memcpy(id + 32, "0123456789AB    ", 16);
        memcpy(id + 48, "PCU-05 TEST     ", 16);
        respond(src, dst, cmd, ext, id, sizeof(id));
      }
      return;
    }
    if (cmd == 0x10) {  // READ_EPROM_BLOCK
      reads++;
      read_dests.push_back(dst);
      if (!answer_reads)
        return;
      if (silent_block >= 0 && ext == silent_block)
        return;
      if (ext < 0x14 || ext > 0x1F) {
        respond(src, dst, cmd, ext, rest[ext], 16);
        return;
      }
      respond(src, dst, cmd, ext, dst == eeprom_addr ? eeprom[ext - 0x14] : stray[ext - 0x14], 16);
      return;
    }
    if (cmd == 0x11) {  // WRITE_EPROM_BLOCK
      const bool unlocked = dst == eeprom_addr ? (service_mode_ee || factory_mode_ee) : (service_mode || factory_mode);
      if (require_service_for_write && !unlocked) {
        rejected_writes++;
        if (!answer_writes)
          return;  // silence, the other plausible refusal
        // a NAK: understood, addressed to the right device, and turned down
        std::vector<uint8_t> nak{0x02, dst, src, 0x15, 0x08, cmd, ext};
        const uint16_t c = crc16(nak.data(), 1, nak.size());
        nak.push_back(static_cast<uint8_t>(c & 0xFF));
        nak.push_back(static_cast<uint8_t>(c >> 8));
        nak.push_back(0x03);
        for (uint8_t b : nak)
          tx.push_back(b);
        return;
      }
      writes++;
      written_frames.emplace_back(f, f + n);
      if (blocking_after_write >= 0)
        blocking = static_cast<uint8_t>(blocking_after_write);
      if (writes_take_effect && ext >= 0x14 && ext <= 0x1F && n == 26)
        memcpy(dst == eeprom_addr ? eeprom[ext - 0x14] : stray[ext - 0x14], f + 7, 16);
      if (!answer_writes)
        return;
      respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x09) {  // CODE_FACTORY_COMMANDO
      factory_on++;
      if (!answer_factory) {
        std::vector<uint8_t> nak{0x02, dst, src, 0x15, 0x08, cmd, ext};
        const uint16_t c = crc16(nak.data(), 1, nak.size());
        nak.push_back(static_cast<uint8_t>(c & 0xFF));
        nak.push_back(static_cast<uint8_t>(c >> 8));
        nak.push_back(0x03);
        for (uint8_t b : nak)
          tx.push_back(b);
        return;
      }
      if (dst == eeprom_addr)
        factory_mode_ee = true;
      else
        factory_mode = true;
      respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x1F && ext == 0x52) {  // the factory re-lock, before the service one
      factory_off++;
      if (dst == eeprom_addr)
        factory_mode_ee = false;
      else
        factory_mode = false;
      if (answer_factory)
        respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x31) {  // RESET
      resets++;
      respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x08) {  // CODE_SERVICE_START
      service_on++;
      if (service_mode_engages) {
        if (dst == eeprom_addr)
          service_mode_ee = true;
        else
          service_mode = true;
      }
      if (answer_service)
        respond(src, dst, cmd, ext, nullptr, 0);
      return;
    }
    if (cmd == 0x1F) {  // CODE_SERVICE_STOP
      service_off++;
      if (dst == eeprom_addr)
        service_mode_ee = false;
      else
        service_mode = false;
      if (drop_service_off > 0) {
        drop_service_off--;
        return;  // acted on, but the ACK never arrives
      }
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
      check(f[1] == 0xFE && f[2] == 0x00, "addressed to the same device the reads go to");
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
    bool one_address = !g_boiler.read_dests.empty();
    for (uint8_t a : g_boiler.read_dests)
      if (a != 0x00)
        one_address = false;
    check(one_address, "every read in the transaction went to the device that holds the image");
    delete d;
  }

  // -- 3d. the re-lock reply is lost once: resent, and the write still stands -
  {
    begin("re-lock reply lost once");
    auto *d = make();
    g_boiler.drop_service_off = 1;  // the EEPROM address's re-lock, as seen live
    check(d->write_param(33, 6), "request accepted");
    pump(*d, 800);
    check(g_boiler.eeprom[2][0] == 6, "p33 was written");
    check(logged("re-lock got no usable reply, sending it again"), "the re-lock was resent");
    check(g_boiler.service_off == 3, "three CODE_SERVICE_STOP frames: the resend plus one per address");
    check(logged("parameter write verified: 8 block(s) from 0x14"), "still reported as verified");
    check(!logged("parameter write failed"), "not reported as a failure");
    check(!g_boiler.service_mode && !g_boiler.service_mode_ee, "both addresses left locked");
    delete d;
  }

  // -- 3e. the re-lock stays unanswered: reported, but the verify still counts -
  {
    begin("re-lock reply lost for good");
    auto *d = make();
    g_boiler.drop_service_off = 2;  // both attempts at the EEPROM address
    check(d->write_param(33, 6), "request accepted");
    pump(*d, 800);
    check(g_boiler.eeprom[2][0] == 6, "p33 was written");
    check(logged("parameter write verified: 8 block(s) from 0x14"),
          "a verified write survives a re-lock that went wrong after it");
    check(!logged("parameter write failed"), "the write is not what failed, so it is not called a failure");
    check(logged("an address may still be unlocked"), "the re-lock is reported on its own");
    delete d;
  }

  // -- 3b. the reads land on the device that does not hold the image ---------
  {
    begin("reads answered by the wrong device (FF for every block)");
    auto *d = make();
    g_boiler.eeprom_addr = 0x01;  // the parameter image is not where we are asking
    check(d->write_param(33, 6), "request accepted");
    pump(*d);
    check(g_boiler.writes == 0, "not one byte written on the back of an FF read");
    check(logged("block 0x14 read back implausible"),
          "refused at the first block, not after reading all eight");
    check(!logged("block 0x15 read back implausible"), "gave up immediately rather than reading on");
    check(!logged("parameter block: FFFFFFFF"), "the FF read never reached the published image");
    check(g_boiler.service_off >= 1 && !g_boiler.service_mode, "boiler left locked");
    check(logged("parameter write failed"), "reported as a failure");
    delete d;
  }

  // -- 3c. the EEPROM address is not unlocked, so it refuses the write -------
  {
    begin("write to an address that was never unlocked");
    auto *d = make();
    g_boiler.require_service_for_write = true;
    check(d->write_param(33, 6), "request accepted");
    // the unlock reaches 0x01 but is dropped on the way to the EEPROM address
    g_boiler.service_mode_engages = true;
    pump(*d, 40);
    g_boiler.service_mode_ee = false;  // as if that unlock had never landed
    pump(*d);
    check(g_boiler.eeprom[2][0] == 4, "p33 untouched");
    check(logged("refused it (NAK)"), "the refusal is reported as a refusal, not a bad frame");
    check(!g_boiler.service_mode && !g_boiler.service_mode_ee, "both addresses left locked");
    delete d;
  }

  // -- 4. a value the boiler already holds costs no EEPROM cycle -------------
  {
    begin("write p33 = 4 when it already reads 4");
    auto *d = make();
    check(d->write_param(33, 4), "request accepted");
    pump(*d);
    check(g_boiler.writes == 0, "no write frame sent at all");
    check(g_boiler.service_on == 2 && g_boiler.service_off == 2, "still unlocked and re-locked, both addresses");
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
    check(logged("block 0x15 read back implausible: p17 (byte 16) is 200, outside its documented 10..100"),
          "the offending byte is named, and the block it came in");
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
    check(g_boiler.service_off == 2, "both re-locks still sent");
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
    check(g_boiler.service_off == 2, "both re-locks still sent");
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
    // two addresses, and a re-lock that goes unanswered is resent once
    check(g_boiler.service_off == 4, "both re-locks attempted anyway, each resent once");
    check(logged("an address may still be unlocked"), "the unanswered re-locks are reported");
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

  // -- 13. identification: read-only, and asked once on connect -------------
  {
    begin("identification on connect");
    auto *d = make();
    d->update();
    pump(*d, 200);
    check(g_boiler.idents == 2, "IDENTIFICATION sent on the very first poll, as Recom opens with");
    check(g_boiler.ident_dests == std::vector<uint8_t>({0x01, 0x00}), "both addresses asked, PCU first");
    // the per-device layout, decoded off the live capture in the fake boiler
    check(logged("device type 5, software version 23, parameter version 255, type 3"), "versions decoded");
    check(logged("operating hours 56600, connected SU type 1, connected PSU type 4"),
          "operating hours use the PCU's x2 scaling, and the connected device is named for the address");
    check(logged("last blocking code 1, last locking code 36"), "the stored fault history is decoded");
    check(logged("serial number (raw): FF FF FF FF FF"), "the 5 byte serial goes out raw, not guessed at");
    check(!logged("dF-code"), "no dF/dU claimed from a reply that does not carry it");
    check(logged("no response to identification 0x00 request"), "the silent address is reported, not decoded");
    check(g_boiler.writes == 0 && g_boiler.service_on == 0, "read-only: nothing written, never unlocked");

    g_log.clear();
    for (int i = 0; i < 3; i++) {
      d->update();
      pump(*d, 120);
    }
    check(g_boiler.idents == 2, "not asked again on later polls");
    delete d;
  }

  // -- 13b. the appliance layout, wherever it turns up ------------------------
  {
    begin("identification, appliance layout");
    auto *d = make();
    g_boiler.ident_len_psu = 64;  // as if 0x00 served group 1
    d->update();
    pump(*d, 200);
    check(logged("dF-code 7, dU-code 12"), "the plate codes are decoded from the long reply");
    check(logged("serial number: 0123456789AB"), "serial read as text, padding trimmed");
    check(logged("boiler name: PCU-05 TEST"), "boiler name read as text, padding trimmed");
    check(logged("device type 5"), "and the short reply from 0x01 is still decoded its own way");
    delete d;
  }

  // -- 13c. asking for it again ----------------------------------------------
  {
    begin("identification on request");
    auto *d = make();
    d->update();
    pump(*d, 200);  // the pair on connect
    check(d->read_identification(), "request accepted");
    check(logged("identification queued"), "acceptance reported");
    d->update();
    pump(*d, 200);
    check(g_boiler.idents == 4, "both addresses asked a second time");
    delete d;
  }

  // -- 13d. not on a variant whose layout this is not ------------------------
  {
    begin("identification is gated on the variant");
    auto *e = new Dietrich();
    e->set_variant(DIETRICH_VARIANT_MCR3);
    check(!e->read_identification(), "refused on a non-pcu05_p3 variant");
    check(logged("only supported on variant pcu05_p3"), "refusal explains why");
    e->update();
    pump(*e, 150);
    check(g_boiler.idents == 0, "and never sent unasked either");
    delete e;
  }

  // -- 14. EEPROM dump: a read-only sweep of the unmapped blocks -------------
  {
    begin("eeprom dump");
    auto *d = make();
    g_boiler.silent_block = 0x03;
    check(d->dump_eeprom(0x00, 0x00, 16), "request accepted");
    check(logged("blocks 0x00..0x0F, read-only"), "the range is reported");
    d->update();
    pump(*d, 1500);
    check(g_boiler.reads == 16, "every block in the range was asked for, exactly once");
    check(logged("eeprom 00:05"), "blocks are logged by address and index");
    check(logged("|PCU05P3TESTBLOCK|"), "the ASCII column renders a string in an unmapped block");
    check(logged("no response to eeprom 0x03 request"), "a silent block is reported");
    check(logged("eeprom 00:04"), "and stepped over rather than ending the sweep");
    check(logged("eeprom dump of 0x00 finished"), "completion reported");
    check(g_boiler.writes == 0 && g_boiler.service_on == 0, "read-only: nothing written, never unlocked");
    delete d;
  }

  // -- 14b. the dump keeps out of the way ------------------------------------
  {
    begin("eeprom dump gates");
    auto *d = make();
    check(!d->dump_eeprom(0x02, 0, 4), "a device address that is not 0x00 or 0x01 is refused");
    check(logged("is not a device address"), "refusal explains why");

    g_log.clear();
    check(d->write_param(33, 6), "a write is staged");
    check(!d->dump_eeprom(0x00, 0, 4), "and the dump refuses to share the bus with it");
    check(logged("the bus is busy"), "refusal explains why");
    pump(*d, 900);
    check(g_boiler.eeprom[2][0] == 6, "the write still completed undisturbed");
    delete d;
  }

  // -- 15. the write waits for a quiet boiler --------------------------------
  {
    begin("a burning boiler is not written to");
    auto *d = make();
    g_boiler.status = 4;      // burning DHW
    g_boiler.substatus = 32;  // normal power control
    g_boiler.fan = 4200;
    g_boiler.ionisation = 62;
    check(d->write_param(33, 6), "request accepted - the boiler has not been asked yet");
    pump(*d);
    check(g_boiler.writes == 0, "no write frame was sent");
    check(g_boiler.service_on == 0, "and service mode was never unlocked");
    check(g_boiler.eeprom[2][0] == 4, "EEPROM untouched");
    check(logged("write refused: the boiler is running"), "the pre-flight sample says why");
    check(logged("refused: the boiler was not quiet enough"), "and the transaction reports itself refused");
    delete d;
  }

  // -- 15b. the exact state the 2026-09-16 write went out in ------------------
  {
    begin("a boiler finishing a charge is not written to either");
    auto *d = make();
    g_boiler.status = 8;      // controlled stop - the burner is already off
    g_boiler.substatus = 60;  // ...but the pump is still running it out
    check(d->write_param(33, 6), "request accepted");
    pump(*d);
    check(g_boiler.writes == 0, "no write frame was sent");
    check(logged("part-way through a cycle"), "the sub state is what gives it away");
    delete d;
  }

  // -- 15c. anti-cycling is quiet enough --------------------------------------
  {
    begin("a boiler in the anti-cycle wait is quiet enough");
    auto *d = make();
    g_boiler.status = 8;
    g_boiler.substatus = 1;  // anti-cycling
    check(d->write_param(33, 6), "request accepted");
    pump(*d, 900);
    check(g_boiler.writes == 8, "the whole parameter block was written");
    check(g_boiler.eeprom[2][0] == 6, "and it took effect");
    delete d;
  }

  // -- 15d. a blocked board can still be written back ------------------------
  {
    begin("a board already in blocking mode can be written to");
    auto *d = make();
    g_boiler.status = 9;     // blocking mode
    g_boiler.blocking = 0;   // PCU parameter fault
    g_boiler.substatus = 0;
    check(d->write_param(33, 6), "request accepted");
    pump(*d, 900);
    check(g_boiler.writes == 8, "the write goes out - this is how the p33 write was undone");
    delete d;
  }

  // -- 16. a blocking code brought on by the write is reported ---------------
  {
    begin("a write that verifies but leaves the boiler blocking");
    auto *d = make();
    g_boiler.blocking_after_write = 20;  // identification running
    check(d->write_param(33, 6), "request accepted");
    pump(*d, 900);
    check(g_boiler.eeprom[2][0] == 6, "the write landed");
    check(logged("parameter write verified"), "and verified byte-for-byte");
    check(logged("blocking code went 255 -> 20"), "the post-write sample reports the blocking anyway");
    check(logged("mains power cycle"), "and says what clears it");
    delete d;
  }

  // -- 16b. a write that changes nothing says so ------------------------------
  {
    begin("a write on a board that does not block");
    auto *d = make();
    check(d->write_param(33, 6), "request accepted");
    pump(*d, 900);
    check(logged("blocking code unchanged at 255"), "the post-write sample is reported either way");
    delete d;
  }

  // -- 17. the board reset is one frame and nothing else ---------------------
  {
    begin("board reset");
    auto *d = make();
    check(d->reset_board(), "request accepted");
    pump(*d);
    check(g_boiler.resets == 1, "COMMAND 0x31 sent once");
    check(g_boiler.writes == 0, "nothing written");
    check(g_boiler.reads == 0, "nothing read");
    check(g_boiler.service_on == 0 && g_boiler.service_off == 0, "service mode never touched");
    delete d;
  }

  // -- 17b. and it is gated like every other intrusive command ---------------
  {
    begin("board reset gates");
    auto *d = new Dietrich();
    d->set_variant(DIETRICH_VARIANT_PCU05_P3);
    check(!d->reset_board(), "refused without allow_writes");
    check(logged("allow_writes is not set"), "refusal explains why");
    pump(*d);
    check(g_boiler.resets == 0, "and no frame was sent");
    delete d;
  }

  // -- 18. factory level: unlock both addresses, sample, re-lock -------------
  {
    begin("factory mode test");
    auto *d = make();
    check(d->test_factory_mode(), "request accepted");
    pump(*d);
    check(g_boiler.factory_on == 2, "COMMAND 0x09 sent to both addresses");
    check(g_boiler.factory_off == 2, "and both re-locked");
    check(!g_boiler.factory_mode && !g_boiler.factory_mode_ee, "boiler left locked");
    check(g_boiler.service_on == 0, "the service-level unlock was not sent");
    check(g_boiler.writes == 0 && g_boiler.reads == 0, "nothing read, nothing written");
    check(logged("factory mode readback: byte 62 = 1"), "the sample taken inside the window is reported");
    delete d;
  }

  // -- 18b. a board that parses COMMAND 0x09 and refuses it ------------------
  {
    begin("factory mode is NAKed");
    auto *d = make();
    g_boiler.answer_factory = false;
    check(d->test_factory_mode(), "request accepted");
    pump(*d);
    check(g_boiler.factory_on >= 1, "the frame went out");
    check(logged("refused it (NAK)"), "the refusal is reported, not swallowed");
    delete d;
  }

  // -- 18c. a parameter write at factory level uses 0x09, never 0x08 ---------
  {
    begin("write_param at factory level");
    auto *d = make();
    d->set_use_factory_mode(true);
    check(d->write_param(33, 6), "request accepted");
    pump(*d);
    check(g_boiler.factory_on == 2 && g_boiler.factory_off == 2, "unlocked and re-locked at factory level");
    check(g_boiler.service_on == 0 && g_boiler.service_off == 0,
          "CODE_SERVICE never sent - it is a level, not an addition");
    check(g_boiler.writes == 8, "the whole parameter block was written");
    check(g_boiler.eeprom[2][0] == 6, "p33 took effect");
    delete d;
  }

  // -- 19. send_command builds the frame; the CRC cannot be got wrong --------
  {
    begin("send_command builds a valid frame");
    auto *d = make();
    check(d->send_command(0x01, 0x09, 0x52, ""), "request accepted");
    pump(*d);
    check(g_boiler.factory_on == 1, "the board saw COMMAND 0x09");
    // exactly the frame this component hardcodes for the factory unlock at 0x01
    check(logged("frame: 02FE01050809522EA603"), "frame matches the computed constant");
    check(logged("raw frame: a response"), "the reply is decoded and reported");
    delete d;
  }

  // -- 19b. with a payload, and the length byte follows it -------------------
  {
    begin("send_command with a payload");
    auto *d = make();
    check(d->send_command(0x01, 0x37, 0x00, "0C 00"), "spaces in the payload are fine");
    pump(*d);
    check(logged("frame: 02FE01050A37000C004D2303"), "length byte and CRC computed for the payload");
    delete d;
  }

  // -- 19bb. the one-string form, which is what a text box in HA sends -------
  {
    begin("send_command_hex from a single string");
    auto *d = make();
    check(d->send_command_hex("01 09 52"), "recipient, command and ext");
    pump(*d);
    check(g_boiler.factory_on == 1, "the board saw COMMAND 0x09 at 0x01");
    check(logged("frame: 02FE01050809522EA603"), "same frame as the typed form");
    delete d;
  }

  // -- 19bc. with a payload, and too short to be one -------------------------
  {
    begin("send_command_hex payload and refusal");
    auto *d = make();
    check(d->send_command_hex("01 37 00 0C 00"), "payload bytes follow the ext");
    pump(*d);
    check(logged("frame: 02FE01050A37000C004D2303"), "length byte and CRC follow the payload");
    check(!d->send_command_hex("01 09"), "fewer than three bytes is refused");
    check(logged("needs at least recipient, command and ext"), "and says what it wanted");
    delete d;
  }

  // -- 19c. bad hex is refused before anything is sent -----------------------
  {
    begin("send_command rejects bad hex");
    auto *d = make();
    check(!d->send_command(0x01, 0x09, 0x52, "0C0"), "odd digit count refused");
    check(!d->send_command(0x01, 0x09, 0x52, "ZZ"), "non-hex refused");
    pump(*d);
    check(g_boiler.factory_on == 0, "and nothing went out");
    delete d;
  }

  // -- 20. send_raw sends the bytes exactly as given -------------------------
  {
    begin("send_raw replays a frame verbatim");
    auto *d = make();
    check(d->send_raw("02 FE 01 05 08 08 0C AE CE 03"), "request accepted");
    pump(*d);
    check(g_boiler.service_on == 1, "the board saw CODE_SERVICE_START");
    check(logged("raw frame: a response"), "the ACK is reported");
    delete d;
  }

  // -- 20b. a bad CRC is flagged and sent anyway -----------------------------
  {
    begin("send_raw warns about a bad CRC but still sends");
    auto *d = make();
    check(d->send_raw("02FE0105 08080C 0000 03"), "request accepted");
    pump(*d);
    check(logged("the CRC in this frame is not the one"), "the mismatch is called out");
    check(g_boiler.service_on == 1, "and the frame still went out - a refusal is a result");
    delete d;
  }

  // -- 20c. too short to be a frame ------------------------------------------
  {
    begin("send_raw rejects a stub");
    auto *d = make();
    check(!d->send_raw("02FE0105"), "shorter than the minimum frame");
    pump(*d);
    check(g_boiler.service_on == 0, "nothing sent");
    delete d;
  }

  // -- 20d. silence is reported rather than treated as a failure -------------
  {
    begin("send_raw when the board says nothing");
    auto *d = make();
    g_boiler.answer_service = false;
    check(d->send_raw("02 FE 01 05 08 08 0C AE CE 03"), "request accepted");
    pump(*d);
    check(logged("no reply at all"), "silence is reported");
    check(!logged("raw frame failed"), "and is not counted as a transaction failure");
    delete d;
  }

  // -- 20e. the raw path is gated like every other intrusive command ---------
  {
    begin("raw frames are gated");
    auto *d = new Dietrich();
    d->set_variant(DIETRICH_VARIANT_PCU05_P3);
    check(!d->send_raw("02 FE 01 05 08 08 0C AE CE 03"), "send_raw refused without allow_writes");
    check(!d->send_command(0x01, 0x09, 0x52, ""), "send_command refused too");
    check(!d->test_factory_mode(), "and so is the factory mode test");
    pump(*d);
    check(g_boiler.service_on == 0 && g_boiler.factory_on == 0, "nothing went out");
    delete d;
  }

  printf("\n%s (%d failed)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
  return g_failures == 0 ? 0 : 1;
}
