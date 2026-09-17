# Write-path tests

The component can put bytes into a boiler's EEPROM, so the parts of it that
decide *not* to write are worth being able to check without a boiler attached.

`test_write.cpp` compiles `components/dietrich/dietrich.cpp` on a PC against the
stub ESPHome headers in `stub/` and drives it against a simulated PCU-05 P3
that speaks the Remeha frame format, keeps 16-byte EEPROM blocks, and refuses a
`WRITE_EPROM_BLOCK` unless service mode is on.

```sh
./tests/run.sh
```

No ESPHome, no toolchain beyond a C++17 compiler, no hardware.

## What it pins down

The happy paths - unlock/re-lock, an identity write, a single-parameter
read-modify-write - and, more to the point, the refusals:

- a parameter write recomputes the image's **two CRC16s** (bytes 62-63 over
  0-61, bytes 126-127 over 64-125), so exactly three bytes move for a
  single-parameter change, and the simulated board - which judges the stored set
  at the re-lock, as a PCU-05 P3 does at its next identification - has nothing to
  raise `Blocking 0` about. Undo just the CRC afterwards and it does raise it.
- an image that arrives already failing its own CRC is reported and repaired
- a read that does not come back means **no write frame is sent at all**, so the
  fifteen unrelated parameters in that block are never at risk
- every failure path still sends `CODE_SERVICE_STOP`, including a failure of the
  unlock itself (Recom re-locks only in its success branch; that bug is
  deliberately not copied here - see `mapping/pcu05_p3_protocol.md`)
- a write that is never ACKed is a failure, not a success (Recom's other bug)
- a value outside the documented range is **refused, not clamped**, and so is a
  parameter outside the writable list - the gas/air settings and the
  controller-protection limits
- the five two's complement parameters (p27, p30, p61, p86, p106) round-trip as
  the manual writes them: `-10` in, `0xF6` in EEPROM, `-10` back out of the
  queue, the verdict and the range check
- p28 and p29 are written in the units the manual numbers them in (2..10, not
  20..100 %), and a percentage typed in by mistake is refused rather than stored
- a write to p73 or p105 lands in the image's **upper** half and refreshes the
  CRC at bytes 126..127 rather than the one at 62..63
- several queued edits ride in **one** transaction and cost exactly what one
  edit does: eight block writes, one unlock pair, one EEPROM cycle
- queueing a parameter twice replaces its value rather than staging two edits
  for one byte, and a ninth edit is refused rather than dropping one silently
- the queue survives a refusal or a failure, so a write that failed part-way
  can be retried without restaging, and is emptied only by a write that reads
  back verified
- the write enable gate refuses a write without unlocking anything
- writing a value the boiler already holds sends no write frame, because EEPROM
  endurance is finite
- `allow_writes` and `variant: pcu05_p3` both gate the whole path
- ordinary polling never unlocks service mode or writes anything
- a boiler that is burning, purging or finishing a charge **is** written to, and
  the pre-flight sample inside the transaction records what it was doing rather
  than refusing. That gate is gone: both writes that put a PCU-05 P3 into
  `Blocking 0` went out to a boiler the gate called quiet, so it never stood
  between the write and the fault - the image CRC did
- a blocking code that appears while the write runs is reported even when the
  write itself verified byte-for-byte
- `reset_board()` sends `COMMAND 0x31` alone: no unlock, no read, no write

The stubs in `stub/` are only as complete as these tests need. They are not a
substitute for building the component with ESPHome.
