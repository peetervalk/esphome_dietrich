# Remeha PCU-05 (P3) — resolved field map

Generated from `PCU-05_P3.xml` (config version 0.2, EEPROM 2048 bytes) joined against the Recom English language file (v7.30).

## Expression syntax

- `A` is the byte at the listed offset; `B` the next one.
- `A.0 + B.1` means **little-endian**: `A + (B << 8)`.
- `(A.1 + B.0)` means **big-endian**: `(A << 8) + B` — used by the counter block.
- `sgn(...)` reinterprets the 16-bit result as signed two's complement.
- `x 0.01` etc. is the scaling factor applied afterwards.
- `bit` rows test a single bit of the byte: offset `41.3` is bit 3 of byte 41.
- `!A` marks a bit the XML flags `invert="true"`: the raw bit is negated before
  the enum lookup, so a raw 1 reads as the enum's value 0.

## Sample block (live data) — `sample` (62 entries)

> Offset `49` is **Water pressure** (`A x 0.1`, bar, range 0 … 10). The PCU-05 P3
> XML ships it commented out, so it is absent from the table below and from
> `pcu05_p3_datamap.json`. Every Avanta map omits it from `sample` as well.

| Offset | Name | Unit | Range | UL | Expression | Enum |
|---|---|---|---|---|---|---|
| `0` | Flow temp. | °C | -25 … 150 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `2` | Return temp. | °C | -25 … 150 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `4` | DHW-in temp. | °C | -25 … 150 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `6` | Outside temp. | °C | -60 … 60 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `8` | Calorifier temp. | °C | -25 … 150 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `12` | Boiler Control temp. | °C | -25 … 125 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `14` | Room temp. | °C | -25 … 125 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `16` | CH Setpoint | °C | 0 … 100 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `18` | DHW Setpoint | °C | 0 … 100 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `20` | Room temp setpoint | °C | 0 … 100 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `22` | Airflow set |  | 0 … 10000 |  | `A.0 + B.1` |  |
| `24` | Airflow |  | 0 … 10000 |  | `A.0 + B.1` |  |
| `26` | Ionisation current | uA | 0 … 13 |  | `A x 0.1` |  |
| `27` | Internal setpoint | °C | 0 … 100 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `29` | Available power | % | 0 … 100 |  | `A` |  |
| `30` | Pump | % | 0 … 100 |  | `A` |  |
| `32` | Required output | % | 0 … 100 |  | `A` |  |
| `33` | Actual power | % | 0 … 100 |  | `A` |  |
| `36.0` | Mod. controller |  |  |  | `A` | no.yes.code |
| `36.1` | Mod. heat demand |  |  |  | `A` | no.yes.code |
| `36.2` | On/off heat demand |  |  |  | `A` | no.yes.code |
| `36.3` | Frost protection |  |  |  | `A` | no.yes.code |
| `36.4` | DHW eco |  |  |  | `!A` | no.yes.code |
| `36.5` | DHW blocking |  |  |  | `A` | no.yes.code |
| `36.6` | Anti Legionella |  |  |  | `A` | no.yes.code |
| `36.7` | DHW heat demand |  |  |  | `A` | no.yes.code |
| `37.0` | Shut down input |  |  |  | `!A` | open.closed.code |
| `37.1` | Release input |  |  |  | `!A` | open.closed.code |
| `37.2` | Ionisation |  |  |  | `A` | no.yes.code |
| `37.3` | Flow switch |  |  |  | `A` | open.closed.code |
| `37.5` | Min gas pressure |  |  |  | `A` | open.closed.code |
| `37.6` | CH enable |  |  |  | `A` | no.yes.code |
| `37.7` | DHW enable |  |  |  | `A` | no.yes.code |
| `38.0` | Gas valve |  |  |  | `!A` | closed.open.code |
| `38.2` | Ignition |  |  |  | `A` | off.on.code |
| `38.3` | Three way valve |  |  |  | `A` | inline |
| `38.4` | Ext. 3-way valve |  |  |  | `A` | open.closed.code |
| `38.6` | External gas valve |  |  |  | `A` | open.closed.code |
| `39.0` | Pump |  |  |  | `A` | off.on.code |
| `39.1` | Calorifier pump |  |  |  | `A` | open.closed.code |
| `39.2` | Ext. CH pump |  |  |  | `A` | off.on.code |
| `39.4` | Status report |  |  |  | `A` | open.closed.code |
| `39.7` | OT Smart power |  |  |  | `A` | off.on.code |
| `40` | STATUS |  |  |  | `A` | status.code |
| `41` | LOCKING |  |  |  | `A` | failure.code |
| `42` | BLOCKING |  |  |  | `A` | error.code |
| `43` | SUBSTATUS |  |  |  | `A` | substatus.code |
| `44` | Fan speed | Rpm | 0 … 10000 | 4 | `A.0 + B.1` |  |
| `46` | SU state |  | 0 … 11 | 4 | `A` |  |
| `47` | SU locking |  | 0 … 17 | 4 | `A` |  |
| `48` | SU blocking |  | 0 … 5 | 4 | `A` |  |
| `50.1` | HRU active |  |  |  | `A` | open.closed.code |
| `50.6` | CH Timer enable |  |  |  | `A` | no.yes.code |
| `50.7` | DHW Timer enable |  |  |  | `A` | no.yes.code |
| `51` | Control temp. | °C | 0 … 100 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `53` | DHW Flow | l/min | 0 … 25 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `56` | Solar. Temp. | °C | -25 … 150 |  | `sgn(A.0 + B.1) x 0.01` |  |
| `58` | HMI active |  | 0 … 1024 | 4 | `A.0 + B.1` |  |
| `60` | CH setpoint HMI | °C |  … 100 |  | `sgn(A)` |  |
| `61` | DHW setpoint HMI | °C |  … 100 |  | `sgn(A)` |  |
| `62` | Service mode |  | 0 … 255 |  | `A` |  |
| `63` | RS232 mode |  | 0 … 255 |  | `A` |  |

## Parameters (writable) — `parameter` (98 entries)

| Code | Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|---|
| 1 | `0` | T flow set point | °C | 20 … 90 | `A` |  |
| 2 | `1` | DHW set point | °C | 40 … 65 | `A` |  |
| 3 | `2` | Boiler controls |  |  | `A` | inline |
| 4 | `3` | Comfort DHW |  |  | `A` | inline |
| 5 | `4` | Pump post run CH | Min | 0 … 99 | `A` |  |
| 17 | `16` | Full load HTG |  | 1000 … 10000 | `A x 100` |  |
| 18 | `17` | Full load DHW |  | 1000 … 10000 | `A x 100` |  |
| 19 | `18` | Partload HTG/DHW |  | 1000 … 5000 | `A x 100` |  |
| 20 | `19` | Offset partload |  | 0 … 99 | `A` |  |
| 21 | `20` | Start load |  | 1000 … 5000 | `A x 100` |  |
| 23 | `22` | Max Flow system | °C | 20 … 90 | `A` |  |
| 24 | `23` | Factor avg flow |  | 1 … 255 | `A` |  |
| 25 | `24` | Footpoint T outside | °C | 0 … 30 | `A` |  |
| 26 | `25` | Footpoint Tflow | °C | 0 … 90 | `A` |  |
| 27 | `26` | Clima p outside temp | °C | -30 … 0 | `sgn(A)` |  |
| 28 | `27` | Pump CH min | *10% | 2 … 10 | `A` |  |
| 29 | `28` | Pump CH max | *10% | 2 … 10 | `A` |  |
| 30 | `29` | Temp frostprot. | °C | -30 … 0 | `sgn(A)` |  |
| 31 | `30` | Anti Legionella |  |  | `A` | inline |
| 32 | `31` | Setpoint raise DHW | °C | 0 … 25 | `A` |  |
| 33 | `32` | Hystereses calorif. | °C | 2 … 15 | `A` |  |
| 34 | `33` | Ext. 3-way valve standby |  |  | `A` | inline |
| 35 | `34` | Boiler type: |  |  | `A` | inline |
| 36 | `35` | Blocking input |  |  | `A` | inline |
| 37 | `36` | Min. gas pressure |  |  | `A` | inline |
| 38 | `37` | HRU active |  |  | `A` | inline |
| 39 | `38` | Fluegas valve time | sec | 0 … 255 | `A` |  |
| 40 | `39` | Status report |  |  | `A` | inline |
| 41 | `40` | Service notification |  |  | `A` | inline |
| 42 | `41` | Service hours | x100h | 1 … 255 | `A` |  |
| 43 | `42` | Service burning | x100h | 1 … 255 | `A` |  |
| 44 | `43` | De-airation cycle |  |  | `A` | inline |
| 55 | `54` | Stop contr. cooling | °C | 20 … 100 | `A` |  |
| 56 | `55` | Start contr cooling | °C | 20 … 100 | `A` |  |
| 57 | `56` | Max controller temp | °C | 20 … 100 | `A` |  |
| 58 | `57` | Gas air type |  |  | `A` | inline |
| 59 | `58` | DHW-in gradient | .01C/s | 1 … 200 | `A` |  |
| 60 | `59` | dT pomp offset | °C | 0 … 100 | `A` |  |
| 61 | `60` | Offset control temp | /10°C | -100 … 100 | `sgn(A)` |  |
| 62 | `61` | DHW Flow at RPM min | l/min | 0 … 5 | `A x 0.1` |  |
| 66 | `65` | Gradiënt dTMax 1 | .01C/s | 1 … 200 | `A` |  |
| 67 | `66` | Gradiënt dTMax 2 | .01C/s | 1 … 200 | `A` |  |
| 68 | `67` | Gradiënt dTMax 3 | .01C/s | 1 … 200 | `A` |  |
| 69 | `68` | dT(Flow,Return) | °C | 0 … 60 | `A` |  |
| 70 | `69` | Startpoint modul. | °C | 10 … 40 | `A` |  |
| 71 | `70` | Pump dTset CH | °C | 0 … 40 | `A` |  |
| 72 | `71` | Pump CH start | % | 0 … 100 | `A` |  |
| 73 | `72` | Hysterese CH | °C | 1 … 10 | `A` |  |
| 74 | `73` | Stabilization time | sec | 10 … 180 | `A` |  |
| 75 | `74` | Min. burner  off | Min | 1 … 15 | `A` |  |
| 76 | `75` | Max. burner off | Min | 3 … 15 | `A` |  |
| 77 | `76` | Max. fanspeed CH |  | 1000 … 10000 | `A x 100` |  |
| 78 | `77` | Max. fanspeed DHW |  | 1000 … 10000 | `A x 100` |  |
| 79 | `78` | Pump dTset DHW | °C | 5 … 40 | `A` |  |
| 80 | `79` | Pump DHW min | % | 0 … 100 | `A` |  |
| 81 | `80` | Pump DHW max | % | 0 … 100 | `A` |  |
| 82 | `81` | Pump DHW start | % | 0 … 100 | `A` |  |
| 83 | `82` | Warm up interval CH | Min | 0 … 255 | `A` |  |
| 84 | `83` | Warm up interval | Min | 0 … 255 | `A` |  |
| 85 | `84` | Hysterese warming up | °C | 0 … 20 | `A` |  |
| 86 | `85` | Offset warming up | °C | -30 … 20 | `sgn(A)` |  |
| 87 | `86` | DHW start raise | .1C/l/m | 0 … 100 | `A` |  |
| 88 | `87` | Hystereses DHW | °C | 1 … 10 | `A` |  |
| 89 | `88` | Offset DHW | °C | 0 … 20 | `A` |  |
| 90 | `89` | Offset pl heat exch | °C | 0 … 20 | `A` |  |
| 91 | `90` | %Tf/Tr DHW pump 20% | % | 0 … 100 | `A` |  |
| 92 | `91` | %Tf/Tr DHW pump 100% | % | 0 … 100 | `A` |  |
| 93 | `92` | Delay pump DHW | /10 sec | 0 … 100 | `A` |  |
| 94 | `93` | Post pump time DHW | sec | 1 … 99 | `A` |  |
| 95 | `94` | K-Factor DHW | /sec | 1 … 255 | `A` |  |
| 96 | `95` | Min DHW flow | l/min | 0 … 10 | `A x 0.1` |  |
| 97 | `96` | DHW sensor |  |  | `A` | inline |
| 98 | `97` | Flowdetection time |  | 0 … 255 | `A` |  |
| 99 | `98` | DHW stabilisation | /10 sec | 0 … 255 | `A` |  |
| 100 | `99` | DHW gradiënt | .01C/s | 1 … 200 | `A` |  |
| 101 | `100` | DHW Booster off | °C | 0 … 99 | `A` |  |
| 102 | `101` | P DHW start |  | 0 … 200 | `A` |  |
| 103 | `102` | P DHW FFWD |  | 0 … 200 | `A` |  |
| 104 | `103` | P DHW flowchanges |  | 0 … 200 | `A` |  |
| 105 | `104` | Offset calorifier | °C | 0 … 10 | `A` |  |
| 106 | `105` | Start DHW pump | °C | -20 … 20 | `sgn(A)` |  |
| 107 | `106` | Post pump time DHW | sec | 1 … 255 | `A` |  |
| 108 | `107` | Prepurge time | sec | 0 … 255 | `A` |  |
| 109 | `108` | Postpurge time | sec | 0 … 255 | `A` |  |
| 110 | `109` | Max. Flow temp. | °C | 0 … 110 | `A` |  |
| 112 | `111` | P factor fan |  | 0 … 100 | `A` |  |
| 113 | `112` | I factor fan |  | 1 … 200 | `A` |  |
| 114 | `113` | P factor CH |  | 0 … 100 | `A` |  |
| 115 | `114` | I factor CH |  | 1 … 200 | `A` |  |
| 116 | `115` | P factor CH down |  | 0 … 100 | `A` |  |
| 117 | `116` | I factor CH down |  | 1 … 200 | `A` |  |
| 118 | `117` | P factor DHW |  | 0 … 100 | `A` |  |
| 119 | `118` | I factor DHW |  | 1 … 200 | `A` |  |
| 120 | `119` | I factor pump CH |  | 1 … 200 | `A` |  |
| 121 | `120` | I factor pump DHW |  | 1 … 200 | `A` |  |
| 122 | `121` | RPM at 0KW |  | 0 … 255 | `A` |  |
| 123 | `122` | KW at 10000 RPM |  | 0 … 255 | `A` |  |
| 124 | `123` | Power rating |  | 0 … 255 | `A` |  |

## Counters — `counter` (11 entries)

| Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|
| `0` | Hours run pump CH+DHW |  |  | `(A.1 + B.0) x 2` |  |
| `2` | Hours run 3-way valve DHW |  |  | `(A.1 + B.0) x 2` |  |
| `4` | Hours run CH+DHW |  |  | `(A.1 + B.0) x 2` |  |
| `6` | Hours run DHW |  |  | `(A.1 + B.0) x 1` |  |
| `8` | Power supply available hrs |  |  | `(A.1 + B.0) x 2` |  |
| `10` | Pump starts CH+DHW |  |  | `(A.1 + B.0) x 8` |  |
| `12` | Number of 3way valve cycles  |  |  | `(A.1 + B.0) x 8` |  |
| `14` | Burner starts DHW |  |  | `(A.1 + B.0) x 8` |  |
| `16` | Total Burner starts CH+DHW |  |  | `(A.1 + B.0) x 8` |  |
| `18` | Failed burner starts |  |  | `(A.1 + B.0) x 1` |  |
| `20` | Number of flame loss |  |  | `(A.1 + B.0) x 1` |  |

## Identification block — `identification` (49 entries)

| Offset | Name | Unit | Range | UL | Expression | Enum |
|---|---|---|---|---|---|---|
| `0` | Device type |  |  |  | `A` |  |
| `0` | Device type |  |  |  | `A` |  |
| `0` | Device type |  |  |  | `A` |  |
| `1` | DF |  |  |  | `A` |  |
| `1` | Software version |  |  |  | 6 bytes, base None |  |
| `1` | Software version |  |  |  | 6 bytes, base None |  |
| `1` | Software version |  |  |  | 6 bytes, base None |  |
| `2` | DU |  |  |  | `A` |  |
| `2` | Parameter version |  |  |  | 6 bytes, base None |  |
| `2` | Parameter version |  |  |  | 6 bytes, base None |  |
| `3` | Parameter type |  |  |  | `A` |  |
| `3` | Parameter type |  |  |  | `A` |  |
| `4` | Operating hours | Hrs |  |  | `(A.1 + B.0) x 2` |  |
| `4` | Operating hours | Hrs |  |  | `(A.1 + B.0) x 8` |  |
| `4` | Operating hours | Hrs |  |  | `(A.1 + B.0) x 8` |  |
| `5` | SW_VERSION |  |  |  | 6 bytes, base None |  |
| `6` | PARAM_VERSION |  |  |  | 6 bytes, base None |  |
| `6` | Connected SU type |  |  |  | `A` |  |
| `6` | Connected PCU type |  |  |  | `A` |  |
| `7` | PARAM_TYPE |  |  |  | `A` |  |
| `7` | Connected PSU type |  |  |  | `A` |  |
| `7` | Connected PSU type |  |  |  | `A` |  |
| `8` | Last blocking code |  |  |  | `A` |  |
| `8` | Last blocking code |  |  |  | `A` |  |
| `9` | Last locking code |  |  |  | `A` |  |
| `9` | Last locking code |  |  |  | `A` |  |
| `10` | Next Service code |  |  |  | `A` | service.counter |
| `10` | Last internal error |  |  |  | `A` |  |
| `11` | Serial number |  |  |  | 5 bytes, base None |  |
| `11` | Serial number |  |  |  | 5 bytes, base None |  |
| `11` | Serial number |  |  |  | 5 bytes, base None |  |
| `16` | Connected PSU type |  |  | 5 | `A` |  |
| `17` | Connected PCU type |  |  | 5 | `A` |  |
| `18` | SCU-C |  |  | 5 | `A` |  |
| `19` | SU no. |  |  | 5 | `A` |  |
| `20` | SU no. |  |  | 5 | `A` |  |
| `21` | SU no. |  |  | 5 | `A` |  |
| `22` | SU no. |  |  | 5 | `A` |  |
| `23` | SU no. |  |  | 5 | `A` |  |
| `24` | SCU-S no. |  |  | 5 | `A` |  |
| `25` | SCU-S no. |  |  | 5 | `A` |  |
| `26` | SCU-S no. |  |  | 5 | `A` |  |
| `27` | SCU-S no. |  |  | 5 | `A` |  |
| `28` | SCU-S no. |  |  | 5 | `A` |  |
| `29` | SCU-S no. |  |  | 5 | `A` |  |
| `30` | SCU-S no. |  |  | 5 | `A` |  |
| `31` | SCU-S no. |  |  | 5 | `A` |  |
| `32` | Serial number |  |  |  | 4 bytes, base 16 |  |
| `48` | Boiler name |  |  |  | 4 bytes, base 16 |  |

## Status block — `status` (4 entries)

| Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|
| `40` | STATUS |  |  | `A` | status.code |
| `41` | LOCKING |  |  | `A` | failure.code |
| `42` | BLOCKING |  |  | `A` | error.code |
| `43` | SUBSTATUS |  |  | `A` | substatus.code |

## Failure (locking) history record — `failure` (13 entries)

| Code | Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|---|
| E | `0` | Error code |  |  | `A` | failure.code |
| n | `1` | Number |  |  | `A` |  |
| Hr | `2` | Operating hours | Hrs |  | `(A.1 + B.0) x 2` |  |
| St | `4` | State |  |  | `A` | status.code |
| Su | `5` | Sub-State |  |  | `A` | substatus.code |
| T1 | `6` | Flow temp. | °C | -25 … 126 | `sgn(A)` |  |
| T2 | `7` | Return temp. | °C | -25 … 126 | `sgn(A)` |  |
| T3 | `8` | Calorifier temp. | °C | -25 … 126 | `sgn(A)` |  |
| T4 | `9` | Outside temp. | °C | -60 … 60 | `sgn(A)` |  |
| Sp | `10` | Internal setpoint | °C | 0 … 100 | `sgn(A)` |  |
| FL | `11` | Ionisation current | uA |  | `A x 0.1` |  |
| nF | `12` | Airflow |  |  | `A.1 + B.0` |  |
| Po | `15` | Actual power | % |  | `A` |  |

## Error (blocking) history record — `error` (13 entries)

| Code | Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|---|
| b | `0` | Error code |  |  | `A` | error.code |
| n | `1` | Number |  |  | `A` |  |
| Hr | `2` | Operating hours | Hrs |  | `(A.1 + B.0) x 2` |  |
| St | `4` | State |  |  | `A` | status.code |
| Su | `5` | Sub-State |  |  | `A` | substatus.code |
| T1 | `6` | Flow temp. | °C | -25 … 126 | `sgn(A)` |  |
| T2 | `7` | Return temp. | °C | -25 … 126 | `sgn(A)` |  |
| T3 | `8` | Calorifier temp. | °C | -25 … 126 | `sgn(A)` |  |
| T4 | `9` | Outside temp. | °C | -60 … 60 | `sgn(A)` |  |
| Sp | `10` | Internal setpoint | °C | 0 … 100 | `sgn(A)` |  |
| FL | `11` | Ionisation current | uA |  | `A x 0.1` |  |
| nF | `12` | Airflow |  |  | `A.1 + B.0` |  |
| Po | `15` | Actual power | % |  | `A` |  |

## dF / dU — `df.du` (2 entries)

| Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|
| `0` | DF |  |  | `A` |  |
| `1` | DU |  |  | `A` |  |

## Service code — `service.code` (2 entries)

| Offset | Name | Unit | Range | Expression | Enum |
|---|---|---|---|---|---|
| `0` | Cancel possible |  |  | `A` | no.yes.code |
| `1` | SERVICE_CODE |  |  | `A` | service.code |

## Code tables

### `service.counter`

| Value | Meaning |
|---|---|
| 0 | A |
| 1 | b |
| 2 | A |
| 3 | C |

### `service.code`

| Value | Meaning |
|---|---|
| 0 | General |
| 1 | A |
| 2 | b |
| 3 | C |

### `status.code`

| Value | Meaning |
|---|---|
| 0 | 0:Standby |
| 1 | 1:Boiler start |
| 2 | 2:Burner start |
| 3 | 3:Burning CH |
| 4 | 4:Burning DHW |
| 5 | 5:Burner stop |
| 6 | 6:Boiler stop |
| 7 | 7:- |
| 8 | 8:Controlled stop |
| 9 | 9:Blocking mode |
| 10 | 10:Locking mode |
| 11 | 11:Chimney mode L |
| 12 | 12:Chimney mode h |
| 13 | 13:Chimney mode H |
| 14 | 14:- |
| 15 | 15:Manual-heatdemand |
| 16 | 16:Boiler-frost-protection |
| 17 | 17:De-airation |
| 18 | 18:Controller temp protection |
| 999 | Unknown State |

### `substatus.code`

| Value | Meaning |
|---|---|
| 0 | 0:Standby |
| 1 | 1:Anti-cycling |
| 2 | 2:Open hydraulic valve |
| 3 | 3:Pump start |
| 4 | 4:Wait for burner start |
| 10 | 10:Open external gas valve |
| 11 | 11:Fan to fluegasvalve speed |
| 12 | 12:Open fluegasvalve |
| 13 | 13:Pre-purge |
| 14 | 14:Wait for release |
| 15 | 15:Burner start |
| 16 | 16:VPS test |
| 17 | 17:Pre-ignition |
| 18 | 18:Ignition |
| 19 | 19:Flame check |
| 20 | 20:Interpurge |
| 30 | 30:Normal internal setpoint |
| 31 | 31:Limited internal setpoint |
| 32 | 32:Normal power control |
| 33 | 33:Gradient control level 1 |
| 34 | 34:Gradient control level 2 |
| 35 | 35:Gradient control level 3 |
| 36 | 36:Flame protection |
| 37 | 37:Stabilization time |
| 38 | 38:Cold start |
| 39 | 39:Limited power Tfg  |
| 40 | 40:Burner stop |
| 41 | 41:Post purge |
| 42 | 42:Fan to fluegasvalve speed |
| 43 | 43:Close fluegasvalve |
| 44 | 44:Stop fan |
| 45 | 45:Close external gas valve |
| 60 | 60:Pump post running |
| 61 | 61:Pump stop |
| 62 | 62:Close hydraulic valve |
| 63 | 63:Start anti-cycle timer |
| 255 | 255:Reset wait time |
| 999 | Unknown Sub-State |

### `failure.code`

| Value | Meaning |
|---|---|
| 0 | PSU not connected (Locking 0) |
| 1 | SU parameter fault (Locking 1) |
| 2 | 02:T Flow closed |
| 3 | 03:T Flow open |
| 4 | 04:T Flow < min. |
| 5 | 05:T Flow > max. |
| 6 | T Return closed (Locking 6) |
| 7 | T Return open (Locking 7) |
| 8 | T Return < min. (Locking 8) |
| 9 | T Return > max. (Locking 9) |
| 10 | 10:dT(Flow,Return) > max. |
| 11 | 11:dT(Return,Flow) > max. |
| 12 | STB activated (Locking 12) |
| 14 | 5x Unsuccessful start (Locking 14) |
| 16 | False flame (Locking 16) |
| 17 | SU Gasvalve driver error (Locking 17) |
| 34 | Fan out of control range (Locking 34) |
| 35 | Return over Flow temp. (Locking 35) |
| 36 | 5x Flame loss (Locking 36) |
| 37 | SU communication (Locking 37) |
| 38 | SCU-S communication (Locking 38) |
| 39 | BL input as lockout (Locking 39) |
| 40 | - (Locking 40) |
| 41 | E11: Airbox temp. > max. |
| 255 | No locking |
| 999 | Unknown locking code |

### `error.code`

| Value | Meaning |
|---|---|
| 0 | PCU parameter fault (Blocking 0) |
| 1 | T Flow > max.(Blocking 1) |
| 2 | dT/s Flow > max. (Blocking 2) |
| 7 | dT(Flow,Return) > max.(Blocking 7) |
| 8 | No release signal(Blocking 8) |
| 9 | L-N swept(Blocking 9) |
| 10 | Blocking signal ex frost(Blocking 10) |
| 11 | Blocking signal inc frost(Blocking 11) |
| 12 | HMI not connected(Blocking 12) |
| 13 | SCU communication(Blocking 13) |
| 14 | Min. water pressure(Blocking 14) |
| 15 | Min. gas pressure(Blocking 15) |
| 16 | Ident. SU mismatch(Blocking 16) |
| 17 | Ident. dF/dU table error(Blocking 17) |
| 18 | Ident. PSU mismatch(Blocking 18) |
| 19 | Ident. dF/dU needed(Blocking 19) |
| 20 | Identification running(Blocking 20) |
| 21 | SU communications lost(Blocking 21) |
| 22 | Flame lost(Blocking 22) |
| 25 | Internal SU error(Blocking 25) |
| 26 | Calorifier sensor error(Blocking 26) |
| 27 | DHW in sensor error(Blocking 27) |
| 28 | Reset in progress...(Blocking 28) |
| 255 | No blocking |
| 999 | Unknown blocking code |

### `no.yes.code`

| Value | Meaning |
|---|---|
| 0 | no |
| 1 | yes |

### `off.on.code`

| Value | Meaning |
|---|---|
| 0 | Off |
| 1 | On |

### `open.closed.code`

| Value | Meaning |
|---|---|
| 0 | Open |
| 1 | Closed |

### `closed.open.code`

| Value | Meaning |
|---|---|
| 0 | Closed |
| 1 | Open |

