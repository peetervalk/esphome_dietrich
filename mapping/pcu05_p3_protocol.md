# Remeha protocol — frames, commands and the EEPROM parameter block

Recovered from **Recom 7.3.8** (`RecomAppWix_7-3-8.msi` → `media1.cab` → `RecomAppExe`,
a .NET single-file bundle → `RecomProgram.Core.dll`), namespace
`RecomProgram.Communication.Remeha`: `RemehaMessage.InitializeMessage`,
`RemehaMessageFactory`, `RemehaBoilerController`.

Everything below is **verified**: the rules here reproduce all three request frames
already hard-coded in `components/dietrich/dietrich.cpp` byte-for-byte, CRC included.

## Frame layout

```
 0    1      2     3     4       5        6       7 …      n-3   n-2   n-1
02 | DEST | SRC | 05 | LEN | COMMAND | EXTCMD | data … | CRClo CRChi | 03
```

- `[3]` is the constant **`0x05`**, not a length.
- `[4]` is **`frame_length - 2`** — 0x08 for a 10-byte request, 0x18 for a 26-byte write.
- `[5]` is `COMMAND`, `[6]` is `EXT_COMMAND` (for EEPROM ops, the block index).
- CRC16 poly `0xA001`, init `0xFFFF`, over bytes `1 … n-4`, appended lo-byte first.
- Responses repeat the same 7-byte header, so response data starts at index 7.

> The comment at the top of `dietrich.cpp` labels these `LEN | FUNC | BLOCK | SUB`,
> which is shifted one position and calls `[4]` a function code. The bytes it sends
> are correct; only the naming was off.

## COMMAND (byte 5)

| Value | Name | Notes |
|---|---|---|
| `0x01` | IDENTIFICATION | with EXT `0x0B` |
| `0x02` | SAMPLES | with EXT `0x01` (`SAMPLES_FORMAT`) — the live-data poll |
| `0x08` | CODE_SERVICE_START | with EXT `0x0C` — **unlocks writing** |
| `0x09` | CODE_FACTORY_COMMANDO | |
| `0x10` | READ_EPROM_BLOCK | EXT = block index, returns 16 bytes |
| `0x11` | WRITE_EPROM_BLOCK | EXT = block index, carries 16 bytes |
| `0x1F` | CODE_SERVICE_STOP | with EXT `0x0C` — re-locks |
| `0x31` | RESET | |
| `0x32` | SET_DFDU | |
| `0x33` | AUTO_DETECT | |
| `0x37` | SERVICE_CODE | |
| `0x66` | SCU_C_SAMPLES | sample variant for SCU-C devices |
| `0x67` | CALIBRATE_SCOT | |

`EXT_COMMAND`: `NONE=0`, `SAMPLES_FORMAT=1`, `IDENTIFICATION=11`, `CODE_SERVICE=12`,
`CODE_FACTORY=82`.

## EEPROM map

The PCU-05 exposes 2048 bytes of EEPROM in **16-byte blocks**, addressed by the
EXT_COMMAND byte. From `RemehaBoilerController`:

| Blocks | Bytes | Contents | Call site |
|---|---|---|---|
| `0x14`–`0x1B` | 128 | **parameter block** (p1 … p124) | `GetEepromData(0x14, 8)` / `SetParameterModel` |
| `0x1C`–`0x1F` | 64 | counter block | `GetEepromData(0x1C, 4)` |

The two counter requests already in `dietrich.cpp` (`…08 10 1C…`, `…08 10 1D…`) are
EEPROM reads of blocks 28 and 29 — the component has been using the EEPROM read
command all along without naming it.

## Reading a parameter

Plain `READ_EPROM_BLOCK`, no unlock needed:

```
02 FE 01 05 08 10 14 A4 C4 03   # block 0x14 -> parameter bytes   0..15  (p1..p5)
02 FE 01 05 08 10 15 65 04 03   # block 0x15 -> parameter bytes  16..31  (p17..p32)
02 FE 01 05 08 10 16 25 05 03   # block 0x16 -> parameter bytes  32..47  (p33..p44)
02 FE 01 05 08 10 17 E4 C5 03
02 FE 01 05 08 10 18 A4 C1 03
02 FE 01 05 08 10 19 65 01 03
02 FE 01 05 08 10 1A 25 00 03
02 FE 01 05 08 10 1B E4 C0 03
```

`SRC` is `0x01` in the sample request and `0x00` in the counter requests, so the
boiler evidently does not care.

## Writing a parameter

`RemehaBoilerController.SetParameterModel` is, in full:

```csharp
if (EnableServiceMode(true, dest)) {
    ValidateDataModel(dataModel);                       // clamps to the XML min/max
    SetEepromData(dataModel.GetBufferByIndex(0), 0x14, 8, dest);
    EnableServiceMode(false, dest);
}
```

and `SetEepromData` writes each block as:

```csharp
for (int i = 0; i < blockCount; i++) {
    var m = factory.CreateEpromWriteMessage((byte)(startIndex + i), dest);
    Array.Copy(data, i * 16, m.Content, 7, 16);         // 16 payload bytes at offset 7
    factory.SetMessageCrc(m);
    var r = SendAndRecieveMessage(m, 1000);             // 1 s timeout
    if (r == null || !r.IsAcknowledged()) throw new CommunicationException();
}
```

So the sequence is:

1. `02 FE 01 05 08 08 0C AE CE 03` — service mode **on**, wait for ACK
2. `02 FE 01 05 18 11 <blk> <16 bytes> <CRClo> <CRChi> 03` — write block, wait for ACK
3. `02 FE 01 05 08 1F 0C A1 3E 03` — service mode **off**

Recom rewrites all 8 blocks; nothing stops a single-block write, but **read the block
first and modify one byte**, since the other 15 bytes go back verbatim.

### Cautions

- The parameter block holds the gas/air settings (p17–p21, p77, p78) and the
  controller-protection limits (p55–p57). A bad write here is a combustion-safety
  problem, not a comfort one. `ValidateDataModel` exists because Recom clamps every
  value to the XML `min`/`max` before sending — do the same.
- EEPROM endurance is finite. This is not somewhere to write on a schedule.
- Write support does not exist in this component today; this document is the
  specification for adding it, not a description of what it does.

## Hysteresis parameters

| Par | Blk | Off | Name | Range | Factory |
|---|---|---|---|---|---|
| **p33** | `0x16` | 0 | Hysteresis calorifier — DHW cut-in below tank setpoint | 2 … 15 °C | **4** |
| p73 | `0x18` | 8 | Hysteresis CH — start hysteresis for CH | 1 … 10 °C | — |
| p85 | `0x19` | 4 | Hysteresis warming up (DHW comfort) | 0 … 20 °C | — |
| p88 | `0x19` | 7 | Hysteresis DHW — switch-on hysteresis, DHW operation | 1 … 10 °C | — |
| p105 | `0x1A` | 8 | Offset calorifier — switch-**off** offset, tank sensor | 0 … 10 °C | — |
| p32 | `0x15` | 15 | Setpoint raise while charging the calorifier | 0 … 25 °C | 20 |

`p33` is the one that governs a tank system: the manual calls it *"DHW cut-in
temperature DHW sensor"* and gives a factory setting of **4** for every EMC-M model,
which matches the observed 56 → 52 °C cut-in. `p88` applies to combi/flow-through DHW.

## Full parameter block

Byte = offset within the 128-byte parameter block; Blk/Off = EEPROM block and index
within it. Gaps (p6–p16, p22, p45–p54, p63–p65, p111) are unused on this parameter set.

| Par | Blk | Off | Byte | Name | Unit | Range / values | Description |
|---|---|---|---|---|---|---|---|
| **p1** | `0x14` | 0 | 0 | T flow set point | °C | 20 … 90 | Max flow temperature during CH mode |
| **p2** | `0x14` | 1 | 1 | DHW set point | °C | 40 … 65 | desired DHW temperature  |
| **p3** | `0x14` | 2 | 2 | Boiler controls |  | 0=Heating off, DHW off / 1=Heating on, DHW on / 2=Heating on, DHW off / 3=Heating off, DHW on | Switch on/off CH/DHW function |
| **p4** | `0x14` | 3 | 3 | Comfort DHW |  | 0=Always on / 1=Always off / 2=Controller | Keeping warm for DHW comfort |
| **p5** | `0x14` | 4 | 4 | Pump post run CH | Min | 0 … 99 | Pump post run time CH, 99 = Pump continuous |
| **p17** | `0x15` | 0 | 16 | Full load HTG |  | 1000 … 10000 (`A x 100`) | Max fanspeed during CH mode |
| **p18** | `0x15` | 1 | 17 | Full load DHW |  | 1000 … 10000 (`A x 100`) | Max fanspeed during DHW mode |
| **p19** | `0x15` | 2 | 18 | Partload HTG/DHW |  | 1000 … 5000 (`A x 100`) | Min fanspeed during CH + DHW mode |
| **p20** | `0x15` | 3 | 19 | Offset partload |  | 0 … 99 | Offset on partload fanspeed |
| **p21** | `0x15` | 4 | 20 | Start load |  | 1000 … 5000 (`A x 100`) | Fan speed at boiler start |
| **p23** | `0x15` | 6 | 22 | Max Flow system | °C | 20 … 90 | Maximum flow temperature for heating system |
| **p24** | `0x15` | 7 | 23 | Factor avg flow |  | 1 … 255 | Tau factor for average flow temperature calculation |
| **p25** | `0x15` | 8 | 24 | Footpoint T outside | °C | 0 … 30 | Footpoint heating curve outside temp |
| **p26** | `0x15` | 9 | 25 | Footpoint Tflow | °C | 0 … 90 | Footpoint heating curve flow temp |
| **p27** | `0x15` | 10 | 26 | Clima p outside temp | °C | -30 … 0 (`sgn(A)`) | Clima point heating curve outside temp. |
| **p28** | `0x15` | 11 | 27 | Pump CH min | *10% | 2 … 10 | Pump control, CH minimum speed |
| **p29** | `0x15` | 12 | 28 | Pump CH max | *10% | 2 … 10 | Pump control, CH maximum speed |
| **p30** | `0x15` | 13 | 29 | Temp frostprot. | °C | -30 … 0 (`sgn(A)`) | Minimal outside temperature for frost protection |
| **p31** | `0x15` | 14 | 30 | Anti Legionella |  | 0=no / 1=yes / 2=Controller | Legionella protection calorifier |
| **p32** | `0x15` | 15 | 31 | Setpoint raise DHW | °C | 0 … 25 | Setpoint raise at warming up calorifier |
| **p33** | `0x16` | 0 | 32 | Hystereses calorif. | °C | 2 … 15 | Switch on hystereses calorifier sensor |
| **p34** | `0x16` | 1 | 33 | Ext. 3-way valve standby |  | 0=CH / 1=DHW | Ext. 3-way valve standby: CH or DHW |
| **p35** | `0x16` | 2 | 34 | Boiler type: |  | 0=Combi / 1=Solo (+boiler) / 2=Comfort Column / 3=Open vented | Boiler type and control type |
| **p36** | `0x16` | 3 | 35 | Blocking input |  | 1=Blocking without frostprot. / 2=Blocking with frostprot. / 3=Locking with frostprot. | Function of blocking input |
| **p37** | `0x16` | 4 | 36 | Min. gas pressure |  | 0=no / 1=yes | Minimum gas pressure detection |
| **p38** | `0x16` | 5 | 37 | HRU active |  | 0=no / 1=yes | HRU connected |
| **p39** | `0x16` | 6 | 38 | Fluegas valve time | sec | 0 … 255 | Wait time for fluegas valve |
| **p40** | `0x16` | 7 | 39 | Status report |  | 0=Operation signal / 1=failure signal / 2=Ext. 3-way valve | Position from alarm./oper.signal relais |
| **p41** | `0x16` | 8 | 40 | Service notification |  | 0=Off / 1=ABC / 2=Custom | Service notification for boiler dependant mantenance |
| **p42** | `0x16` | 9 | 41 | Service hours | x100h | 1 … 255 | Service hours for boiler conncted to mains supply |
| **p43** | `0x16` | 10 | 42 | Service burning | x100h | 1 … 255 | Service hours for boiler burner |
| **p44** | `0x16` | 11 | 43 | De-airation cycle |  | 0=No pump, no de-airation / 1=Single speed pump / 2=Modulating pump | Pump type for de-airation cycle on startup |
| **p55** | `0x17` | 6 | 54 | Stop contr. cooling | °C | 20 … 100 | Stop forced controller cooling at this temperature |
| **p56** | `0x17` | 7 | 55 | Start contr cooling | °C | 20 … 100 | Start forced controller cooling at this temperature |
| **p57** | `0x17` | 8 | 56 | Max controller temp | °C | 20 … 100 | Absolute maximum temperature of controller for lockout |
| **p58** | `0x17` | 9 | 57 | Gas air type |  | 0=GA 28 / 1=GA 40 | Type of the Gas-Air unit |
| **p59** | `0x17` | 10 | 58 | DHW-in gradient | .01C/s | 1 … 200 | DHW-in gradient for rerstart stabilisationtime |
| **p60** | `0x17` | 11 | 59 | dT pomp offset | °C | 0 … 100 | dT pomp offset |
| **p61** | `0x17` | 12 | 60 | Offset control temp | /10°C | -100 … 100 (`sgn(A)`) | Offset control temp |
| **p62** | `0x17` | 13 | 61 | DHW Flow at RPM min | l/min | 0 … 5 (`A x 0.1`) | DHW Flow at minimum output power |
| **p66** | `0x18` | 1 | 65 | Gradiënt dTMax 1 | .01C/s | 1 … 200 | Maximum gradiënt for decreasing modulation |
| **p67** | `0x18` | 2 | 66 | Gradiënt dTMax 2 | .01C/s | 1 … 200 | Maximum gradiënt for forced minimal load |
| **p68** | `0x18` | 3 | 67 | Gradiënt dTMax 3 | .01C/s | 1 … 200 | Maximum gradiënt voor blocking |
| **p69** | `0x18` | 4 | 68 | dT(Flow,Return) | °C | 0 … 60 | Maximum temp difference between flow and return |
| **p70** | `0x18` | 5 | 69 | Startpoint modul. | °C | 10 … 40 | Modulate back when dT > this par |
| **p71** | `0x18` | 6 | 70 | Pump dTset CH | °C | 0 … 40 | Pump control, control range dT for CH |
| **p72** | `0x18` | 7 | 71 | Pump CH start | % | 0 … 100 | Pump control, CH on start heatdemand |
| **p73** | `0x18` | 8 | 72 | Hysterese CH | °C | 1 … 10 | Start hysteresis for CH |
| **p74** | `0x18` | 9 | 73 | Stabilization time | sec | 10 … 180 | Stabilization time after burner start CH |
| **p75** | `0x18` | 10 | 74 | Min. burner  off | Min | 1 … 15 | Minimum burner anti-cycle time |
| **p76** | `0x18` | 11 | 75 | Max. burner off | Min | 3 … 15 | Maximum burner anti-cycle time |
| **p77** | `0x18` | 12 | 76 | Max. fanspeed CH |  | 1000 … 10000 (`A x 100`) | Absolute max fan speed CH |
| **p78** | `0x18` | 13 | 77 | Max. fanspeed DHW |  | 1000 … 10000 (`A x 100`) | Absolute max fan speed DHW |
| **p79** | `0x18` | 14 | 78 | Pump dTset DHW | °C | 5 … 40 | Pump control, control range dT for DHW |
| **p80** | `0x18` | 15 | 79 | Pump DHW min | % | 0 … 100 | Pump control, DHW minimum speed |
| **p81** | `0x19` | 0 | 80 | Pump DHW max | % | 0 … 100 | Pump control, DHW maximum speed |
| **p82** | `0x19` | 1 | 81 | Pump DHW start | % | 0 … 100 | Pump control, DHW on start DHW demand |
| **p83** | `0x19` | 2 | 82 | Warm up interval CH | Min | 0 … 255 | Warm up interval for DHW after CH |
| **p84** | `0x19` | 3 | 83 | Warm up interval | Min | 0 … 255 | Time between warming up starts boiler |
| **p85** | `0x19` | 4 | 84 | Hysterese warming up | °C | 0 … 20 | Hysterese when warming up for DHW comfort |
| **p86** | `0x19` | 5 | 85 | Offset warming up | °C | -30 … 20 (`sgn(A)`) | Offset when warming up for DHW comfort |
| **p87** | `0x19` | 6 | 86 | DHW start raise | .1C/l/m | 0 … 100 | DHW start raise depending op DHW flow |
| **p88** | `0x19` | 7 | 87 | Hystereses DHW | °C | 1 … 10 | Switch on hystereses DHW operation |
| **p89** | `0x19` | 8 | 88 | Offset DHW | °C | 0 … 20 | Offset DHW |
| **p90** | `0x19` | 9 | 89 | Offset pl heat exch | °C | 0 … 20 | T corr. DHW for Tset, ww - Tret, plate heat exch. |
| **p91** | `0x19` | 10 | 90 | %Tf/Tr DHW pump 20% | % | 0 … 100 | %Tf/Tr for DHW control temp at pumpspeed 20% |
| **p92** | `0x19` | 11 | 91 | %Tf/Tr DHW pump 100% | % | 0 … 100 | %Tf/Tr for DHW control temp at pumpspeed 100% |
| **p93** | `0x19` | 12 | 92 | Delay pump DHW | /10 sec | 0 … 100 | Waiting time pump for prehaet plate heat exchanger |
| **p94** | `0x19` | 13 | 93 | Post pump time DHW | sec | 1 … 99 | Postpump time DHW |
| **p95** | `0x19` | 14 | 94 | K-Factor DHW | /sec | 1 … 255 | Correction factor DHW pulses to l/min |
| **p96** | `0x19` | 15 | 95 | Min DHW flow | l/min | 0 … 10 (`A x 0.1`) | Min DHW flow for DHW detection |
| **p97** | `0x1A` | 0 | 96 | DHW sensor |  | 0=DHW Flow sensor / 1=DHW Flow switch | Type of sensor for DHW detection |
| **p98** | `0x1A` | 1 | 97 | Flowdetection time |  | 0 … 255 | Factor for dynamic flowdetection |
| **p99** | `0x1A` | 2 | 98 | DHW stabilisation | /10 sec | 0 … 255 | DHW stabilisation time for pump modulation |
| **p100** | `0x1A` | 3 | 99 | DHW gradiënt | .01C/s | 1 … 200 | DHW gradiënt for stabilisation time pump |
| **p101** | `0x1A` | 4 | 100 | DHW Booster off | °C | 0 … 99 | Range in which the DHW booster is diabled |
| **p102** | `0x1A` | 5 | 101 | P DHW start |  | 0 … 200 | P factor for booster on start DHW |
| **p103** | `0x1A` | 6 | 102 | P DHW FFWD |  | 0 … 200 | P factor for feedforward on flow DHW |
| **p104** | `0x1A` | 7 | 103 | P DHW flowchanges |  | 0 … 200 | P factor for booster on flowchanges DHW |
| **p105** | `0x1A` | 8 | 104 | Offset calorifier | °C | 0 … 10 | Switch off offset calorifier sensor |
| **p106** | `0x1A` | 9 | 105 | Start DHW pump | °C | -20 … 20 (`sgn(A)`) | Switch on delay DHW pump in comparison with boiler pump |
| **p107** | `0x1A` | 10 | 106 | Post pump time DHW | sec | 1 … 255 | Post pump time DHW |
| **p108** | `0x1A` | 11 | 107 | Prepurge time | sec | 0 … 255 | Prepurge time for burner start |
| **p109** | `0x1A` | 12 | 108 | Postpurge time | sec | 0 … 255 | Postpurge time for burner stop |
| **p110** | `0x1A` | 13 | 109 | Max. Flow temp. | °C | 0 … 110 | Maximum flow temperature for blocking |
| **p112** | `0x1A` | 15 | 111 | P factor fan |  | 0 … 100 | P factor fan speed control |
| **p113** | `0x1B` | 0 | 112 | I factor fan |  | 1 … 200 | I factor fan speed control |
| **p114** | `0x1B` | 1 | 113 | P factor CH |  | 0 … 100 | P factor CH control |
| **p115** | `0x1B` | 2 | 114 | I factor CH |  | 1 … 200 | I factor CH control |
| **p116** | `0x1B` | 3 | 115 | P factor CH down |  | 0 … 100 | P factor for CH control when T1>setpoint |
| **p117** | `0x1B` | 4 | 116 | I factor CH down |  | 1 … 200 | I factor for CH control when T1>setpoint |
| **p118** | `0x1B` | 5 | 117 | P factor DHW |  | 0 … 100 | P factor DHW control |
| **p119** | `0x1B` | 6 | 118 | I factor DHW |  | 1 … 200 | I factor DHW control |
| **p120** | `0x1B` | 7 | 119 | I factor pump CH |  | 1 … 200 | I factor for pump control on CH |
| **p121** | `0x1B` | 8 | 120 | I factor pump DHW |  | 1 … 200 | I factor for pump control on DHW |
| **p122** | `0x1B` | 9 | 121 | RPM at 0KW |  | 0 … 255 | RPM at theoretical oKW |
| **p123** | `0x1B` | 10 | 122 | KW at 10000 RPM |  | 0 … 255 | Power output (KW) at theoretical 10000RPM |
| **p124** | `0x1B` | 11 | 123 | Power rating |  | 0 … 255 | Power rating |

## Reproducing this

`mapping/` already carries `PCU-05_P3.xml` and `language.xml`. To rebuild the command
tables from the installer:

1. `7z e RecomAppWix_7-3-8.msi media1.cab` then `7z e media1.cab RecomAppExe`
2. `RecomAppExe` is a .NET single-file bundle v1.0 — the manifest sits at the int64
   stored 8 bytes before the bundle signature
   `8B1202B96A612038727B930214D7A03213F5B9E6EFAE3318EE3B2DCE24B36AAE`, and lists
   `(offset, size, type, path)` per file. `RecomProgram.Core.dll` carves straight out.
3. Read its IL with `System.Reflection.Metadata` (in-box in the .NET SDK).
