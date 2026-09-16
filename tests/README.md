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

- a read that does not come back means **no write frame is sent at all**, so the
  fifteen unrelated parameters in that block are never at risk
- every failure path still sends `CODE_SERVICE_STOP`, including a failure of the
  unlock itself (Recom re-locks only in its success branch; that bug is
  deliberately not copied here - see `mapping/pcu05_p3_protocol.md`)
- a write that is never ACKed is a failure, not a success (Recom's other bug)
- values are clamped to the documented range, and parameters outside the
  writable list - the gas/air settings and the controller-protection limits -
  are refused outright
- writing a value the boiler already holds sends no write frame, because EEPROM
  endurance is finite
- `allow_writes` and `variant: pcu05_p3` both gate the whole path
- ordinary polling never unlocks service mode or writes anything
- a boiler that is burning, purging or finishing a charge is **not** written to,
  and the pre-flight sample that decides this is taken inside the transaction -
  but one already in blocking mode is, because that is how a bad value gets undone
- a blocking code that appears while the write runs is reported even when the
  write itself verified byte-for-byte
- `reset_board()` sends `COMMAND 0x31` alone: no unlock, no read, no write

The stubs in `stub/` are only as complete as these tests need. They are not a
substitute for building the component with ESPHome.
