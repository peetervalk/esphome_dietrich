# Live parameter image, PCU-05 P3

The 128 byte parameter block as read off the boiler at **2026-09-16 14:22:59**, one
minute before the first full-block write - so this is the image the appliance had
been running on, not the one it holds now.

Kept because a `Blocking 0` (*PCU parameter fault*) is one of the faults whose
documented remedy is to reload the factory parameter set from the **dF/dU codes on
the appliance's identification plate**, which discards every commissioned value.
This file is what you re-enter afterwards. The dF/dU codes are not in this image -
see *dF/dU is not in the parameter block* in `pcu05_p3_protocol.md`. See *The full-block write lands, and the PCU blocks on
it* in `pcu05_p3_protocol.md` for how the boiler got there.

The byte layout, the parameter numbering and the ranges below all come from the
*Full parameter block* table in `pcu05_p3_protocol.md`; only the **Raw** and
**Value** columns are this installation's.

## Raw blocks

EEPROM blocks `0x14`..`0x1B` at device address `0x00`:

```
0x14: 3D 38 01 00 0A FF FF FF FF FF FF FF FF FF FF FF
0x15: 2F 2F 0B 50 17 FF 5A 23 1E 19 FA 03 0A F6 00 18
0x16: 04 00 01 01 00 00 00 02 00 AF 1E 02 FF FF FF FF
0x17: FF FF FF FF FF FF 32 3C 50 00 C8 05 00 12 8F 05
0x18: FF 28 46 5A 32 19 05 1E 0A 1E 03 09 41 41 14 64
0x19: 64 64 07 46 05 05 1E 02 05 00 28 00 00 0F BC 09
0x1A: 00 78 00 0A 05 1E 65 4B 00 FE 0A 00 0A 64 FF 28
0x1B: 1A 02 05 05 02 02 05 14 14 19 05 FF FF FF 85 9A
```

One flat 128 byte string, the form the component logs it in:

```
3D3801000AFFFFFFFFFFFFFFFFFFFFFF2F2F0B5017FF5A231E19FA030AF60018040001010000000200AF1E02FFFFFFFFFFFFFFFFFFFF323C5000C80500128F05FF28465A3219051E0A1E0309414114646464074605051E0205002800000FBC090078000A051E654B00FE0A000A64FF281A02050502020514141905FFFFFF859A
```

Bytes 124..127 are `FF FF 85 9A`. They sit past the last documented parameter (p124
at byte 123) and are not a checksum of the image - `pcu05_p3_protocol.md` records
which algorithms were ruled out.

## Commissioned values

`Raw` is the stored byte. `Value` is it in the parameter's own units: p28 and p29
hold the percentage divided by ten, and p27, p30, p61, p86 and p106 are two's
complement. `Range` is copied verbatim from the map and is in *its* units, which is
not always `Value`'s - p28 reads `0x03` = 30 % against a documented range of 2..10,
because that range counts raw bytes. Parameters not listed (p6-p16, p22, p45-p54,
p63-p65, p111) are unused on this parameter set and read `FF`.

| Par | Blk | Byte | Name | Raw | Value | Unit | Range |
|---|---|---|---|---|---|---|---|
| **p1** | `0x14` | 0 | T flow set point | `0x3D` | 61 | °C | 20 … 90 |
| **p2** | `0x14` | 1 | DHW set point | `0x38` | 56 | °C | 40 … 65 |
| **p3** | `0x14` | 2 | Boiler controls | `0x01` | 1 |  | 0=Heating off, DHW off / 1=Heating on, DHW on / 2=Heating on, DHW off / 3=Heating off, DHW on |
| **p4** | `0x14` | 3 | Comfort DHW | `0x00` | 0 |  | 0=Always on / 1=Always off / 2=Controller |
| **p5** | `0x14` | 4 | Pump post run CH | `0x0A` | 10 | Min | 0 … 99 |
| **p17** | `0x15` | 16 | Full load HTG | `0x2F` | 47 |  | 1000 … 10000 (`A x 100`) |
| **p18** | `0x15` | 17 | Full load DHW | `0x2F` | 47 |  | 1000 … 10000 (`A x 100`) |
| **p19** | `0x15` | 18 | Partload HTG/DHW | `0x0B` | 11 |  | 1000 … 5000 (`A x 100`) |
| **p20** | `0x15` | 19 | Offset partload | `0x50` | 80 |  | 0 … 99 |
| **p21** | `0x15` | 20 | Start load | `0x17` | 23 |  | 1000 … 5000 (`A x 100`) |
| **p23** | `0x15` | 22 | Max Flow system | `0x5A` | 90 | °C | 20 … 90 |
| **p24** | `0x15` | 23 | Factor avg flow | `0x23` | 35 |  | 1 … 255 |
| **p25** | `0x15` | 24 | Footpoint T outside | `0x1E` | 30 | °C | 0 … 30 |
| **p26** | `0x15` | 25 | Footpoint Tflow | `0x19` | 25 | °C | 0 … 90 |
| **p27** | `0x15` | 26 | Clima p outside temp | `0xFA` | -6 | °C | -30 … 0 (`sgn(A)`) |
| **p28** | `0x15` | 27 | Pump CH min | `0x03` | 30 | *10% | 2 … 10 |
| **p29** | `0x15` | 28 | Pump CH max | `0x0A` | 100 | *10% | 2 … 10 |
| **p30** | `0x15` | 29 | Temp frostprot. | `0xF6` | -10 | °C | -30 … 0 (`sgn(A)`) |
| **p31** | `0x15` | 30 | Anti Legionella | `0x00` | 0 |  | 0=no / 1=yes / 2=Controller |
| **p32** | `0x15` | 31 | Setpoint raise DHW | `0x18` | 24 | °C | 0 … 25 |
| **p33** | `0x16` | 32 | Hystereses calorif. | `0x04` | 4 | °C | 2 … 15 |
| **p34** | `0x16` | 33 | Ext. 3-way valve standby | `0x00` | 0 |  | 0=CH / 1=DHW |
| **p35** | `0x16` | 34 | Boiler type: | `0x01` | 1 |  | 0=Combi / 1=Solo (+boiler) / 2=Comfort Column / 3=Open vented |
| **p36** | `0x16` | 35 | Blocking input | `0x01` | 1 |  | 1=Blocking without frostprot. / 2=Blocking with frostprot. / 3=Locking with frostprot. |
| **p37** | `0x16` | 36 | Min. gas pressure | `0x00` | 0 |  | 0=no / 1=yes |
| **p38** | `0x16` | 37 | HRU active | `0x00` | 0 |  | 0=no / 1=yes |
| **p39** | `0x16` | 38 | Fluegas valve time | `0x00` | 0 | sec | 0 … 255 |
| **p40** | `0x16` | 39 | Status report | `0x02` | 2 |  | 0=Operation signal / 1=failure signal / 2=Ext. 3-way valve |
| **p41** | `0x16` | 40 | Service notification | `0x00` | 0 |  | 0=Off / 1=ABC / 2=Custom |
| **p42** | `0x16` | 41 | Service hours | `0xAF` | 175 | x100h | 1 … 255 |
| **p43** | `0x16` | 42 | Service burning | `0x1E` | 30 | x100h | 1 … 255 |
| **p44** | `0x16` | 43 | De-airation cycle | `0x02` | 2 |  | 0=No pump, no de-airation / 1=Single speed pump / 2=Modulating pump |
| **p55** | `0x17` | 54 | Stop contr. cooling | `0x32` | 50 | °C | 20 … 100 |
| **p56** | `0x17` | 55 | Start contr cooling | `0x3C` | 60 | °C | 20 … 100 |
| **p57** | `0x17` | 56 | Max controller temp | `0x50` | 80 | °C | 20 … 100 |
| **p58** | `0x17` | 57 | Gas air type | `0x00` | 0 |  | 0=GA 28 / 1=GA 40 |
| **p59** | `0x17` | 58 | DHW-in gradient | `0xC8` | 200 | .01C/s | 1 … 200 |
| **p60** | `0x17` | 59 | dT pomp offset | `0x05` | 5 | °C | 0 … 100 |
| **p61** | `0x17` | 60 | Offset control temp | `0x00` | 0 | /10°C | -100 … 100 (`sgn(A)`) |
| **p62** | `0x17` | 61 | DHW Flow at RPM min | `0x12` | 18 | l/min | 0 … 5 (`A x 0.1`) |
| **p66** | `0x18` | 65 | Gradiënt dTMax 1 | `0x28` | 40 | .01C/s | 1 … 200 |
| **p67** | `0x18` | 66 | Gradiënt dTMax 2 | `0x46` | 70 | .01C/s | 1 … 200 |
| **p68** | `0x18` | 67 | Gradiënt dTMax 3 | `0x5A` | 90 | .01C/s | 1 … 200 |
| **p69** | `0x18` | 68 | dT(Flow,Return) | `0x32` | 50 | °C | 0 … 60 |
| **p70** | `0x18` | 69 | Startpoint modul. | `0x19` | 25 | °C | 10 … 40 |
| **p71** | `0x18` | 70 | Pump dTset CH | `0x05` | 5 | °C | 0 … 40 |
| **p72** | `0x18` | 71 | Pump CH start | `0x1E` | 30 | % | 0 … 100 |
| **p73** | `0x18` | 72 | Hysterese CH | `0x0A` | 10 | °C | 1 … 10 |
| **p74** | `0x18` | 73 | Stabilization time | `0x1E` | 30 | sec | 10 … 180 |
| **p75** | `0x18` | 74 | Min. burner  off | `0x03` | 3 | Min | 1 … 15 |
| **p76** | `0x18` | 75 | Max. burner off | `0x09` | 9 | Min | 3 … 15 |
| **p77** | `0x18` | 76 | Max. fanspeed CH | `0x41` | 65 |  | 1000 … 10000 (`A x 100`) |
| **p78** | `0x18` | 77 | Max. fanspeed DHW | `0x41` | 65 |  | 1000 … 10000 (`A x 100`) |
| **p79** | `0x18` | 78 | Pump dTset DHW | `0x14` | 20 | °C | 5 … 40 |
| **p80** | `0x18` | 79 | Pump DHW min | `0x64` | 100 | % | 0 … 100 |
| **p81** | `0x19` | 80 | Pump DHW max | `0x64` | 100 | % | 0 … 100 |
| **p82** | `0x19` | 81 | Pump DHW start | `0x64` | 100 | % | 0 … 100 |
| **p83** | `0x19` | 82 | Warm up interval CH | `0x07` | 7 | Min | 0 … 255 |
| **p84** | `0x19` | 83 | Warm up interval | `0x46` | 70 | Min | 0 … 255 |
| **p85** | `0x19` | 84 | Hysterese warming up | `0x05` | 5 | °C | 0 … 20 |
| **p86** | `0x19` | 85 | Offset warming up | `0x05` | 5 | °C | -30 … 20 (`sgn(A)`) |
| **p87** | `0x19` | 86 | DHW start raise | `0x1E` | 30 | .1C/l/m | 0 … 100 |
| **p88** | `0x19` | 87 | Hystereses DHW | `0x02` | 2 | °C | 1 … 10 |
| **p89** | `0x19` | 88 | Offset DHW | `0x05` | 5 | °C | 0 … 20 |
| **p90** | `0x19` | 89 | Offset pl heat exch | `0x00` | 0 | °C | 0 … 20 |
| **p91** | `0x19` | 90 | %Tf/Tr DHW pump 20% | `0x28` | 40 | % | 0 … 100 |
| **p92** | `0x19` | 91 | %Tf/Tr DHW pump 100% | `0x00` | 0 | % | 0 … 100 |
| **p93** | `0x19` | 92 | Delay pump DHW | `0x00` | 0 | /10 sec | 0 … 100 |
| **p94** | `0x19` | 93 | Post pump time DHW | `0x0F` | 15 | sec | 1 … 99 |
| **p95** | `0x19` | 94 | K-Factor DHW | `0xBC` | 188 | /sec | 1 … 255 |
| **p96** | `0x19` | 95 | Min DHW flow | `0x09` | 9 | l/min | 0 … 10 (`A x 0.1`) |
| **p97** | `0x1A` | 96 | DHW sensor | `0x00` | 0 |  | 0=DHW Flow sensor / 1=DHW Flow switch |
| **p98** | `0x1A` | 97 | Flowdetection time | `0x78` | 120 |  | 0 … 255 |
| **p99** | `0x1A` | 98 | DHW stabilisation | `0x00` | 0 | /10 sec | 0 … 255 |
| **p100** | `0x1A` | 99 | DHW gradiënt | `0x0A` | 10 | .01C/s | 1 … 200 |
| **p101** | `0x1A` | 100 | DHW Booster off | `0x05` | 5 | °C | 0 … 99 |
| **p102** | `0x1A` | 101 | P DHW start | `0x1E` | 30 |  | 0 … 200 |
| **p103** | `0x1A` | 102 | P DHW FFWD | `0x65` | 101 |  | 0 … 200 |
| **p104** | `0x1A` | 103 | P DHW flowchanges | `0x4B` | 75 |  | 0 … 200 |
| **p105** | `0x1A` | 104 | Offset calorifier | `0x00` | 0 | °C | 0 … 10 |
| **p106** | `0x1A` | 105 | Start DHW pump | `0xFE` | -2 | °C | -20 … 20 (`sgn(A)`) |
| **p107** | `0x1A` | 106 | Post pump time DHW | `0x0A` | 10 | sec | 1 … 255 |
| **p108** | `0x1A` | 107 | Prepurge time | `0x00` | 0 | sec | 0 … 255 |
| **p109** | `0x1A` | 108 | Postpurge time | `0x0A` | 10 | sec | 0 … 255 |
| **p110** | `0x1A` | 109 | Max. Flow temp. | `0x64` | 100 | °C | 0 … 110 |
| **p112** | `0x1A` | 111 | P factor fan | `0x28` | 40 |  | 0 … 100 |
| **p113** | `0x1B` | 112 | I factor fan | `0x1A` | 26 |  | 1 … 200 |
| **p114** | `0x1B` | 113 | P factor CH | `0x02` | 2 |  | 0 … 100 |
| **p115** | `0x1B` | 114 | I factor CH | `0x05` | 5 |  | 1 … 200 |
| **p116** | `0x1B` | 115 | P factor CH down | `0x05` | 5 |  | 0 … 100 |
| **p117** | `0x1B` | 116 | I factor CH down | `0x02` | 2 |  | 1 … 200 |
| **p118** | `0x1B` | 117 | P factor DHW | `0x02` | 2 |  | 0 … 100 |
| **p119** | `0x1B` | 118 | I factor DHW | `0x05` | 5 |  | 1 … 200 |
| **p120** | `0x1B` | 119 | I factor pump CH | `0x14` | 20 |  | 1 … 200 |
| **p121** | `0x1B` | 120 | I factor pump DHW | `0x14` | 20 |  | 1 … 200 |
| **p122** | `0x1B` | 121 | RPM at 0KW | `0x19` | 25 |  | 0 … 255 |
| **p123** | `0x1B` | 122 | KW at 10000 RPM | `0x05` | 5 |  | 0 … 255 |
| **p124** | `0x1B` | 123 | Power rating | `0xFF` | 255 |  | 0 … 255 |

98 of the 128 bytes are documented in the map. The other 30 are in the raw blocks
above and go back verbatim whatever they mean.

## What changed since

2026-09-16 14:23:14 wrote **p33 = 6** (byte 32, `0x04` -> `0x06`), verified by
read-back. Every other byte of the 128 went back identical. The boiler reported
`Blocking 0` fourteen seconds later. Nothing has been written since.

The full EEPROM sweep at 15:50 re-read all eight blocks and confirms it from the
other side: the image above and the one on the boiler now differ at **byte 32 and
nowhere else**, so the write is still in place and took nothing else with it. The
sweep also rules the obvious culprit out - the parameter block carries no checksum
that the write could have left stale. See
[`pcu05_p3_eeprom_map.md`](pcu05_p3_eeprom_map.md).
