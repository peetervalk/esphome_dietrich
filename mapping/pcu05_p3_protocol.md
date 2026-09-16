# Remeha protocol — frames, commands and the EEPROM parameter block

Recovered from **Recom 7.3.8** (`RecomAppWix_7-3-8.msi` → `media1.cab` → `RecomAppExe`,
a .NET single-file bundle → `RecomProgram.Core.dll`), namespace
`RecomProgram.Communication.Remeha`: `RemehaMessage.InitializeMessage`,
`RemehaMessageFactory`, `RemehaBoilerController`.

Everything below is **verified** twice over: the rules here reproduce all three request
frames already hard-coded in `components/dietrich/dietrich.cpp` byte-for-byte, CRC
included, and the response rules were checked against a live PCU-05 P3 capture (see
*Response validation*).

> **Settled, 2026-09-16:** the 128-byte parameter image carries **two CRC16s of its
> own** — bytes 62–63 over bytes 0–61, bytes 126–127 over bytes 64–125 — and the PCU
> stores a set that fails them but never adopts it, which is what `Blocking 0`
> (*PCU parameter fault*) means. Recom recomputes them inside every write; this
> component did not, which is the whole of the fault. Jump to
> [*What the PCU actually checks: the parameter image CRC*](#what-the-pcu-actually-checks-the-parameter-image-crc).
> The evidence is a sniffer capture of Recom writing two parameters,
> `mapping/260916_2303.pcapng`, and it also disposes of the service-versus-factory-level
> theory this document argued for a fortnight: `COMMAND 0x09` and `EXT 0x52` are never
> sent, and the 0012 PIN never reaches the wire.

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
  an ACK is an ordinary response frame. A PCU-05 P3 also answers **`0x15`** — a NAK,
  an otherwise perfectly formed frame that means *understood and refused*. Recom
  lumps it in with every other non-ACK, but it is worth telling apart: silence means
  the frame did not land, a NAK means it landed on the right device and was turned
  down.
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
> against both — so the swap rule below holds regardless of which is used.
>
> Answering, however, is not the same as answering with the same bytes. On this
> board `0x00` and `0x01` have **different EEPROM contents**, and only `0x00` holds
> the parameter image the boiler runs on — see *The read address was it after all*.
> The whole EEPROM path is addressed to `0x00`; service mode stays at `0x01`, where
> it is observed to engage.

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

A `0x15` NAK satisfies every one of those checks except the first, which is why this
component reports it separately:

```
02 00 FE 15 08 1116 219F 03   # WRITE_EPROM_BLOCK 0x16 refused by 0x00
02 00 FE 15 08 1114 A05E 03   # WRITE_EPROM_BLOCK 0x14 refused by 0x00
```

Addresses swapped, length byte right, COMMAND and block index echoed, CRC good. The
board read the whole frame and said no.

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

Those two rows are what Recom's call sites give away. The whole 2 KB has since been
swept at both addresses and is mapped in
[`pcu05_p3_eeprom_map.md`](pcu05_p3_eeprom_map.md): identification group 1 at
`0x10`–`0x13`, the blocking and locking history rings at `0x20`–`0x2F` and
`0x30`–`0x3F`, a CRC16 on the counter block, and the fact that `0x1E`–`0x1F` is a
mirror of `0x1C`–`0x1D` rather than 32 more bytes of counters.

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
requests; both are answered, so the board does not enforce it on reads. The frames
above are kept for the record only — the component sends the `0x00` forms, because
the `0x01` ones do not return this boiler's parameters. See below.

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
payload, and nothing in the decompiled write path touches `SERVICE_CODE` (`0x37`).

> **Confirmed on the wire.** A 2026-09-16 capture of Recom writing two parameters shows
> exactly this sequence and nothing else — no PIN, no `0x37`, no factory command. What
> the decompile does not show, because it happens inside `ValidateDataModel`'s data
> model rather than in the message layer, is that the image being written carries **two
> CRC16s that Recom recomputes every time**. Miss those and the write is stored and
> ignored. See *What the PCU actually checks: the parameter image CRC*.

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

0. read all eight blocks, apply the edit, and **recompute the image's two CRC16s** —
   bytes 62–63 and 126–127; without this the rest of the sequence is pointless
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

### The read address was it after all

One further difference from Recom had gone unnoticed: this component's parameter
reads were addressed to `0x00`, while Recom addresses everything on the write path
to `0x01`. The component was changed to read from `0x01` inside a write
transaction, retested single-block against `0x16`, and the reads "came back from
`0x01` as expected" — so the address was written off as a dead end.

That test was single-block, and `0x16` is the one block for which it could not
fail. Reading all eight blocks from `0x01` settles it:

```
0201FE06 18 1014 FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF 2563 03   # and 0x15, 0x17..0x1B
0201FE06 18 1016 060001010000000200AF1E02FFFFFFFF 7811 03
```

Every frame is well-formed — length byte right, block index echoed, addresses
swapped, CRC good. **`0x01` answers sixteen `FF` bytes for seven of the eight
parameter blocks.** The same eight blocks read from `0x00` return the full,
plausible image this document records.

The one block that is not `FF` gives the game away. `0x01`'s block `0x16` reads
byte-for-byte identical to the captured image except at byte 32 — **p33, which
reads 6 there and 4 at `0x00`**. 6 is precisely what the earlier single-block
write attempts asked for. So those writes were not ignored at all:

- `0x00` and `0x01` are **two different EEPROM stores**. `0x00` holds the
  parameter set the boiler runs on; `0x01` is blank (`FF`) apart from what this
  component has written into it.
- `WRITE_EPROM_BLOCK` to `0x01` is accepted, ACKed, **and stored** — in a place
  nothing reads. That is the whole of "the write was ACKed and p33 did not move".
- The verify read that confirmed the write "took" was reading `0x01` back, so it
  agreed with itself.

Whatever Recom's `DeviceIdentifier` table calls these addresses, on this board the
parameter EEPROM is the one at `0x00`. The component addresses the whole EEPROM
path — read, write and verify — there, and keeps service mode at `0x01`, which is
where byte 63 is observed to move. The block count may still matter; it is simply
no longer the only difference left, and it was never the one doing the damage.

It was — with a NAK, first press. See *Service mode is per address* below.

### Service mode is per address

Reads from `0x00` came back complete and correct on the first attempt, so that much
was settled. The write to `0x00` was then **NAKed** — not ignored, not answered with
silence, but explicitly refused by a well-formed `0x15` frame.

The unlock was still going to `0x01`. That is where `CODE_SERVICE_START` is known to
work, because sample byte 63 tracks it; but the sample and the EEPROM are not the
same device, and nothing said they share a service-mode flag. They do not. The
component now unlocks **both** addresses at the start of a write transaction and
re-locks both at the end:

```
CODE_SERVICE_START -> 0x01      (the one visible in sample byte 63)
CODE_SERVICE_START -> 0x00      (the one the EEPROM answers on)
READ_EPROM_BLOCK   -> 0x00      0x14 .. 0x1B
WRITE_EPROM_BLOCK  -> 0x00      0x14 .. 0x1B
READ_EPROM_BLOCK   -> 0x00      0x14 .. 0x1B, to verify
CODE_SERVICE_STOP  -> 0x00
CODE_SERVICE_STOP  -> 0x01
```

Both re-locks are last in the queue and a failure part-way through jumps to the
first of them, so whatever was unlocked is locked again on every path out.

The `0x00` unlock frames, CRC computed the same way as the rest:

```
02 FE 00 05 08 08 0C 930E 03    # CODE_SERVICE_START -> 0x00
02 FE 00 05 08 1F 0C 9CFE 03    # CODE_SERVICE_STOP  -> 0x00
```

#### What it broke on the way

Until this was found, a transaction's reads were copied straight into `params_`,
the same buffer the parameter sensors publish from. Eight `FF` blocks therefore
arrived in Home Assistant as p1 = 255, p28 = 2550 % and so on, and would have been
the source bytes for the next read-modify-write had the sanity check not refused
the write first. A transaction now reads into a buffer of its own and touches
`params_` only after a write has verified.

### What the component does now

That leaves the block count as the only remaining difference from Recom, so
`write_param()` now does what `SetParameterModel` does — it writes the **whole**
parameter block in one service-mode session:

```
CODE_SERVICE_START  -> 0x01, then 0x00
READ_EPROM_BLOCK    0x14 .. 0x1B      (8 frames)
WRITE_EPROM_BLOCK   0x14 .. 0x1B      (8 frames, 127 bytes verbatim + 1 edited)
READ_EPROM_BLOCK    0x14 .. 0x1B      (8 frames, to verify)
CODE_SERVICE_STOP   -> 0x00, then 0x01
```

28 exchanges, around three seconds, borrowing one poll interval. `write_param()`
uses this; `write_block_unchanged()` stays deliberately single-block, as the
frame-level diagnostic it always was.

Handing back 127 bytes verbatim is a real step up in risk from one block, because
the bytes include the gas/air settings and the controller-protection limits. Three
guards stand in front of it:

- **Every block must have been read by this transaction**, whole and CRC-valid.
  A missing block aborts to the re-lock without writing anything.
- **The image must be plausible.** 58 documented parameters — deliberately
  including every one this component refuses to write — are checked against the
  ranges in the table below. Each block is checked the moment it arrives, so a
  transaction gives up on the first bad block instead of reading all eight and
  naming forty-five bad bytes in one pass of `loop()`; the check runs again over
  the whole image as the last gate before any byte goes out. A block that arrived
  mangled but with a valid CRC, or an `FF` block from the wrong device, shows up
  as a parameter outside its range and the transaction is refused with the
  offending byte named. All 58 fall inside their ranges on the live image above,
  so the check does not fire spuriously.
- **A transaction's reads never reach the sensors.** They land in a buffer of the
  transaction's own; `params_`, which the parameter sensors publish from, is
  updated only once a write has verified. A refused transaction leaves Home
  Assistant showing what the last good sweep read.

If the boiler still declines a full-block write, the next things to look at are
`SERVICE_CODE` (`0x37`, the 0012 PIN Recom asks for, which the IL says is never
sent) and `IDENTIFICATION` (`0x01`/`0x0B`), which Recom issues when it connects
and this component never does.

### The full-block write lands, and the PCU blocks on it

2026-09-16 14:23, the first press that wrote where the boiler reads. It worked, and
the boiler did not like it.

The transaction is in the log byte for byte. Block `0x16` was read as
`04 00 01 01 …`, written as `06 00 01 01 …`, and read back as `06 00 01 01 …`; the
other seven blocks went out identical to the bytes they came in as. p33 = 6 is in
the EEPROM at `0x00`. Fourteen seconds later the next sample read:

| Byte | Field | Value | Meaning |
|---|---|---|---|
| 40 | STATUS | 9 | Blocking mode |
| 41 | LOCKING | 255 | none |
| 42 | BLOCKING | **0** | **PCU parameter fault** |
| 43 | SUBSTATUS | 60 | Pump post running |

Burner off, fan at 0, ionisation 0, pump at 30 %, calorifier 65.8 C - it had been
charging the tank, shut down, and gone into blocking. Byte 63 was 0, so the `0x01`
re-lock had taken. Only one full sample survives in that log, so this does not
strictly prove the board was not already blocking before 14:23:14; the write is
simply the obvious suspect, fourteen seconds upstream.

What the PCU objected to is not the bytes. The image that went back differs from
the one the boiler had been running on by exactly one byte, and the component's own
sanity check - 58 parameters against the ranges below - passed on every block as it
was read, so nothing implausible was handed back. The whole image as read is kept
in `pcu05_p3_live_parameters.md`. So the fault is not *what* changed but that
something changed without whatever else the PCU expects to see change with it.

The obvious candidate was a checksum. Bytes 124..127 of the image are
`FF FF 85 9A`, past the last documented parameter (p124 at byte 123), and `85 9A`
has the shape of a 16-bit trailer. It is not one, or not by any usual reckoning:
CRC-16 with polynomials `A001`, `8408`, `8005`, `1021` and `3D65`, both bit orders,
inits `0000` and `FFFF`, plus 16-bit sums, over **every** contiguous range of the
128 bytes, produces `859A`/`9A85` only from ranges like `[5,127)` and `[63,89)` -
coincidences, not a checksum anybody would store. If the PCU keeps a consistency
value for the parameter set, it is not in those four bytes.

That leaves the two things Recom does around a write that this component still does
not: `IDENTIFICATION` (`0x01`/`0x0B`) on connect, and `SERVICE_CODE` (`0x37`). It
also leaves the possibility that a parameter write is only legitimate with the
boiler in standby rather than mid-DHW-charge. Untested, both.

Blocking is not locking: a blocking code clears when its cause does.

#### What cleared it

The recovery is worth recording in full, because the two writes provoked *different*
blocking codes and only one thing ended it.

| Time | Event | Blocking after |
|---|---|---|
| 14:19 | — | `No blocking` |
| 14:23:14 | full-block write, `p33 4 -> 6`, boiler in `8:Controlled stop` / `0:Standby` | **0**, PCU parameter fault, 14 s later |
| ~15:0x | mains power cycle | **0**, unchanged |
| 16:38:28 | full-block write, `p33 6 -> 4`, boiler already in blocking mode, substatus 0 | **20**, Identification running, 17 s later |
| 16:40:49 | `IDENTIFICATION` read, both addresses | **20**, unchanged |
| 17:00, 17:01 | ESP reboots (firmware flashes) | **20**, unchanged |
| ~17:45 | mains power cycle | **`No blocking`** at 17:46:15 |

What that settles, and what it does not:

- ~~**A full-block write provokes a blocking on this board whatever the value is.**~~
  **Wrong, and superseded twice over.** The intermediate explanation — that these writes
  were made at *service* level and a *factory*-level write would not block — was wrong
  too. What the PCU objects to is an image whose CRC does not match it; see
  *What the PCU actually checks: the parameter image CRC*.
- **A power cycle is not reliably enough.** The first one, with `p33 = 6` in the EEPROM,
  changed nothing at all. The second, after `p33 = 4` was back, cleared it. Two things
  differ between them — the value, and the fact that a second write had happened — so
  this does not isolate which mattered.
- **`Blocking 20` is not cleared by an `IDENTIFICATION` read.** The board sat in
  *Identification running* for 68 minutes while answering identification reads normally,
  and rode out two resets of the client. Whatever it is waiting for, that command is not
  it. `AUTO_DETECT` (`0x33`) and `RESET` (`0x31`) were the two untried candidates;
  `PCU-05_P3.xml` gives both a 5 second settle time (`command.auto.detect.time`,
  `command.df.du.time`). `RESET` has since been sent to a *healthy* board, which
  ACKed it and carried on unchanged — see *`RESET` is ACKed and does nothing
  observable*. Whether it clears a blocking is still unknown.
- **The image itself is accepted.** The boiler has run CH and DHW normally on it since
  17:46, and the second EEPROM sweep shows nothing anywhere in its 2 KB was brought into
  agreement with the write — see `pcu05_p3_eeprom_map.md`, *The second sweep*. The
  stale-checksum theory is dead, and with it the idea that the PCU keeps parameter
  integrity data the write failed to update.

~~The one difference between the two writes that is still unaccounted for is the
boiler's own state: the first went out while it was finishing a DHW charge, the second
while it was stopped.~~ **Also wrong, and it came from misreading a consequence as a
cause.** Home Assistant's recorder has the boiler in `8:Controlled stop` / `0:Standby`
continuously from 14:19:58, and `9:Blocking mode` *and* `60:Pump post running` arriving
together in the same state change at 14:23:28. Substatus 60 is the boiler shutting down
**because** it blocked. The same is true of the second episode at 19:15:16. Both
`p33 = 6` writes went out to a quiet boiler.

`boiler_is_quiet_()` therefore cannot prevent this and never could: the gate passed on
both occasions, correctly. It is kept because writing 128 bytes of live control settings
into a burning boiler is a bad idea on its own merits, not because it addresses this
fault.

### What the PCU actually checks: the parameter image CRC

**This is where the investigation ends.** On 2026-09-16 Recom 7.3.50 made two parameter
writes to this appliance with a USB sniffer on the FTDI service cable. The capture is
`mapping/260916_2303.pcapng`; `tools/decode_capture.py` turns it into annotated frames.
It answers every open question at once, and most of what this section used to argue —
that the difference was *service* versus *factory* level — was wrong.

#### The image carries two CRC16s of its own

| Image bytes | Cover | Where they live |
|---|---|---|
| 62–63 | bytes 0–61 | block `0x17`, offsets 14–15 |
| 126–127 | bytes 64–125 | block `0x1B`, offsets 14–15 |

Same CRC as everywhere else on this board — poly `0xA001`, init `0xFFFF`, stored LSB
first. On the image this boiler was running when the capture starts:

```
crc16(image[0:62])   = 0x058F     stored at 62,63 as  8f 05
crc16(image[0:64])   = 0x0000     <- data + CRC checks to zero
crc16(image[64:126]) = 0x9A85     stored at 126,127 as 85 9a
crc16(image[64:128]) = 0x0000
```

Recom recomputes them inside every write, which is why block `0x17` goes back *changed*
in both sessions although no parameter in it moved:

| | parameter written | bytes 62,63 |
|---|---|---|
| session 1, t=124 s | p33 `4 → 5` (block `0x16`) | `8f 05` → `8e 55` |
| session 2, t=208 s | p2 `56 → 55` (block `0x14`) | `8e 55` → `9a 50` |

Both recompute correctly from the edited image, and the zero-check holds after each.
Two independent writes, two different blocks, one rule.

#### That is what `Blocking 0` was

The PCU stores whatever is written to it. What it will not do is *adopt* a set that
fails its own CRC: at the next identification it finds the store inconsistent with what
it is running, keeps running the old set and raises `Blocking 0`, *PCU parameter fault*
— which is exactly what the code is named for.

Every observation that made no sense under the level theory falls out of this one:

- **The value never mattered.** Any change to a covered byte without a CRC update does
  it. `p33 = 6` was inside its documented `2..15` all along.
- **Writing the old value back "fixed" it** because it restored agreement with the CRC
  that was never updated — not because the value was better.
- **Five resets with no write at all still fell back to `Blocking 0`** (2026-09-16,
  19:40–19:48). Each reset ran an identification, judged the stored set against the CRC,
  and failed it again. Nothing about *how* the bytes got there is involved.
- **Recom's writes never blocked** because Recom's images always check out.

#### The EEPROM dumps taken at the time say the same thing

This does not rest on the capture alone. Three full 2 KB sweeps were taken on
2026-09-16, before any of this was suspected, and the CRC state in them tracks the
boiler's condition exactly:

| Dump | p33 | Low-half CRC (bytes 62–63) | Boiler |
|---|---|---|---|
| `eeprom_dump_260916.txt`, 15:49 | 6 | stored `058F`, computes `A58D` — **mismatch** | in `Blocking 0` |
| `eeprom_dump_260916_2.txt`, 18:11 | 4 | stored `058F`, computes `058F` — match | recovered |
| `eeprom_dump_260916_3.txt`, 20:52 | 4 | stored `058F`, computes `058F` — match | recovered |

The dump made while the boiler was blocked is the one whose stored image fails its own
CRC, and the stored value `058F` is the CRC of the image *with `p33 = 4`* — the CRC for
the set the PCU was still running. Writing `4` back "fixed" it by restoring agreement
with a CRC that no write had ever touched. The high half matched in all three, as it
would: nothing had written to it.

> Address `0x01`'s parameter blocks fail both CRCs in every dump. That store is
> sixteen `FF` bytes per block wherever nothing has been written to it — it is not an
> image, and its CRC is not meaningful. Only `0x00` holds the set the boiler runs on.

#### What is still inferred: the upper half

Bytes 126–127 have never been seen being *maintained* by anything — both of Recom's
edits were in the low half. That the upper half is protected the same way is not in
doubt (the stored CRC checks out on every dump and every read), but no writer has been
observed updating it, and on this appliance none can be. What Recom exposes here is:

| Level | Parameters |
|---|---|
| user | p1–p5 |
| service | p17–p44, **except p22** |
| anything above p44 | not shown at all |

and its *Communication → EEPROM* item — the raw block editor, which would reach the
whole 2 KB — is greyed out. So Recom cannot be driven into the upper half at all.

That p17–p44 is one screen is worth noting on its own: p33 and p2 came from different
menu levels and produced identical frames, so the levels are Recom's, not the board's.

Four parameters on this component's writable list live there — p73, p85, p88 and p105 —
and a write to any of them exercises the inferred rule. Writing one and watching for
`Blocking 0` is the only experiment that can settle it.

#### There is no factory level on the wire

`COMMAND 0x09` (`CODE_FACTORY_COMMANDO`) and `EXT 0x52` (`CODE_FACTORY`) **do not appear
anywhere in the capture.** Recom's write sequence is:

```
123.603  PC -> PSU  CODE_SERVICE_START  0x08 / 0x0C
123.876  PC -> PSU  WRITE_EPROM_BLOCK   blk 0x14        ~200 ms apart
  …                                     blk 0x15 … 0x1B
125.573  PC -> PSU  CODE_SERVICE_STOP   0x1F / 0x0C
```

Plain service level, all eight blocks, addressed to `0x00` — byte-for-byte what this
component already sent. Two details worth copying: Recom unlocks **only** the EEPROM
address (`0x00`), not `0x01` as well, and it allows ~200 ms per write ACK.

The owner wrote **p2 from Recom's user-level screen and p33 from the level above it**,
and both produced the identical wire sequence. The levels are Recom's own UI gating;
the board is told nothing about them.

> The table that used to stand here — service `0x08`/`0x0C` versus factory
> `0x09`/`0x52`, with the pairing "inferred from the symmetry" — described something
> that has never been sent by Recom and has no observed effect. `use_factory_mode` on
> the component is kept only as a way to put the question to a board; nothing depends
> on it.

#### `SERVICE_CODE` carries no PIN

The 0012 PIN was typed once in this session, **after** both writes, and it produced
exactly one frame:

```
02 FE 01 05 0A 37 0B 0000 39E1 03     PC  -> PCU   SERVICE_CODE / IDENTIFICATION, data 00 00
02 01 FE 06 0A 37 0B 0101 B989 03     PCU -> PC    ACK,                           data 01 01
```

The PIN itself is nowhere in those bytes, in any encoding, and no write followed it.
Whatever `0x37` is — a level query is the obvious reading of `00 00` → `01 01` — it is
not an unlock, and the writes did not need it.

#### What the owner did with Recom

Before any of this: p1, p2, p25, p26, p27 and p28 written with Recom, cleanly, no
blocking, the heating curve recalculating around the new footpoint as it went. That
remains the fixed point the whole investigation was measured against, and the CRC is
what it was showing.

#### What was ruled out along the way

Worth recording so it is not re-tried:

- **It is not the parameter group.** p25–p28 are group 2, the same group as p33.
- **It is not the value, the address, the block count or the boiler's state.**
- **It is not a PSU disagreement.** The PCU has a dedicated code for that,
  `Blocking 18` *Ident. PSU mismatch*, and it never fired.
- **"Parameter CRC fault" is not a PCU-05 string.** It is `language.xml` id 3624, in
  the Avanta/MCBA auto-detect family. The PCU-05 tables are at 2300–2334 (blocking) and
  2400–2412 (locking). The PCU-05 says `Blocking 0` instead.

> An earlier revision concluded **"there is no parameter checksum anywhere in either
> 2 KB image"**. That search covered whole-image ranges and the counter block; it never
> tried the two 62-byte halves, which is where the CRCs are. The rest of that bullet
> still holds — the CRC of the *whole* image appears nowhere, and the two apparent hits
> inside the locking ring are one operating-hours counter seen twice.

#### What Home Assistant's recorder added

The ESP log covers 19:50 onwards only; the recorder has the rest, and three things came
from it alone: the five reset-and-fall-back cycles between 19:40 and 19:48, the boiler
being in standby at the instant of both `p33 = 6` writes, and the attribution of each
write to the button press that caused it. `button.*` entities keep their last-press
timestamp as state, so the recorder's history of them is a log of every press. Local
time is UTC+3.

### dF/dU is not in the parameter block

Worth settling, because a full-block write that rewrote the combustion
identification data would be a different order of problem from one that rewrote
comfort settings. It does not. `PCU-05_P3.xml` keeps them in three separate
`configurations` nodes:

| Node | Fields | Bytes | Carried by |
|---|---|---|---|
| `parameter` | 98 | 0..123 | `READ_EPROM_BLOCK` / `WRITE_EPROM_BLOCK`, blocks `0x14`..`0x1B` |
| `identification` | 49 | 0..48 | `IDENTIFICATION` (`0x01` / EXT `0x0B`), read-only |
| `df.du` | 2 | 0..1 | `SET_DFDU` (`0x32`) |

Not one of the 98 `parameter` fields is labelled `DF` or `DU`. The codes live in the
identification payload - `dF-code` at byte 1, `dU-code` at byte 2, then
`SW_VERSION` at 5, `PARAM_VERSION` at 6 and `PARAM_TYPE` at 7 - and are set with a
command of their own that has nothing to do with the EEPROM block path. So the
eight blocks written on 2026-09-16 could not have disturbed them, which agrees with
the board reporting `Blocking 0` (*PCU parameter fault*) rather than `Blocking 17`
(*Ident. dF/dU table error*) or `Blocking 19` (*Ident. dF/dU needed*).

dF/dU is also this family's factory-restore mechanism - the role CN1/CN2 plays on
other boilers. From `language.xml`: *"Do you want to use this dF/dU code to restore
the factory settings?"*, *"Are you sure that the dF/dU codes entered match the
identification plate?"* and *"Note! You must not enter any dF/dU code that is not
indicated on the identification plate!"*. Both codes can be read back off the board
without writing anything, since `IDENTIFICATION` is a plain read - a way to check
what the PCU thinks it is against what the plate says, before any restore.

One thing that is *not* an EEPROM checksum: `<checksum value="7129" />`, the last
element of `PCU-05_P3.xml`. That is Recom's integrity value for the map file.

#### Reading it

`IDENTIFICATION` is a bare 10 byte request, COMMAND `0x01` with EXT `0x0B`. Both
device addresses answer it:

```
02 FE 01 05 08 01 0B E9 5C 03   # IDENTIFICATION -> 0x01
02 FE 00 05 08 01 0B D4 9C 03   # IDENTIFICATION -> 0x00
```

The `identification` node holds **four** layouts, and which one a reply carries has
to be worked out from its length, because the map does not say which device serves
which group. Groups 2, 3 and 4 are one device's own identity; group 1 is the
appliance's, and it is the only one with the dF/dU codes on it.

| Byte | Groups 2-4 (16 bytes) | Group 1 (64 bytes) |
|---|---|---|
| 0 | Device type | - |
| 1 | Software version | **dF-code** |
| 2 | Parameter version | **dU-code** |
| 3 | Parameter type | - |
| 4..5 | Operating hours, `(A.1 + B.0) x N` | - |
| 5 | - | Software version |
| 6 | Connected SU type (g2) / PCU type (g3) | Parameter version |
| 7 | Connected PSU type | Parameter type |
| 8 | Last blocking code | - |
| 9 | Last locking code | - |
| 10 | Last internal error (g3 only) | Next service code |
| 11..15 | Serial number, `number="5"` | - |
| 16..18 | - | Connected PSU type, connected PCU type, SCU-C |
| 19..31 | - | SU no., SCU-S no. |
| 32..47 | - | Serial number, 16 characters |
| 48..63 | - | Boiler name, 16 characters |

`N` in the operating-hours expression is the one thing separating group 2 from
group 3: `x 2` for the PCU, `x 8` for the SU and PSU. The pair is big-endian, the
same way the counter blocks read.

##### What the board actually answers

`0x01` answers with **16 bytes, not 64** - groups 2-4's shape, not the group 1 the
dF/dU field names might lead you to expect. Captured 2026-09-16 15:28:

```
0201FE06 18 010B 0517FF036E8C01040124FFFFFFFFFFFF 5EFC 03
```

| Field | Value |
|---|---|
| Device type | 5 |
| Software version | 23 (`0x17`) |
| Parameter version | **255** (`0xFF`) |
| Parameter type | 3 |
| Operating hours | `0x6E8C` x 2 = 56 600 |
| Connected SU type | 1 |
| Connected PSU type | 4 |
| Last blocking code | 1 - *T Flow > max.* |
| Last locking code | 36 - *5x Flame loss* |
| Serial number | `FF FF FF FF FF`, unset |

Two cross-checks that this is group 2 and not something else: byte 0 is defined
only in groups 2-4, and 56 600 operating hours sits plausibly between the same
board's *hours run pump* (62 222) and *power supply available* (64 832) counters,
which it would not under group 3's `x 8`.

`0x00` answered nothing at all on that capture. **It does now.** At 15:49:45 the same
request to `0x00` came back with 64 data bytes — group 1, the appliance identity:

```
0200FE06 48 010B 0D1302FFFF17FF037C0200FFFFFFFFFF0405FF01FFFFFFFF0BFFFFFFFFFFFFFF
                 31383332373230313033383430202020547A65727261204578706F7274202020 4FD2 03
```

| Field | Value |
|---|---|
| dF-code | **19** |
| dU-code | **2** |
| Software version / parameter version / parameter type | 23 / 255 / 3 |
| Next service code | 0 |
| Connected PSU / PCU / SCU-C | 4 / 5 / 255 |
| Serial number | `1832720103840` |
| Boiler name | `Tzerra Export` |

So the dF/dU codes *are* readable over the wire and the identification plate is no
longer the only source for them — which matters, because reloading the factory
parameter set after a `Blocking 0` is keyed on exactly those two numbers.
`CODE_FACTORY_COMMANDO` never had to be tried.

Those 64 bytes are EEPROM blocks `0x10`–`0x13` at `0x00` concatenated, byte for byte,
and the 16 bytes `0x01` answers with are block `01:00` at that address. Identification
is a read of a known EEPROM range and nothing more. See
[`pcu05_p3_eeprom_map.md`](pcu05_p3_eeprom_map.md).

*Parameter version reads `0xFF`.* Worth writing down next to a `Blocking 0` (*PCU
parameter fault*), and worth not reading too much into: there is no capture from
before the write to compare it with, and an unset serial number in the same reply
suggests `0xFF` is simply what this board stores for fields it does not fill in.

The component sends both requests once on the first poll after boot, as Recom does
on connect, and `read_identification()` asks again. Read-only, and therefore not
gated behind `allow_writes`.

### The rest of the EEPROM, and where group 1 has to be

> **Answered.** Both addresses were swept end to end on 2026-09-16 and the findings
> are in [`pcu05_p3_eeprom_map.md`](pcu05_p3_eeprom_map.md). Group 1 is blocks
> `0x10`–`0x13` at `0x00`, the blocking history is `0x20`–`0x2F` and the locking
> history `0x30`–`0x3F`, both rings of 16 records in exactly the layout predicted
> below. What follows is the reasoning that went looking for them.

`EEPROMSize` in `PCU-05_P3.xml` is 2048 bytes, so the EEPROM is **128 blocks**,
`0x00`..`0x7F`. Twelve of them are mapped - `0x14`..`0x1B` parameters, `0x1C`..`0x1F`
counters - and the other 116 have never been read. Two things are known to exist
and have no address yet, and both have to be in there:

- **Group 1 of the `identification` node**: the dF/dU codes, the 16 character
  serial number and the boiler name. `0x01` answers `IDENTIFICATION` with group 2
  instead and `0x00` answers nothing, so no command is known to serve it.
- **The fault history records.** The `error` (blocking) and `failure` (locking)
  records in the field map are **16 bytes** each - exactly one EEPROM block, which
  is a strong hint about how they are stored:

| Byte | Blocking record | Locking record |
|---|---|---|
| 0 | Error code (`error.code`) | Error code (`failure.code`) |
| 1 | Number - how many times | as blocking |
| 2..3 | Operating hours, `(A.1 + B.0) x 2` | as blocking |
| 4 | State (`status.code`) | as blocking |
| 5 | Sub-State (`substatus.code`) | as blocking |
| 6..9 | Flow, return, calorifier, outside temp, `sgn(A)` | as blocking |
| 10 | Internal setpoint | as blocking |
| 11 | Ionisation current, `A x 0.1` | as blocking |
| 12..13 | Airflow, `A.1 + B.0` | as blocking |
| 15 | Actual power, % | as blocking |

That operating-hours stamp is the interesting column. The PCU reports 56 600
operating hours in its identification reply, so a blocking record carrying a
`0` error code and a stamp near 56 600 would date the `Blocking 0` to the hour -
and either place it at the 2026-09-16 write or clear the write of it. The state,
sub-state and temperatures at the time come with it.

The sweep found the records and they decode in exactly this layout — but **no slot
in either ring carries code 0**, and the newest blocking is stamped 382 hours before
the sweep. A *PCU parameter fault* never enters the blocking history, so the history
cannot date the write either way. The stamp turned out to run on the *Power supply
available hrs* counter (64 832), not the 56 600 the identification reports.

#### Last blocking code

The identification reply's byte 8 is *Last blocking code* and read **1**
(*T Flow > max.*) while the sample reported the current blocking as **0**. So the
field is not a copy of the live status byte. Either it lags, or it records only
faults that have cleared, or parameter faults do not go into it at all - the map
does not say, and one reading cannot tell them apart. The history records settled
it: **the field is the newest entry of the blocking ring** (code 1 at block `0x2B`,
stamped 382 hours earlier), and the third reading was the right one - a parameter
fault is never written to the ring, so it can never reach this field either. The
companion *Last locking code* read 36 (*5x Flame loss*),
which suits a board with 139 flame losses and 261 failed starts on its counters.

#### Sweeping it

`dump_eeprom(addr, first, count)` walks a range of blocks and logs each one as hex
and ASCII. `READ_EPROM_BLOCK` needs no service mode, so the sweep unlocks nothing
and writes nothing; a block that answers nothing is logged and stepped over, since
which blocks a device declines is itself part of what the sweep is for. It is one
request re-armed rather than a queue - 128 blocks would not fit in one - and it
borrows the poll intervals it needs, about 45 seconds for the full 2 KB.

Both addresses are worth sweeping: they answer `READ_EPROM_BLOCK` with different
contents, as `0x14`..`0x1B` already showed.

Done on 2026-09-16, both addresses, all 128 blocks each, in
`mapping/eeprom_dump_260916.txt`. Every request was answered - no block is declined
by either device, so an `FF` block is empty rather than refused - and 62 blocks at
`0x00` and 11 at `0x01` hold data. The decode is
[`pcu05_p3_eeprom_map.md`](pcu05_p3_eeprom_map.md).

### A re-lock that goes unanswered is not a failed write

The same transaction reported `parameter write failed` even though the write above
had already verified. Two mistakes, both since fixed:

- The step that actually timed out was `CODE_SERVICE_STOP -> 0x00`, and the log
  called it `sample`, because the label switch in `handle_response_()` had no case
  for either `_EE` request and fell through to the default.
- `finish_txn_()` tested `txn_failed_` before `txn_wrote_`, so a stumble on a step
  that runs *after* the write and its read-back discarded the verify, printed a
  failure, and left `params_` holding the pre-write value - which is why Home
  Assistant went on showing p33 = 4 with 6 in the EEPROM.

A re-lock failure is now tracked apart from the rest of the transaction and
reported on its own, after the verdict on the write. It is also the one step that
gets resent: `CODE_SERVICE_STOP` carries no payload and re-locking an already
locked address is a no-op, so a repeat costs nothing, while walking away from an
unanswered one leaves the boiler unlocked. Reads and writes are still never
retried.

### What the component implements

> Since 2026-09-16 a parameter write also **recomputes the image's two CRC16s** before
> the blocks go out, which is what makes the PCU adopt the result instead of storing it
> and raising `Blocking 0`. See *What the PCU actually checks: the parameter image CRC*.

Write support now exists in `components/dietrich/dietrich.cpp`, built to this
specification. It is gated behind `allow_writes` in the YAML and behind
`variant: pcu05_p3` — the 128 byte parameter map belongs to that parameter
set, so the same byte offset means something else on another board.

A write is a seven step transaction on the component's existing request queue:

```
SAMPLES -> CODE_SERVICE_START x2 -> READ_EPROM_BLOCK -> WRITE_EPROM_BLOCK
        -> READ_EPROM_BLOCK -> CODE_SERVICE_STOP x2 -> SAMPLES
```

Between the read and the write the image is edited **and both CRC16s are recomputed**
(`apply_param_crcs_`), so what goes back is a set the PCU will adopt. A parameter write
that does not span all eight blocks is refused outright, because the CRCs cannot be
recomputed from a partial image. An image that *arrives* failing its own CRC — the state
every pre-2026-09-16 write of this component's left behind — is reported and then
repaired by the write going out.

`use_factory_mode: true` swaps the two unlocks and re-locks for `CODE_FACTORY_COMMANDO`
(`0x09`/`0x52`). The capture shows Recom never sending either, so this is now only a way
to ask a board what it makes of the command; nothing depends on it and it stays off.

The two `SAMPLES` are what *What cleared it* above cost:

- **The first is a pre-flight.** The write goes out only when that sample shows a
  boiler that is not burning and not part-way through a cycle: status in
  {0, 8, 9, 10}, sub-status in {0, 1, 255}, fan stopped, no ionisation current.
  Anything else and the transaction stops there, having unlocked nothing. A board
  already in blocking or locking mode passes deliberately — that is the state you
  need to write to it in to undo a bad value. The check is made on a sample taken
  inside the transaction rather than on the last poll's, which can be 15 seconds
  old, and 15 seconds is long enough for a burner to start.
- **The last is a post-mortem.** The blocking code from the pre-flight sample is
  compared with the one after the re-lock, and any change is logged as an error
  even when the write verified byte-for-byte — because that is exactly what
  happened on 2026-09-16, twice.

Each `WRITE_EPROM_BLOCK` is also followed by 250 ms rather than the 60 ms between
reads. Eight EEPROM program cycles inside half a second is this component's own
idea of a reasonable pace; Recom allows a full second per write ACK, with a human
in a dialog box on top of that.

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
| `reset_board()` | `COMMAND 0x31` on its own — see below |

`reset_board()` sends `RESET` to the PCU, unlocking nothing and touching no EEPROM.
It is deliberately not part of `write_param()`: the only reason to want it is the
blocking a write leaves behind, and a mains power cycle clears that anyway.

#### `RESET` is ACKed and does nothing observable

Pressed on 2026-09-16 at 19:05:29 on a healthy board, out of curiosity rather than
need. The PCU answered in 119 ms:

```
02 FE 01 05 08 31 00 BC9B 03   # request:  RESET, EXT NONE
02 01 FE 06 08 31 00 BCC4 03   # response: ACK, COMMAND and EXT echoed, no data
```

Ten bytes, `[3] = 06`, zero data bytes — the same bare-echo ACK shape as
`CODE_SERVICE_START`, which means the frame was understood and says nothing about
what was done with it. What the logged sensors say happened afterwards: nothing.

| Time | State | Sub-status | Blocking |
|---|---|---|---|
| 19:03:15 | 8 Controlled stop | 1 Anti-cycling | No blocking |
| **19:05:29** | **`RESET` sent and ACKed** | | |
| 19:09 | 8 Controlled stop | 1 Anti-cycling | No blocking |

So `0x31` is a command this board accepts, and whatever it does, it is **not** a
restart: the mains power cycle at 17:46 the same afternoon announced itself as
*17 De-airation* in the very next sample, and this produced no state change at
all — the anti-cycle period that began at 19:01 ran out on its own schedule.
Nothing was cleared because nothing needed clearing.

That leaves the experiment worth doing still undone: `RESET` on a board that is
actually in blocking mode. It costs a deliberate re-block to set up, so it waits
for the next accidental one. Note also that the component sends the one frame and
reports immediately, taking no sample of its own — `PCU-05_P3.xml` allows a 5
second settle time for its neighbouring commands, and any effect would surface on
the next poll, not in the transaction's own log lines.

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

### Capturing Recom on the wire

The decompile was wrong about the PIN and silent about the CRC, so the wire is the
authority. The service cable is an FTDI FT232R, which USBPcap and Wireshark's `ftdi-ft`
dissector handle directly — no virtual COM port and no hardware tap needed:

1. Start the USBPcap capture on the root hub the adapter sits on, **before** Recom opens
   the port, so the connect handshake is in it.
2. Disconnect the ESP's TX from the boiler first. Its 15-second poll will otherwise
   collide with Recom mid-write.
3. Drive Recom: connect, change one parameter, change it back.
4. `py tools/decode_capture.py capture.pcapng --out trace.txt`.

`tools/decode_capture.py` reassembles both directions, verifies every CRC, names
commands and EXT codes from the tables above, diffs a `WRITE_EPROM_BLOCK` against the
block last read so a write reads as *"p33 Hystereses calorif.: 4 -> 5"*, and ends with
everything unknown, factory-flavoured or PIN-related collected in one place.

Recom 7.3.50 will not display a boiler whose map file it cannot find — it reports
*"Unknown boiler … data will be stored, but can not be displayed"* and then crashes on
an empty model list. It downloads those maps from a server that no longer answers. The
fix is to drop `mapping/PCU-05_P3.xml` into
`%APPDATA%\Remeha bv\Recom (PCST)\config\overrides\`; the file in this repo is
byte-identical to the one Recom 7.3.8 carries inside its own bundle. The log at
`%APPDATA%\Remeha bv\Recom (PCST)\logs\pcservicetool.log` names the exact path it
looked in.
