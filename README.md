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
  # variant: calenta_v1_p5   # for Calenta / MCX Plus / Avanta V1_P5 (default: mcr3)
  flow_temp:
    name: "Boiler flow temp"
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

### ESP32 notes

The component works on ESP32 as well (verified with an ESP32 DevKit V4 /
`az-delivery-devkit-v4`), but do **not** copy the ESP8266 UART/logger settings:

- keep the `logger:` on its default UART0/USB console — `hardware_uart: UART1`
  would map to GPIO9/GPIO10, which are wired to the internal SPI flash on classic
  ESP32 modules and crash the board in a boot loop (this was the cause of issue #7),
- put the boiler bus on free pins, e.g. UART2: `tx_pin: GPIO17`, `rx_pin: GPIO16`.

The component source lives in [components/dietrich](components/dietrich). Compared to the
legacy custom component it also validates every response frame with CRC16 (`mcr3` variant)
before publishing any values.

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

## Credits

Thanks to great work from https://github.com/rjblake/remeha - for creating maping of data in excel file.

The rewrite of the original custom component into a native ESPHome external component
(the C++ and Python code in [components/dietrich](components/dietrich), including CRC16
frame validation and the `calenta_v1_p5` variant support) was done with the help of
Claude (Anthropic), based on the original protocol logic in this repository.
