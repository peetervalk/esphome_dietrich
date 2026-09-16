# Remeha protocol — frames, commands and the EEPROM parameter block

Recovered from **Recom 7.3.8** (`RecomAppWix_7-3-8.msi` → `media1.cab` → `RecomAppExe`,
a .NET single-file bundle → `RecomProgram.Core.dll`), namespace
`RecomProgram.Communication.Remeha`: `RemehaMessage.InitializeMessage`,
`RemehaMessageFactory`, `RemehaBoilerController`.

Everything below is **verified** twice over: the rules here reproduce all three request
frames already hard-coded in `components/dietrich/dietrich.cpp` byte-for-byte, CRC
included, and the response rules were checked against a live PCU-05 P3 capture (see
*Response validation*).

## Frame layout

```
 0    1     2      3      4       5        6       7 …      n-3   n-2   n-1
02 | SRC | DEST | TYPE | LEN | COMMAND | EXTCMD | data … | CRClo CRChi | 03
```

- `[1]` is the **sender** and `[2]` the **recipient**; they swap between a request
  and its response. From `RemehaMessage.InitializeMessage`, a request always carries
  `[1] = DeviceIdentifier.PC`.
- `[3]` is the **message type**: `0x05` in a request, `0x06` in a response.
  `RemehaMessage.IsAcknowledged()` is exactly `content[3] == 6`, so this one byte is
  both the direction marker and the ACK flag — there is no separate ACK field, and
  an ACK is an ordinary response frame.
- `[4]` is **`frame_length - 2`** — 0x08 for a 10-byte request, 0x18 for a 26-byte write.
- `[5]` is `COMMAND`, `[6]` is `EXT_COMMAND` (for EEPROM ops, the block index).
- CRC16 poly `0xA001`, init `0xFFFF`, over bytes `1 … n-4`, appended lo-byte first.
- Both directions use the same 7-byte header, so data starts at index 7 and runs for
  `frame_length - 10` bytes (`RemehaMessage.GetData`).

> An earlier revision of this document called `[3]` "the constant `0x05`" and labelled
> `[1]`/`[2]` DEST/SRC. Both were wrong; the layout above is what the IL actually does.

## Device addresses

`RecomLibrary.Data.DeviceIdentifier` is constructed as `(deviceType, address)`:

| Device | Address |
|---|---|
| PSU | `0x00` |
| PCU | `0x01` |
| SCU_C | `0x02` |
| SU | `0x03` |
| SCU_S | `0x08` |
| PC | `0xFE` |
| NO_DEVICE | `0xFF` |

Every request Recom builds is `[1] = 0xFE` (itself) and `[2] =` the device it is
talking to — so for a boiler, `[2] = 0x01`.

> `dietrich.cpp` sends the sample request to `0x01` but the counter and parameter
> reads to `0x00`, which is nominally the **PSU**, not the PCU. The board answers on
> either and **echoes back whichever address it was sent** — verified on a PCU-05 P3
> against both — so the swap rule below holds regardless of which is used. Writes
> should nevertheless be addressed to `0x01`, which is what Recom does.

## Response validation

`RemehaReceiver.ValidateResponse` runs on **every** exchange, read or write:

```csharp
if (response == null)                          throw "Response message is null.";
if (!response.IsAcknowledged())                throw "The response message is not an ACK-message.";
if (request.Content[1] != response.Content[2]) throw "...device-address of the receiver...";
if (request.Content[2] != response.Content[1]) throw "...device-address of the sender...";
if (request.Content[5] != response.Content[5]) throw "...command-value...";
if (request.Content[6] != response.Content[6]) throw "...extended command-value...";
```

So a response is good when byte 3 is `0x06`, the two addresses are swapped against the
request, and COMMAND/EXT_COMMAND are echoed — on top of the CRC.

> Recom itself does not benefit from any of this: `Receiver.Receive` wraps the
> `ValidateResponse` call in a catch-all that swallows the exception and returns the
> message anyway. The checks are sound; Recom's use of them is not.

### Verified against hardware

A live sample response from a PCU-05 P3, logged by this component:

```
0201FE06 48 0201 470DF30B80F3FA0530160080D40D0080820FE015008000…  (74 bytes)
```

| Byte | Value | Meaning |
|---|---|---|
| `[0]` | `02` | STX |
| `[1]` | `01` | sender — the PCU (request had `[2] = 01`) |
| `[2]` | `FE` | recipient — the PC (request had `[1] = FE`) |
| `[3]` | `06` | **response / ACK** |
| `[4]` | `48` | 72 = 74 − 2 |
| `[5]` | `02` | COMMAND echoed (SAMPLES) |
| `[6]` | `01` | EXT_COMMAND echoed (SAMPLES_FORMAT) |

CRC16 over `[1 … n-4]` computes to `08F8`, matching the frame. Data is 64 bytes
(74 − 10) and decodes to plausible values throughout: flow 33.99 °C, return 30.59 °C,
outside 15.3 °C, calorifier 56.8 °C, state 8, lockout/blocking 255.

Two EEPROM-read replies from the same board, answering requests addressed to `0x00`:

```
0200FE06 18 101C 798506673F6D0C187E9E0228045104  D2A9 03   # block 0x1C, CRC 12A9
0200FE06 18 101D 3F350105008B00000000000000667C02  0646 03  # block 0x1D, CRC 4606
```

Both are 26 bytes with exactly 16 data bytes, `[3] = 06`, COMMAND `0x10` and the block
index echoed, addresses swapped, CRC good — so a `READ_EPROM_BLOCK` reply is a 26-byte
frame, and the 16-byte parameter-block assumption holds on the wire.

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

Byte `[2]` is `0x01` (PCU) in the sample request and `0x00` (PSU) in the counter
requests; both are answered, so the board does not enforce it on reads.

## Writing a parameter

`RemehaBoilerController.SetParameterModel` is, in full:

```csharp
if (EnableServiceMode(true, dest)) {
    ValidateDataModel(dataModel);                       // clamps to the XML min/max
    SetEepromData(dataModel.GetBufferByIndex(0), 0x14, 8, dest);
    EnableServiceMode(false, dest);
}
```

No service code is sent. `CreateServiceModeMessage` is a bare 10-byte frame with no
payload, and nothing in the write path touches `SERVICE_CODE` (`0x37`) — the 0012 PIN
Recom asks for is an application-level gate only.

`SetEepromData` writes each block as:

```csharp
for (int i = 0; i < blockCount; i++) {
    var m = factory.CreateEpromWriteMessage((byte)(startIndex + i), dest);
    Array.Copy(data, i * 16, m.Content, 7, 16);         // 16 payload bytes at offset 7
    factory.SetMessageCrc(m);
    var r = SendAndRecieveMessage(m, 1000);             // 1 s timeout
    if (r != null && !r.IsAcknowledged())               // NB: r == null falls through
        throw new CommunicationException();
}
```

So the sequence is:

1. `02 FE 01 05 08 08 0C AE CE 03` — service mode **on**, wait for ACK
2. `02 FE 01 05 18 11 <blk> <16 bytes> <CRClo> <CRChi> 03` — write block, wait for ACK
3. `02 FE 01 05 08 1F 0C A1 3E 03` — service mode **off**

The ACK for step 2 is an ordinary response frame carrying no data, so for a write to
block `0x16` sent as above it should read:

```
02 01 FE 06 08 11 16 <CRClo> <CRChi> 03
```

Recom rewrites all 8 blocks. Always **read the block first and modify one byte**,
since the other 15 bytes go back verbatim, and read it inside the write transaction
— a cached copy may be stale, and writing it back would silently revert 15
unrelated parameters.

> An earlier revision of this document added “nothing stops a single-block write”.
> **Hardware says otherwise** — see *A single-block write is ACKed and ignored*
> below. Recom writing all 8 blocks every time now looks like a requirement rather
> than laziness.

### Two bugs in Recom worth not copying

- **A timed-out write is treated as success.** The null check above falls through to
  the next block rather than throwing, and `Receiver.Receive` swallows the
  `"Response message is null."` exception that `ValidateResponse` raises. Treat no
  response as a failure.
- **A failed write leaves the boiler unlocked.** `EnableServiceMode(false, dest)` sits
  only in the success branch of `SetParameterModel`, with no `try`/`finally`, so a
  `CommunicationException` from `SetEepromData` propagates with service mode still on.
  An implementation should always re-lock, and should re-lock at boot as well — a
  reset mid-transaction otherwise leaves it on indefinitely. Sample byte 62
  (`service_mode`) reads the current state back.

### Cautions

- The parameter block holds the gas/air settings (p17–p21, p77, p78) and the
  controller-protection limits (p55–p57). A bad write here is a combustion-safety
  problem, not a comfort one. `ValidateDataModel` exists because Recom clamps every
  value to the XML `min`/`max` before sending — do the same.
- EEPROM endurance is finite. This is not somewhere to write on a schedule.
- A write this board declines is **not** signalled: it comes back as an ordinary,
  correctly formed ACK and simply does not happen. Never treat an ACK as proof that
  a value changed; read the block back and compare.

### A single-block write is ACKed and ignored

Observed on a live PCU-05 P3 on 2026-09-16, driving the sequence in this document
exactly. Every frame validated — CRC, length byte, swapped addresses and echoed
COMMAND/EXT_COMMAND, in both directions:

```
-> 02 FE 01 05 08 08 0C AE CE 03                                   CODE_SERVICE_START
<- 02 01 FE 06 08 08 0C AE 91 03                                   ACK
-> 02 FE 00 05 08 10 16 18 C5 03                                   READ_EPROM_BLOCK 0x16
<- 02 00 FE 06 18 10 16 04 00 01 01 ... FF  29 BC 03               p33 = 4
-> 02 FE 01 05 18 11 16 06 00 01 01 ... FF  36 D5 03               WRITE_EPROM_BLOCK 0x16, p33 = 6
<- 02 01 FE 06 08 11 16 24 CA 03                                   ACK
-> 02 FE 00 05 08 10 16 18 C5 03                                   READ_EPROM_BLOCK 0x16
<- 02 00 FE 06 18 10 16 04 00 01 01 ... FF  29 BC 03               p33 = 4, unchanged
-> 02 FE 01 05 08 1F 0C A1 3E 03                                   CODE_SERVICE_STOP
<- 02 01 FE 06 08 1F 0C A1 61 03                                   ACK
```

The write frame is byte-for-byte what `CreateEpromWriteMessage` builds, the ACK is
byte-for-byte what this document predicted, and the value did not change. So:

- `WRITE_EPROM_BLOCK` is understood at the protocol level and refused at the
  application level, silently.
- An **identity write** (writing a block back unchanged) cannot distinguish a board
  that applied the write from one that ignored it, so its “verified” result proves
  only that the frame was accepted. It remains a safe first test; it is just not an
  informative one.

Two candidate explanations, in order of suspicion:

1. **The full 0x14 — 0x1B sequence is required.** `SetParameterModel` never writes a
   single block: `SetEepromData(buffer, 0x14, 8, dest)` always sends all eight in one
   service-mode session. A partial parameter image may simply be dropped.
2. ~~**Service mode never actually engaged.**~~ **Ruled out** — see below.

### Service mode engages, and it is reported by byte 63, not byte 62

`Dietrich::test_service_mode()` reads a sample between the unlock and the re-lock.
Diffing that sample against one taken three seconds after the re-lock, on the same
board:

| Data byte | Inside the window | After the re-lock | |
|---|---|---|---|
| 0, 2, 8, 51 | — | — | flow / return / calorifier / control temp, drifting |
| **63** | **01** | **00** | tracks the service-mode window exactly |
| 62 | 00 | 00 | never set |

Nothing else in all 64 bytes differs. So `CODE_SERVICE_START` **does** take effect,
and the flag for it is **byte 63** — which this map labels `rs232_mode`, not byte
62, which it labels `service_mode`. Plausibly the board regards the service command
as putting it under RS232/PC control and the label is simply describing that; either
way, byte 63 is the observable to trust. The component reads byte 63 for its
service-mode check and for the boot re-lock, and logs both bytes.

> The `service_mode` sensor still publishes byte 62 and the `rs232_mode` sensor
> byte 63, so existing configurations keep their meaning. On this board it is
> `rs232_mode` that moves.

### The read address was not it either

One further difference from Recom had gone unnoticed: this component's parameter
reads were addressed to `0x00`, while Recom addresses everything on the write path
to `0x01`. A frame aimed at a different device in the middle of an unlock/write
sequence is a cheaper suspect than the block count, so it was eliminated first.
Inside a write transaction the component now uses the `0x01` read frames listed
under *Reading a parameter*; polling still uses `0x00`, which is known to work.

Retested on the same board with every frame addressed to `0x01` — unlock, read,
write, re-lock. The unlock was confirmed engaged (byte 63 = 1) and the reads came
back from `0x01` as expected. **The write was ACKed and p33 still did not move.**
So the destination address is not the cause, and the burner state is not either:
the attempts span both `state 8` (controlled stop) and `state 3` (burning CH).

### What the component does now

That leaves the block count as the only remaining difference from Recom, so
`write_param()` now does what `SetParameterModel` does — it writes the **whole**
parameter block in one service-mode session:

```
CODE_SERVICE_START
READ_EPROM_BLOCK  0x14 .. 0x1B      (8 frames)
WRITE_EPROM_BLOCK 0x14 .. 0x1B      (8 frames, 127 bytes verbatim + 1 edited)
READ_EPROM_BLOCK  0x14 .. 0x1B      (8 frames, to verify)
CODE_SERVICE_STOP
```

26 exchanges, around three seconds, borrowing one poll interval. `write_param()`
uses this; `write_block_unchanged()` stays deliberately single-block, as the
frame-level diagnostic it always was — on this board it is expected to be ignored.

Handing back 127 bytes verbatim is a real step up in risk from one block, because
the bytes include the gas/air settings and the controller-protection limits. Two
guards stand in front of it:

- **Every block must have been read by this transaction**, whole and CRC-valid.
  A missing block aborts to the re-lock without writing anything.
- **The image must be plausible.** Before any of it goes back, 58 documented
  parameters — deliberately including every one this component refuses to write —
  are checked against the ranges in the table below. A block that arrived mangled
  but with a valid CRC shows up as a parameter outside its range, and the whole
  transaction is refused with the offending byte named. All 58 fall inside their
  ranges on the live image above, so the check does not fire spuriously.

If the boiler still declines a full-block write, the next things to look at are
`SERVICE_CODE` (`0x37`, the 0012 PIN Recom asks for, which the IL says is never
sent) and `IDENTIFICATION` (`0x01`/`0x0B`), which Recom issues when it connects
and this component never does.

### What the component implements

Write support now exists in `components/dietrich/dietrich.cpp`, built to this
specification. It is gated behind `allow_writes` in the YAML and behind
`variant: pcu05_p3` — the 128 byte parameter map belongs to that parameter
set, so the same byte offset means something else on another board.

A write is a five step transaction on the component's existing request queue:

```
CODE_SERVICE_START -> READ_EPROM_BLOCK -> WRITE_EPROM_BLOCK -> READ_EPROM_BLOCK -> CODE_SERVICE_STOP
```

The block is read *inside* the transaction, because fifteen of the sixteen bytes
go back to the boiler verbatim and a copy from the hourly parameter sweep could
be stale. Neither Recom bug above is reproduced: any failed step — including a
failure of the unlock itself — skips straight to the re-lock instead of carrying
on, a missing response is a failure rather than a success, and a board found in
service mode at boot (sample byte 62) is re-locked. Values are clamped to the
ranges in the table below, the gas/air and controller-protection parameters are
not writable at all, and a write is skipped outright when the freshly read block
already holds the wanted value.

`tests/` drives all of that against a simulated PCU-05 P3 on a PC, with no
hardware and no ESPHome involved; `./tests/run.sh` builds and runs it.

Three entry points are callable from a YAML lambda, which is how a button in
Home Assistant reaches them — see the commented-out section at the bottom of
`dietrich_pcu05_p3_en.yaml`:

| Method | What it does |
|---|---|
| `test_service_mode()` | unlock and immediately re-lock, writing nothing |
| `write_block_unchanged(blk)` | read a block and write it back byte-for-byte |
| `write_param(p, v)` | read-modify-write one parameter, clamped to its range |

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

**Confirmed on hardware.** A full sweep of blocks `0x14`–`0x1B` off a live PCU-05 P3
reassembles to 128 bytes in which every documented parameter falls inside its
documented range once the `A x 100` / `A x 0.1` expressions are applied. The tank
settings read `p2 = 56 °C`, `p33 = 4 °C`, `p105 = 0 °C` and `p32 = 24 °C` — i.e. charge
to 56, cut in at 52, cut out at 56, and raise the flow setpoint to 80 while charging —
against a calorifier sensor reading 56.8 °C at the time. `p35 = 1` (*Solo (+boiler)*)
confirms the map's boiler-type decode, and with it that `p88`, `p96` and the DHW-in
sensor at sample byte 4 have no meaning on this installation.

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
3. Read its IL with `System.Reflection.Metadata` (in-box in the .NET SDK). The
   methods that matter are `RemehaMessage.InitializeMessage` / `IsAcknowledged` /
   `GetData`, `RemehaMessageFactory.Create*Message` / `GenerateAnswerMessage`,
   `RemehaReceiver.ValidateResponse`, and `RemehaBoilerController.SetParameterModel` /
   `SetEepromData` / `EnableServiceMode`. `DeviceIdentifier` lives next door in
   `RecomLibrary.Core.dll`.
