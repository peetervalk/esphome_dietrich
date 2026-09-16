# Dietrich (Remeha) Boiler connectivity using ESP8266 / ESP32 with ESPHOME

Native ESPHome **external component** for reading data from De Dietrich (or Remeha) PC interface, tested with model mcr3.
Works on both **ESP8266** (e.g. Wemos D1) and **ESP32** (e.g. DevKit V4) boards with ESPHOME software - sample YAML files are in English and Polish.

## Usage

Since ESPHome 2025.2 the old `platform: custom` + `includes:` mechanism is removed, so this
project is now a proper external component. Add it to your YAML:

```yaml
external_components:
  - source: github://kakaki/esphome_dietrich
    components: [ dietrich ]

uart:
  id: uart_bus
  baud_rate: 9600
  tx_pin: GPIO1
  rx_pin: GPIO3

dietrich:
  uart_id: uart_bus
  update_interval: 15s
  # variant: calenta_v1_p5   # Calenta / MCX Plus / Avanta V1_P5
  # variant: pcu05_p3        # PCU-05 control board, parameter set P3
  flow_temp:
    name: "Boiler flow temp"
  state_text:
    name: "Boiler state"     # decoded status text, no lambda needed
  # ... see the example YAML files for the full sensor list
```

Full examples:

| File | Board | Language | Protocol variant |
|---|---|---|---|
| [dietrich_en.yaml](dietrich_en.yaml) | ESP8266 | English | `mcr3` (default) |
| [dietrich_pl.yaml](dietrich_pl.yaml) | ESP8266 | Polish | `mcr3` (default) |
| [dietrich_calenta_v1_p5_en.yaml](dietrich_calenta_v1_p5_en.yaml) | ESP8266 | English | `calenta_v1_p5` |
| [dietrich_calenta_v1_p5_pl.yaml](dietrich_calenta_v1_p5_pl.yaml) | ESP8266 | Polish | `calenta_v1_p5` |
| [dietrich_esp32_en.yaml](dietrich_esp32_en.yaml) | ESP32 | English | `mcr3` (default) |
| [dietrich_pcu05_p3_en.yaml](dietrich_pcu05_p3_en.yaml) | ESP32 | English | `pcu05_p3` |

### Protocol variants

Recom's own `DeviceConfiguration.xml` groups these boards into several wire
protocols. This component implements two of them, across three variants:

| Variant | Protocol | Frame | Boards |
|---|---|---|---|
| `mcr3` (default) | Remeha (`protocol.nr` 1) | CRC16, 7-byte response header | MCR3, PCU-0x |
| `pcu05_p3` | Remeha (`protocol.nr` 1) | same frames as `mcr3` | PCU-05, parameter set P3 |

### Writing parameters (`pcu05_p3` only)

The component can also write the boiler's stored parameters back to EEPROM. It
stays off unless you ask for it:

```yaml
dietrich:
  id: boiler
  variant: pcu05_p3
  allow_writes: true
```

There is no writable entity yet. The write path is reached from a YAML lambda,
so Home Assistant only ever sees an ordinary button and sends “press” — no
frame, byte or parameter value crosses the HA boundary, and the component builds
and validates everything itself:

```yaml
button:
  - platform: template
    name: "Boiler set DHW hysteresis to 6"
    on_press:
      - lambda: 'id(boiler).write_param(33, 6);'
```

`write_param(p, v)` takes the `pNN` number from the parameter table in
[mapping/pcu05_p3_protocol.md](mapping/pcu05_p3_protocol.md). Two further entry
points exist for bringing this up on a boiler for the first time, and
[dietrich_pcu05_p3_en.yaml](dietrich_pcu05_p3_en.yaml) carries all three ready
to uncomment, in the order they are worth trying:

| Method | What it does |
|---|---|
| `test_service_mode()` | unlock service mode and re-lock it, writing nothing |
| `write_block_unchanged(blk)` | read an EEPROM block and write it back unchanged |
| `write_param(p, v)` | read-modify-write one parameter |

Each write is a single transaction that unlocks service mode, re-reads the block
it is about to modify, writes it, reads it back to verify and re-locks — and
that re-lock happens whether or not the write succeeded. Values are clamped to
the documented range; a value the boiler already holds is not written at all,
because EEPROM endurance is finite; and the gas/air settings (p17-p21, p77, p78)
and the controller-protection limits (p55-p57) are refused outright, because a
bad write there is a combustion-safety problem rather than a comfort one.

The refusal paths are covered by tests that run on a PC against a simulated
board — see [tests/](tests/).
| `calenta_v1_p5` | Avanta (`protocol.nr` 2) | XOR checksum, 6-byte header | Calenta, MCX Plus, Avanta V1_P5 |

`pcu05_p3` sends the same requests as `mcr3` - the PCU-05 sample block is a
superset of every Avanta/Calenta map at identical byte offsets. What the variant
changes is the decode:

- adds the fields only the PCU-05 P3 map defines: `fan_speed_rpm` (the real rpm
  reading at data offset 44, distinct from the airflow pair that
  `fan_speed`/`fan_speed_setpoint` expose), `su_state`, `su_locking`,
  `su_blocking`, `ch_timer_enable`, `dhw_timer_enable`, `solar_temp`,
  `hmi_active`, `ch_setpoint_hmi`, `dhw_setpoint_hmi`, `service_mode` and
  `rs232_mode`,
- inverts `input_bit0` (shutdown input) and `input_bit1` (release input), which
  the PCU-05 P3 map marks `invert="true"` while the Avanta maps do not,
- leaves `hydro_pressure` available but almost certainly dead. The PCU-05 P3 map
  has no analog pressure field (Recom ships it commented out) and no
  pressure-sensor parameter to enable one. This board reads water pressure as a
  switch, which surfaces as `Min. water pressure(Blocking 14)` on
  `blocking_text` - watch that rather than a bar reading.

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

Every Recom map - Avanta and PCU-05 alike - marks two sample bits
`invert="true"`, which this component previously published raw:

- `valve_bit0` (gas valve)
- `demand_source_bit4` (DHW eco)

Both are now inverted for **all** variants, so those two sensors report the
opposite of what earlier versions did. If you built automations or template
sensors that compensated for the old behaviour, drop the compensation.

### ESP32 notes

The component works on ESP32 as well (verified with an ESP32 DevKit V4 /
`az-delivery-devkit-v4`), but do **not** copy the ESP8266 UART/logger settings:

- keep the `logger:` on its default UART0/USB console — `hardware_uart: UART1`
  would map to GPIO9/GPIO10, which are wired to the internal SPI flash on classic
  ESP32 modules and crash the board in a boot loop (this was the cause of issue #7),
- put the boiler bus on free pins, e.g. UART2: `tx_pin: GPIO17`, `rx_pin: GPIO16`.

The component source lives in [components/dietrich](components/dietrich). Compared to the
legacy custom component it also validates every response frame with CRC16 (Remeha variants)
before publishing any values.

Reads are non-blocking: the request/response exchange runs as a small state machine in
`loop()` rather than with `delay()` calls, so the main loop is never stalled waiting on the
bus and every configured sensor is published on every poll.

The legacy header files `dietrich.h` and `dietrich_calentaV1_P5.h` are kept for users of
ESPHome ≤ 2025.1 with the old `platform: custom` mechanism.

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

I have connected ESP to prototype board with pins, on board there was simple voltage divider (1kΩ / 2kΩ) to change power from 5V to 3V, but on my board it was not needed and i have connected pins directly to Dietich MCR33, .
Cable is simple phone cord, cut and added pin connector to it.

Screenshot of board connected to boiler.

![Screenshot](board.jpg)

And in printed box.

![Screenshot](box.jpg)

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
GPL-3.0 is used to ensure any derivative work remains open source.

## Protocol mapping

[mapping/](mapping/) holds the Recom configuration files the decode is derived from -
one XML per boiler and parameter set, `language.xml` for the string table, and
`DeviceConfiguration.xml` for the boiler-code to protocol mapping.
`mapping/pcu05_p3_fieldmap.md` is the resolved, human-readable field map for the
PCU-05 P3 and `mapping/pcu05_p3_datamap.json` the machine-readable form used to
generate the code tables.

## Credits

Thanks to great work from https://github.com/rjblake/remeha - for creating maping of data in excel file.

The rewrite of the original custom component into a native ESPHome external component
(the C++ and Python code in [components/dietrich](components/dietrich), including CRC16
frame validation and the `calenta_v1_p5` variant support) was done with the help of
Claude (Anthropic), based on the original protocol logic in this repository.
