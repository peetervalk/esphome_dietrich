# The PCU-05 P3 EEPROM, swept end to end

Both device addresses, all 128 blocks each, 2026-09-16 15:49–15:51. The raw log is
`mapping/eeprom_dump_260916.txt`; this file is what it decodes to.

The sweep is the two diagnostic buttons in `dietrich_pcu05_p3_en.yaml`
(`dump_eeprom(0x00, 0x00, 128)` and the same for `0x01`). `READ_EPROM_BLOCK` needs
no service mode, so nothing was unlocked and nothing was written. Every one of the
256 requests was answered — no block is declined by either address, so a block that
reads `FF` is empty, not refused.

Of the 128 blocks, **62 hold data at `0x00` and 11 at `0x01`**. Everything the
`pcu05_p3_protocol.md` EEPROM map listed as never read is below.

## Address 0x00

| Blocks | Contents | Certainty |
|---|---|---|
| `0x00`–`0x02` | 48-byte record, unidentified; its first 37 bytes repeat at `0x04`:11 | raw only |
| `0x03`, `0x04`:0–10 | scattered small signed bytes (`F7 F5 FA FA EB FD F5`, `FE FB DA FE`) | raw only |
| `0x04`:11–`0x06` | shadow copy of the `0x00` record | verified byte-for-byte |
| `0x07`–`0x0F` | empty | |
| `0x10`–`0x13` | **identification group 1** — dF/dU, serial, boiler name | verified byte-for-byte |
| `0x14`–`0x1B` | **parameter block**, p1 … p124 | already mapped |
| `0x1C`–`0x1D` | **counter block** + CRC16 | already mapped, CRC new |
| `0x1E`–`0x1F` | exact mirror of `0x1C`–`0x1D` | verified byte-for-byte |
| `0x20`–`0x2F` | **blocking history**, 16 records, ring | decodes cleanly |
| `0x30`–`0x3F` | **locking history**, 16 records, ring | decodes cleanly |
| `0x40`–`0x41` | two 14-byte records + CRC16, mains-hours stamped | CRC verified, contents not |
| `0x42`–`0x5F` | empty | |
| `0x60` | `A5 5A` then `FF` — a marker of some kind | raw only |
| `0x61`–`0x64` | 64 bytes of high-entropy data, no ASCII, no repeats | raw only |
| `0x65`–`0x7F` | empty | |

So the counter block is 32 bytes held twice, not the 64 distinct bytes
`GetEepromData(0x1C, 4)` implies. Reading `0x1C`–`0x1D`, as the component does, reads
all of it.

## Address 0x01

| Blocks | Contents |
|---|---|
| `0x00` | **identification group 2** — byte-for-byte the reply this address gives to `IDENTIFICATION` |
| `0x01` | `1832720103840` — the 16-character serial the group 2 reply reports as unset |
| `0x16` | the stale block left by the misdirected writes, p33 = 6. Nothing else of `0x14`–`0x1B` exists here |
| `0x40`–`0x47` | 114 bytes of two-byte pairs, unidentified |
| everything else | empty |

`0x01` block `0x16` is the artefact described under *Two addresses, two EEPROMs* in
`pcu05_p3_protocol.md`. It is still there, still identical to `0x00`'s `0x16`, and
still read by nothing.

The `0x40`–`0x47` pairs look like `(code, value)`: `01 4B`, `1F 00`, `29 01`, `24 02`,
`28 01` recur, values stay small, and the run ends mid-block at `0x47`:2. A second
sweep after a burner cycle would say whether it grows.

## Group 1 is served, and it is at 0x00

`pcu05_p3_protocol.md` recorded that `0x00` answered `IDENTIFICATION` with nothing
and concluded no address had yet served group 1. **It does now** — 15:49:45, 64 data
bytes:

| Field | Value |
|---|---|
| dF-code | 19 |
| dU-code | 2 |
| Software version / parameter version / type | 23 / 255 / 3 |
| Next service code | 0 |
| Connected PSU / PCU / SCU-C | 4 / 5 / 255 |
| Serial number | `1832720103840` |
| Boiler name | `Tzerra Export` |

Those 64 bytes are **blocks `0x10`–`0x13` concatenated**, byte for byte. Same for
group 2 at the other address: the 16-byte reply from `0x01` is block `01:00` exactly.
The identification command is a read of a known EEPROM range, nothing more, and
either route gets the same bytes.

**The dF/dU codes can now be read over the wire**, which is what the recovery
procedure for a `Blocking 0` otherwise needs from the identification plate. dF 19 /
dU 2 for this appliance.

## The fault history rings

The 16-byte record layout from the field map decodes both rings cleanly: valid
codes throughout, states and sub-states that suit the codes, temperatures in range.
Byte 2..3 is `(A.1 + B.0) x 2` operating hours, on the same scale and the same
counter as *Power supply available hrs*, which reads **64 832** in this sweep — so
`Ago` below is hours before the sweep.

Each ring is 16 slots written in ascending block order and wrapped. The oldest
entry is where the write pointer sits: **`0x2C`** for blockings, **`0x32`** for
lockings. Rows are in ring order, oldest first.

### Blocking history, `0x20`–`0x2F`

| Blk | Code | Name | N | Hours | Ago | State | Sub-state | Flow | Ret | Cal | Out | Setp | Ion | Airflow | Pwr |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `0x2C` | 1 | T Flow > max. | 38 | 55980 | 8852 | Controlled stop | Anti-cycling | 100 | 66 | 56 | 20 | 35 | 0.0 | 0 | 0 |
| `0x2D` | 2 | dT/s Flow > max. | 4 | 57740 | 7092 | Burning CH | Gradient control level 2 | 60 | 34 | 59 | 1 | 54 | 5.9 | 1278 | 2 |
| `0x2E` | 22 | Flame lost | 9 | 59498 | 5334 | Burning CH | Stabilization time | 51 | 37 | 59 | -6 | 90 | 1.0 | 1198 | 0 |
| `0x2F` | 2 | dT/s Flow > max. | 1 | 59584 | 5248 | Burning CH | Gradient control level 2 | 64 | 37 | 55 | -10 | 60 | 6.5 | 1656 | 13 |
| `0x20` | 22 | Flame lost | 9 | 59642 | 5190 | Burning CH | Stabilization time | 51 | 36 | 59 | -4 | 90 | 0.9 | 1199 | 0 |
| `0x21` | 2 | dT/s Flow > max. | 1 | 59750 | 5082 | Burning CH | Gradient control level 2 | 64 | 37 | 55 | -5 | 60 | 6.6 | 1437 | 7 |
| `0x22` | 22 | Flame lost | 30 | 59756 | 5076 | Burning CH | Stabilization time | 50 | 36 | 59 | -3 | 90 | 0.9 | 1226 | 1 |
| `0x23` | 1 | T Flow > max. | 1 | 62344 | 2488 | Controlled stop | Anti-cycling | 100 | 65 | 57 | 26 | 29 | 0.0 | 0 | 0 |
| `0x24` | 1 | T Flow > max. | 1 | 63030 | 1802 | Burner stop | Stop fan | 100 | 62 | 56 | 18 | 37 | 0.0 | 258 | 0 |
| `0x25` | 1 | T Flow > max. | 2 | 63658 | 1174 | Controlled stop | Anti-cycling | 100 | 67 | 56 | 25 | 30 | 0.0 | 0 | 0 |
| `0x26` | 1 | T Flow > max. | 1 | 63682 | 1150 | Burner stop | Stop fan | 100 | 67 | 56 | 27 | 28 | 0.0 | 343 | 0 |
| `0x27` | 1 | T Flow > max. | 1 | 63812 | 1020 | Controlled stop | Anti-cycling | 100 | 67 | 57 | 21 | 34 | 0.0 | 0 | 0 |
| `0x28` | 22 | Flame lost | 1 | 64110 | 722 | Burning CH | Cold start | 32 | 28 | 58 | 18 | 90 | 0.8 | 2285 | 31 |
| `0x29` | 22 | Flame lost | 1 | 64110 | 722 | Burning CH | Stabilization time | 36 | 29 | 59 | 18 | 90 | 0.9 | 1216 | 1 |
| `0x2A` | 1 | T Flow > max. | 1 | 64382 | 450 | Burner stop | Post purge | 100 | 65 | 56 | 20 | 35 | 0.0 | 1174 | 0 |
| `0x2B` | 1 | T Flow > max. | 1 | 64450 | 382 | Controlled stop | Anti-cycling | 100 | 67 | 56 | 22 | 33 | 0.0 | 0 | 0 |

Every *T Flow > max.* row is a flow of exactly 100 °C with the burner already
stopping and no ionisation — heat coasting into a pump that has stopped, not a
runaway. `N` is how many times that code repeated into the same slot; the oldest
slot carries 38 of them.

### Locking history, `0x30`–`0x3F`

| Blk | Code | Name | N | Hours | Ago | State | Sub-state | Flow | Ret | Cal | Out | Setp | Ion | Airflow | Pwr |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `0x32` | 14 | 5x Unsuccessful start | 1 | 32164 | 32668 | Burner start | Flame check | 12 | 13 | 15 | -4 | 44 | 0.0 | 2304 | 0 |
| `0x33` | 36 | 5x Flame loss | 1 | 32520 | 32312 | Burning DHW | Normal power control | 68 | 64 | 36 | 2 | 60 | 1.1 | 1200 | 0 |
| `0x34` | 36 | 5x Flame loss | 1 | 39154 | 25678 | Burning CH | Flame protection | 45 | 38 | 47 | 12 | 42 | 1.1 | 1172 | 0 |
| `0x35` | 36 | 5x Flame loss | 1 | 39158 | 25674 | Burning DHW | Flame protection | 73 | 70 | 43 | 12 | 70 | 0.9 | 1189 | 0 |
| `0x36` | 36 | 5x Flame loss | 2 | 39160 | 25672 | Burning CH | Flame protection | 45 | 38 | 49 | 10 | 44 | 1.0 | 1199 | 0 |
| `0x37` | 36 | 5x Flame loss | 1 | 39164 | 25668 | Burning CH | Stabilization time | 43 | 38 | 51 | 8 | 90 | 1.1 | 1182 | 0 |
| `0x38` | 36 | 5x Flame loss | 1 | 39164 | 25668 | Burning CH | Normal internal setpoint | 44 | 37 | 51 | 8 | 44 | 1.1 | 1200 | 0 |
| `0x39` | 35 | Return over Flow temp. | 1 | 39206 | 25626 | Burning CH | Normal internal setpoint | 51 | 58 | 56 | 3 | 52 | 6.7 | 1949 | 21 |
| `0x3A` | 14 | 5x Unsuccessful start | 2 | 49694 | 15138 | Burner start | Flame check | 29 | 29 | 57 | 3 | 57 | 0.0 | 2303 | 0 |
| `0x3B` | 36 | 5x Flame loss | 2 | 50310 | 14522 | Burning CH | Stabilization time | 35 | 33 | 57 | 3 | 90 | 1.0 | 1362 | 5 |
| `0x3C` | 14 | 5x Unsuccessful start | 4 | 50508 | 14324 | Burner start | Flame check | 32 | 30 | 58 | 3 | 57 | 0.0 | 2303 | 0 |
| `0x3D` | 14 | 5x Unsuccessful start | 1 | 50524 | 14308 | Standby | Standby | 0 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| `0x3E` | 14 | 5x Unsuccessful start | 2 | 50524 | 14308 | Burner start | Flame check | 39 | 39 | 49 | 5 | 64 | 0.0 | 2302 | 0 |
| `0x3F` | 38 | SCU-S communication | 1 | 50590 | 14242 | Blocking mode | Standby | 37 | 37 | 56 | 6 | 7 | 0.0 | 0 | 0 |
| `0x30` | 38 | SCU-S communication | 2 | 50590 | 14242 | Blocking mode | Standby | 44 | 45 | -71 | -71 | 7 | 0.0 | 0 | 0 |
| `0x31` | 36 | 5x Flame loss | 8 | 59524 | 5308 | Burning CH | Stabilization time | 51 | 36 | 59 | -5 | 90 | 1.0 | 1255 | 2 |

Every lockout is a start or a flame it could not hold, at ionisation currents around
1 µA where a burning boiler in the same table shows 6. The `0x30` row reads
calorifier and outside as −71 °C, both sensors open at once, which is what an
SCU-S that has stopped talking looks like from the PCU.

### What the rings settle

- **The identification's *Last blocking code* and *Last locking code* are the newest
  ring entries**, not the live status: code 1 at `0x2B` and code 36 at `0x31`. They
  do not lag, and they are not copies of the sample bytes.
- **The `Blocking 0` of 2026-09-16 14:23 left no record.** No slot in either ring
  carries code 0, and the newest blocking is 382 hours — sixteen days — before this
  sweep. So a *PCU parameter fault* does not enter the blocking history at all, and
  the history cannot date the write. That closes the question `pcu05_p3_protocol.md`
  raised under *The rest of the EEPROM*.
- Both rings are unprotected: no checksum, and the last byte of each record is the
  *Actual power* field the map documents.

## Counters, and the CRC convention

Blocks `0x1C`–`0x1D` at 15:50:

| Off | Name | Raw | Value |
|---|---|---|---|
| 0 | Hours run pump CH+DHW | `0x7987` | 62 222 |
| 2 | Hours run 3-way valve DHW | `0x0667` | 3 278 |
| 4 | Hours run CH+DHW | `0x3F6D` | 32 474 |
| 6 | Hours run DHW | `0x0C18` | 3 096 |
| 8 | Power supply available hrs | `0x7EA0` | 64 832 |
| 10 | Pump starts CH+DHW | `0x0228` | 4 416 |
| 12 | 3-way valve cycles | `0x0451` | 8 840 |
| 14 | Burner starts DHW | `0x04D2` | 9 872 |
| 16 | Total burner starts CH+DHW | `0x3F37` | 129 464 |
| 18 | Failed burner starts | `0x0105` | 261 |
| 20 | Number of flame loss | `0x008B` | 139 |

Offsets 22–28 are zero. **Offset 29 is a byte the map does not have** and it moved
`0x66` → `0x6C` between the capture in `pcu05_p3_protocol.md` and this one, while the
hour counters moved by 4 hours — too fast for an hour count, about right for a
count of how often the block has been saved.

**Offsets 30–31 are a CRC16** — poly `0xA001`, init `0xFFFF`, over offsets 0…29,
stored LSB first. Exactly the frame CRC, run over stored bytes instead of wire
bytes. It checks out on both captures independently (`0x027C` then `0x99A1`), which
is two out of two on data that moved in between.

Blocks `0x40` and `0x41` carry the same convention at record scale: CRC16 over their
first 14 bytes in bytes 14–15, LSB first, verified on both.

```
00:40  FF FF 7E A0 3F 69 40 3A 00 FF FF FF FF 1C 92 0D
00:41  FF FF 7E 9F 3F 69 40 3A 00 FF FF FF FF 1B 1C DB
```

`7E A0` and `7E 9F` are the *Power supply available* counter one step apart, `3F 69`
is *Hours run CH+DHW* four hours behind the live value, and the two records
alternate — a ping-pong pair, each written in turn so a mains loss can never leave
both corrupt. What `40 3A` and byte 13 are is open.

## What the parameter block does not have

`0x14`–`0x1B` bytes 126–127 are `85 9A`, past the last documented parameter, and
they are **not** a checksum of the block. Searched and found nothing:

- CRC16 `0xA001` and CRC16-CCITT, inits `0xFFFF` / `0x0000` / `0x1D0F`, 8-bit XOR,
  16-bit sum and its complement, over every start 0…19 and every end 96…128,
  against both the pre-write image and this one. One coincidental hit on a
  nonsense range.
- The CRC of the **pre-write** image over 0…123, 0…125 and 0…127 appears nowhere in
  either 2 KB image, in either byte order. If the PCU keeps a parameter checksum,
  it is not in the EEPROM in any form searchable this way.

Which leaves the `Blocking 0` without a stale-checksum explanation. `85 9A` did not
move when p33 did — but the component wrote those two bytes back verbatim, so that
proves only that the PCU did not rewrite them afterwards.

## The parameter block after the write

Blocks `0x14`–`0x1B` in this sweep differ from the pre-write image in
`pcu05_p3_live_parameters.md` at **one byte**: byte 32, p33, `0x04` → `0x06`. The other
127 are identical. The write did what it was asked to and nothing else, and it has
survived the blocking and the reboots since.

## Still unidentified

| Where | Bytes | Note |
|---|---|---|
| `00:00`–`00:02` + shadow | 48 | 16-bit-looking fields, one duplicated copy, no ASCII |
| `00:03`, `00:04`:0–10 | ~16 | small signed values, calibration-shaped |
| `00:40`–`00:41` bytes 6–13 | 8 each | alongside two known hour counters |
| `00:60`–`00:64` | 80 | `A5 5A` marker then high-entropy data — the only region in 2 KB that does not look like plain fields |
| `01:40`–`01:47` | 114 | two-byte pairs, `(code, value)`-shaped |

None of it is needed to read or write parameters. The one thing a second sweep would
be worth doing for is `0x60`–`0x64`: if those 64 bytes change when a parameter is
written, they are the parameter store's integrity data and the `Blocking 0` has its
explanation.
