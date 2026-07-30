# Flash Ring Buffer — Hardware Bring-Up Procedure

**English** | [Polski](FLASH_RING_BRINGUP_PL.md) | [Español](FLASH_RING_BRINGUP_ES.md)

📖 [Project home](../README.md) · [Manual](README_EN.md)

The wear-levelled ring buffer stores both "live" data (learned drift/damping, LC
calibration, last PWM) and the settings block in flash **sector 7**
(0x08060000, 128 KB) — the last sector, chosen so firmware keeps the maximum
contiguous space below it. Records are typed, so settings and live data share
one ring without colliding. The ring is always on from v0.96; `FR` remains as a
read-only status command and `EW` reports wear.

The core logic is unit-tested (28 assertions on a PC). Bring the flash side
up deliberately, with a backup, the first time.

## 0. Backup first (recommended)

With the J-Link (or ST-Link), dump the whole flash so you can restore if
needed:

```
JLinkExe -device STM32F411CE -if SWD -speed 4000
> savebin backup_full.bin 0x08000000 0x80000
> exit
```

To restore later: `loadbin backup_full.bin 0x08000000`.

## 1. Confirm firmware fits below sector 7

Check the compile size line: `Sketch uses NNNNNN bytes ...`

`NNNNNN` must stay **below 393216** (0x08060000 - 0x08000000) — that is where
sector 7 and the ring begin. v0.95 measured 216976 B (212 KB); against the
384 KB available that leaves ~176 KB of head-room. If a future build nears
384 KB, move the ring or trim firmware **before** flashing.

Ignore the percentage the IDE prints. It reports against the full 512 KB, so a
212 KB build shows as "41%" — but sector 7 is spoken for by the ring, and
against the 384 KB firmware may actually use, the real figure is ~57%.

## 2. Enable the ring, watch `EW`

The ring defaults to **on**. To be explicit, over serial:

```
FR 1
ES
```

Then `EW`. Expect on a virgin sector:

```
Flash ring: erase cycles=1  slots used=0/255  (sector 7, 0x08060000)
```

`erase cycles=1` is normal: the first `begin` finds no valid header, erases
once, and lays down a fresh one. `slots used=0` because nothing has been
auto-saved yet.

## 3. Force a save, confirm it persists

Let LRN accumulate drift past the hysteresis threshold (> 8 LSB), or run a
successful `LC` (which auto-saves immediately). Then `EW` should show
`slots used=1`. Power-cycle the unit. On boot you should see:

```
Flash ring: live data recalled
Live store: LRN + LC applied from flash ring
```

Run `EW` again — still `slots used=1`, and the learned/calibration values are
restored. This proves the write survived a reboot — the whole point.

## 4. Garbage/erase safety (optional, thorough)

Erase only sector 7 with the J-Link and reboot:

```
> erase 0x08060000 0x0807FFFF
```

On boot the firmware must detect the blank sector, re-init the ring (erase
cycles increments), and start from defaults — no hang, no garbage. Verifies
the robustness path.

## 5. Disabling

`FR 0` + `ES` stops all flash-ring activity: no reads, no writes, no erases.
Learned/calibration values then live only in RAM and are lost on reboot.
Re-enable any time with `FR 1` + `ES`.

## 6. Flashing new firmware later (important)

- **Bootloader / DFU / Arduino IDE upload**: touches only firmware sectors
  (0-5). Ring (6) and settings EEPROM (7) survive.
- **J-Link/ST-Link "Erase Chip" / full-chip erase**: wipes everything,
  including learned data and calibration. To keep them, erase only sectors
  0-5:
  ```
  > erase 0x08000000 0x0803FFFF
  > loadbin firmware.bin 0x08000000
  ```
- If the ring is wiped, the firmware simply relearns/recalibrates from
  defaults — nothing breaks, you just lose the accumulated tuning.

## Rollback

If anything misbehaves, `FR 0` + `ES` disables the ring immediately (no
reflash needed). Restore `backup_full.bin` if flash contents were disturbed.
