# De Dietrich / Remeha PCU-05 P3 over ESPHome

Native ESPHome **external component** for a De Dietrich (or Remeha) boiler with a
**PCU-05 control board running parameter set P3**, over the board's PC/service
interface. It reads the full sample block and the stored parameter set, and it can
write parameters back. Works on **ESP32** (e.g. DevKit V4) and **ESP8266** (e.g.
Wemos D1).

> **Scope.** This started as a fork of
> [kakaki/esphome_dietrich](https://github.com/kakaki/esphome_dietrich), which
> supports the **MCR3** and **Calenta / MCX Plus / Avanta V1_P5** boards. Those
> variants have been removed here: everything below the sample decode — the
> parameter image, its CRCs, the service-mode unlock and the whole write path — is
> specific to the PCU-05 P3 parameter set, and keeping variants nobody could test
> meant shipping code that was only plausibly correct. **If your board is an MCR3
> or an Avanta/Calenta, use
> [kakaki/esphome_dietrich](https://github.com/kakaki/esphome_dietrich) instead.**

## Usage

Since ESPHome 2025.2 the old `platform: custom` + `includes:` mechanism is removed, so this
project is a proper external component. Add it to your YAML:

```yaml
external_components:
  - source: github://peetervalk/esphome_dietrich
    components: [ dietrich ]

uart:
  id: uart_bus
  baud_rate: 9600
  tx_pin: GPIO18
  rx_pin: GPIO19

dietrich:
  uart_id: uart_bus
  update_interval: 15s
  flow_temp:
    name: "Boiler flow temp"
  state_text:
    name: "Boiler state"     # decoded status text, no lambda needed
  # ... see the example YAML for the full sensor list
```

Full example: **[dietrich_pcu05_p3_en.yaml](dietrich_pcu05_p3_en.yaml)** (ESP32,
English) — every sensor the component exposes, the stored-parameter block, the
read-only diagnostics and the complete write UI, all commented.

### Protocol

The PCU-05 speaks the Remeha protocol (`protocol.nr` 1 in Recom's
`DeviceConfiguration.xml`): CRC16, 7-byte request and response header. Recom groups
other boards into other wire protocols — the Avanta protocol (`protocol.nr` 2, XOR
checksum, 6-byte header) is the other common one — and this component no longer
implements any of them.

### Identification

The component asks both device addresses who they think they
are, on the first poll after boot, the way Recom opens a connection. It is a plain
read — no service mode, nothing written — so it is not gated behind `allow_writes`
and needs no configuration. The answer goes to the log:

```
[I][dietrich]: identification 0x01: device type 5, software version 23, parameter version 255, type 3
[I][dietrich]:   operating hours 56600, connected SU type 1, connected PSU type 4
[I][dietrich]:   last blocking code 1, last locking code 36
[I][dietrich]:   serial number (raw): FF FF FF FF FF
[I][dietrich]: identification 0x00: dF-code 19, dU-code 2 (compare these with the identification plate)
[I][dietrich]:   software version 23, parameter version 255, parameter type 3 (raw bytes)
[I][dietrich]:   next service code 0, connected PSU type 4, connected PCU type 5, SCU-C 255
[I][dietrich]:   serial number: 1832720103840
[I][dietrich]:   boiler name: Tzerra Export
```

The reply's length picks the layout. A PCU-05 P3 answers at `0x01` with the 16-byte
per-device form and at `0x00` with the 64-byte appliance form, which is the one
carrying the **dF/dU codes**, the full serial number and the boiler name. Those
codes are the ones printed on the identification plate and the ones a
factory-settings restore asks for, and they are **not** in the parameter block, so
no parameter write can disturb them. See
[mapping/pcu05_p3_protocol.md](mapping/pcu05_p3_protocol.md) for both layouts.

To ask again, e.g. from a diagnostic button, `read_identification()` sends it on the
next poll interval:

```yaml
button:
  - platform: template
    name: "Boiler identification"
    entity_category: diagnostic
    on_press:
      - lambda: 'id(boiler).read_identification();'
```

### Dumping the EEPROM (`pcu05_p3` only)

`dump_eeprom(addr, first, count)` reads a range of EEPROM blocks and logs each as
hex and ASCII. `READ_EPROM_BLOCK` needs no service mode, so like identification it
is read-only and works with `allow_writes` off:

```yaml
button:
  - platform: template
    name: "Boiler dump EEPROM 0x00"
    entity_category: diagnostic
    on_press:
      - lambda: 'id(boiler).dump_eeprom(0x00, 0x00, 128);'
```

```
[I][dietrich]: eeprom 00:05 5043552D30352050332054455354 |PCU05P3TESTBLOC|
```

The EEPROM is 2 KB in 128 blocks of 16 bytes, of which only `0x14`–`0x1B`
(parameters) and `0x1C`–`0x1F` (counters) are mapped. The other 116 are where the
appliance identification and the boiler's fault history records have to be — the
history records are 16 bytes each, exactly one block, and carry an operating-hours
stamp alongside the fault code. A full sweep is about 45 seconds and borrows the
poll intervals it needs. Both device addresses are worth sweeping; they answer with
different contents.

### Writing parameters (`pcu05_p3` only)

The component can also write the boiler's stored parameters back to EEPROM. It
stays off unless you ask for it:

```yaml
dietrich:
  id: boiler
  allow_writes: true
```

Writes are driven by entities, not by a frame from Home Assistant. A dropdown
holds the parameter — listing only the ones the component is willing to write,
each with its range — and a box holds the value; a button stages that pair; a
second button writes everything staged. Only the `pNN` number and the value ever
cross the HA boundary, and the dropdown is a convenience rather than the
authority: `queue_param()` re-checks both in the firmware, so a stale list
produces a refusal with a reason and never a bad write. The component owns the
rest — the writable list, each range, the read-modify-write, both image CRCs and
the re-lock. See [katel.yaml](katel.yaml) for the whole block:

```yaml
globals:
  # The parameter number behind the dropdown, parsed when the selection changes
  # rather than read out of the select inside the button's lambda: `.state` is
  # gone from `select::Select` on ESPHome 2025.x, whereas the `x` handed to
  # on_value has been stable for years. 0 is not a writable parameter, so a
  # Queue pressed before anything is picked is refused rather than acting on a
  # default.
  - id: write_param_no
    type: uint8_t
    restore_value: false
    initial_value: '0'

select:
  - platform: template
    name: "Write parameter"
    id: write_param_sel
    optimistic: true
    initial_option: "(nothing selected)"
    on_value:
      - lambda: |-
          // Every option starts "p<N>". Skip the p and take digits until the
          // description begins.
          uint8_t n = 0;
          for (size_t i = 1; i < x.size() && x[i] >= '0' && x[i] <= '9'; i++)
            n = (uint8_t) (n * 10 + (x[i] - '0'));
          id(write_param_no) = n;
    options:
      - "(nothing selected)"
      - "p1 - max CH flow temp (20..90)"
      - "p2 - DHW tank setpoint (40..65)"
      # ...twenty-two in all

button:
  - platform: template
    name: "Boiler queue parameter"
    on_press:
      - lambda: |-
          id(boiler).queue_param(id(write_param_no), (int) id(write_value).state);

  - platform: template
    name: "Boiler write queue to boiler"
    on_press:
      - lambda: 'id(boiler).write_queue();'
```

**Queue several edits and they all ride in one transaction.** A parameter write
rewrites and re-verifies the whole 128-byte image whatever it is changing, so six
queued edits cost exactly what one does — the same thirty-odd frames on the bus
and the same single EEPROM cycle. Writing them one at a time would be six EEPROM
cycles and six chances to provoke the blocking a write can bring on.

Every verdict the write path reaches is published to a `write_result_text`
sensor — refusals, progress and success alike — so the answer arrives in Home
Assistant rather than only in the log. The verdict is a good half minute behind
the press, so it reads `writing p2=55, p33=6...` first and lands on
`write successful, verified by read-back: p2=55, p33=6` after. `write_queue_text`
lists what is staged.

| Method | What it does |
|---|---|
| `queue_param(p, v)` | stage one edit; refuses a parameter that is not writable or a value out of range. `v` is signed — pass `-10`, not the `246` the byte holds |
| `write_queue()` | write every staged edit in one transaction, and empty the queue only if it verifies |
| `clear_param_queue()` | throw the staged edits away |
| `write_param(p, v)` | stage one edit and write it immediately — the three above in one call |
| `test_service_mode()` | unlock, read a sample back to confirm it engaged, re-lock |
| `write_block_unchanged(blk)` | read an EEPROM block and write it back unchanged |

`set_write_enabled(bool)` is a UI gate for a switch in Home Assistant, so a write
cannot be set off by a stray press. It is *not* the safety boundary: that is
`allow_writes`, which is compiled in and unreachable from Home Assistant.

A parameter write is a single transaction that unlocks service mode, re-reads all
eight EEPROM blocks, writes them all back with the staged bytes changed, reads
them back to verify and re-locks — and that re-lock happens whether or not the
write succeeded. It writes the whole block because Recom does.

The transaction opens with a sample, which records in the log what the boiler was doing 
before the write and in case of a blocking code gives a before-and-after to compare.

It also **recomputes the two CRC16s the 128-byte parameter image carries over
itself** — bytes 62–63 and 126–127 — because the PCU stores a set that fails them
and then refuses to adopt it, staying on its old values and raising `Blocking 0`.
That is not in Recom's protocol layer; it was found by putting a sniffer on
Recom's own cable. See [mapping/pcu05_p3_protocol.md](mapping/pcu05_p3_protocol.md),
*What the PCU actually checks*. Before anything is
written back, 58 documented parameters in the freshly read image are checked
against their ranges - each block as it arrives, and the whole image again as the
last gate - so a corrupted read is refused rather than returned to EEPROM. A
transaction's reads go to a buffer of its own and reach the published sensors only
once a write has verified, so a refused write leaves Home Assistant alone. A value
outside the documented range is **refused, not clamped** — a 30 meant for p1 and
typed into p33 writes nothing, rather than quietly writing 15 and reporting
success. A value the boiler already holds is not written at all, because EEPROM
endurance is finite, and a queue where every edit is already satisfied sends no
write frame.

Twenty-two parameters are writable; the table is `PARAM_LIMITS` in
[dietrich.cpp](components/dietrich/dietrich.cpp). Two groups need watching.

**p28 and p29** (pump CH min/max) are stored 2–10 and mean 20–100 %, which is how
Recom and the manual number them. This component does not: the thing being set is
a pump speed in per cent and the `param_pump_ch_*` sensors publish per cent, so
the write path takes per cent too and divides on the way into the byte. Write
`30` to get `30 %`, and the sensor reads back the `30` you wrote. Only whole tens
exist — `45` is refused rather than rounded, because the stored byte cannot hold
it. The one parameter where what you write is *not* what you read is **p61**,
which is in tenths of a degree: type `-5` and `param_control_temp_offset` reads
`-0.5 °C`. It was deliberately not rescaled the way p28/p29 were — the pump bytes
only ever held ten values, so per cent in steps of 10 threw nothing away, whereas
whole degrees here would turn 201 usable settings into 21.

**p73, p85, p86, p88, p105 and p106** — six in all — sit in the image's upper
half, so a write to any of them refreshes the CRC at bytes 126–127 rather than the
one at 62–63. That the upper half is protected the same way is certain, since the
stored CRC checks out on every dump, but no other writer has been observed
maintaining it: Recom's parameter screens stop at p44 on this board and its EEPROM
menu is greyed out.

Five parameters — p27, p30, p61, p86 and p106 — are stored two's complement. The
manual puts it as *“Setting value − 256 = Desired value”* and tabulates 226 = −30,
246 = −10, 255 = −1, which is `int8` by another name, so the conversion each way
is a cast. Pass the value the manual gives. It matters most for **p27**, the cold
end of the heating curve: without it the curve could be moved but not pivoted.

The gas/air settings
(p17-p21, p77, p78) and the controller-protection limits (p55-p57) are refused
outright, because a bad write there is a combustion-safety problem rather than a
comfort one.

The refusal paths are covered by tests that run on a PC against a simulated
board — see [tests/](tests/).

**Status:** the transaction, the frames and the checks are confirmed against a
live PCU-05 P3. Writes to that board were ACKed and appeared to change nothing for
several rounds; reading all eight blocks from both device addresses showed why -
`0x00` and `0x01` hold **different** EEPROM images, only `0x00` holds the
parameters the boiler runs on, and the write path had been aimed at `0x01`, which
accepted and stored the bytes where nothing reads them. The EEPROM path is now
addressed to `0x00` throughout, and `0x00` promptly refused the first write with a
NAK because service mode is per address — so a transaction unlocks and re-locks
both. An ACK is still not proof a value changed, which
is why every write is read back and compared; see
[mapping/pcu05_p3_protocol.md](mapping/pcu05_p3_protocol.md) for what is known
and what is still being pinned down.

### Fields specific to the PCU-05 P3 map

The PCU-05 sample block is a superset of the older Remeha and Avanta maps at
identical byte offsets, and this component decodes it to the P3 map throughout:

- the fields only the PCU-05 P3 map defines — `fan_speed_rpm` (the real rpm
  reading at data offset 44, distinct from the airflow pair that
  `fan_speed`/`fan_speed_setpoint` expose), `su_state`, `su_locking`,
  `su_blocking`, `ch_timer_enable`, `dhw_timer_enable`, `solar_temp`,
  `hmi_active`, `ch_setpoint_hmi`, `dhw_setpoint_hmi`, `service_mode` and
  `rs232_mode`,
- `input_bit0` (shutdown input) and `input_bit1` (release input) are inverted,
  which the PCU-05 P3 map marks `invert="true"` while the older Avanta maps do
  not,
- `hydro_pressure` is available but almost certainly dead. The PCU-05 P3 map has
  no analog pressure field (Recom ships it commented out) and no pressure-sensor
  parameter to enable one. This board reads water pressure as a switch, which
  surfaces as `Min. water pressure(Blocking 14)` on `blocking_text` — watch that
  rather than a bar reading.

### Status text sensors

`state`, `sub_state`, `lockout` and `blocking` are numeric codes. Each now has a
matching `*_text` sensor - `state_text`, `sub_state_text`, `lockout_text`,
`blocking_text` - that publishes the same wording Recom shows, replacing the
template-lambda ladders the older examples used:

```yaml
dietrich:
  state_text:
    name: "Boiler state"        # e.g. "3:Burning CH"
  lockout_text:
    name: "Boiler lockout"      # e.g. "No locking"
```

The code tables are generated from `mapping/pcu05_p3_datamap.json`.

### Bit inversion fix

Every Recom map marks two sample bits `invert="true"`, which this component
previously published raw:

- `valve_bit0` (gas valve)
- `demand_source_bit4` (DHW eco)

Both are now inverted, so those two sensors report the opposite of what earlier
versions did. 

### ESP32 notes

The component works on ESP32 (verified with an ESP32 DevKit-C V4 /
`variant: esp32`).

Put the boiler bus on free pins — `tx_pin: GPIO18`, `rx_pin: GPIO19` is what this
board runs. ESPHome routes whichever pins you name through the ESP32's UART
matrix, so there is no need to pick a particular hardware UART or to stay on its
nominal pins. **Avoid GPIO16 and GPIO17**: they are the obvious-looking choice,
and they are wired to the PSRAM on WROVER modules, so a config that works on one
DevKit will fail on another that looks identical.

The component source lives in [components/dietrich](components/dietrich). Compared to the
legacy custom component it also validates every response frame with CRC16
before publishing any values.

Reads are non-blocking: the request/response exchange runs as a small state machine in
`loop()` rather than with `delay()` calls, so the main loop is never stalled waiting on the
bus and every configured sensor is published on every poll.

## Hardware

It connects to the boiler using a 4P4C (RJ10) connector with the following pinouts:
```
 Heater Board from top       ESP8266 / ESP32
    4P4C RJ connector
    
       +---------+
GND 4  ---       +--+        GND
TXD 3  ---          |        RX
RXD 2  ---          |        TX
5V  1  ---       +--+        5V
       +---------+
```

I have connected ESP to prototype board with pins. 
There is a simple voltage divider (1kΩ / 1.8kΩ) to change RX from 5V to 3V.

Screenshots from original repo.

![Screenshot](board.jpg)

And in printed box.

![Screenshot](box.jpg)

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
GPL-3.0 is used to ensure any derivative work remains open source.

## Protocol mapping

[mapping/](mapping/) holds the Recom configuration files the decode is derived from:
`PCU-05_P3.xml` for this board, `language.xml` for the string table, and
`DeviceConfiguration.xml` for the boiler-code to protocol mapping. `AvantaV1_P5.xml`
is kept alongside them because reading a field against the same field in a
neighbouring map is often what settles what the field is.
`mapping/pcu05_p3_fieldmap.md` is the resolved, human-readable field map for the
PCU-05 P3 and `mapping/pcu05_p3_datamap.json` the machine-readable form used to
generate the code tables.

Alongside them, from an actual board rather than from Recom:
[`mapping/pcu05_p3_protocol.md`](mapping/pcu05_p3_protocol.md) for the wire protocol
and the write path, [`mapping/pcu05_p3_eeprom_map.md`](mapping/pcu05_p3_eeprom_map.md)
for the full 2 KB EEPROM of both device addresses — parameters, counters, appliance
identification and the blocking and locking history rings — and
[`mapping/pcu05_p3_live_parameters.md`](mapping/pcu05_p3_live_parameters.md) for one
appliance's commissioned parameter image.

## Credits

Thanks to great work from https://github.com/rjblake/remeha 

The rewrite of the original custom component into a native ESPHome external component
(the C++ and Python code in [components/dietrich](components/dietrich), including CRC16
frame validation), and the subsequent PCU-05 P3 work — the parameter map, the EEPROM
transactions and the write path — were done with the help of Claude (Anthropic), based on
the original protocol logic in this repository and on Recom's own data maps.
