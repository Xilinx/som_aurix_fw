## What's New in v0.3

Built on the v0.2 feature set below, v0.3 is a stability/safety candidate that
integrates the reviewed PRs merged into `v0.3-testing`:

- **OTA safety (PR #7)** — revert now changes the swap bank *before* clearing the
  pending metadata; a SOTA update is committed only after POST passes on a
  pending image, and a failed commit is retried; SOTA metadata is verified
  (read-back) after writing; flash self-tests are skipped while an update is
  pending.
- **Boot / IPC (PR #6)** — the core-sync barrier word is read through a volatile
  pointer so Release (`-O2`) builds no longer hang at boot; UART owner and debug
  rings are reset before CPU0's first print; the ASCLIN4 transfer UART gets its
  own interrupt priorities; the UART flush at handoff keeps the TLF window
  watchdog serviced.
- **UART hand-off on request** — the AURIX keeps the shared UART by default, so
  the CLI and CLI OTA stay available. CLI `swap uart` (alias `mux apu`) hands it
  to the x86 SoC for the BIOS console until the next AURIX reset. The automatic
  hand-off after System ON from PR #9 (`PM_UART_HANDOFF_AFTER_ON_MS`) is kept
  but now defaults to `0u` (off), for the v0.4 dedicated Ryzen-AURIX UART.
- **FuSa SPI (PR #4 / PR #5)** — register map populated at version 1.1.0; frame
  length corrected to 32 bits and an idle slave is re-armed after an SPI error.
- **Version / cleanup (PR #8)** — firmware version reported as v0.3
  (`FUSA_FW_VERSION = 0x00030000`, CLI and boot banner), dead code removed.

### Carried from v0.2

- **Multicore architecture** — CPU0/1/2/3 with LMU-based IPC, per-core peripheral ownership, TLF WDT handover
- **Over-the-Air firmware update (SOTA)** — PFlash A/B bank swap, UART transfer protocol, CRC-32 integrity, 3-retry rollback
- **NV event logging** — DFlash crash recorder with 4 rotating slots, survives power loss
- **FuSa SPI slave** — 96-register status map readable by carrier safety controller
- **Debug CLI** — 20+ interactive commands over UART
- **Self-test suite** — validation of CRC, SOTA, Swap, FusaSPI, USB PD proxy, HPD, topology config
- **APU sideband protocol** — bidirectional Ryzen↔AURIX communication over ASCLIN4
- **TLF35585 support** — INITERR clearing, protected config writes, SoM-specific init sequence
- **APML temperature monitoring** — SB-TSI/SB-RMI over I2C1
- **CF9 cold reset suppression** — prevents unnecessary LPDDR5 retraining (35s→10s boot)
- **Recovery mode** — RSTBTN# held at boot enters dedicated update loop
- **USB PD expanded** — HPI register reader, APU proxy packing (AMD 40-bit format), virtual HPD, topology config, I2C diagnostics

## Files
- `TC387_v0.3_provision.hex` — first flash / factory programming: application + BMHD + UCB_SWAP + UCB_OTP0 (SOTA enabled). Use AURIXFlasher with `-ucb on`.
- `TC387_v0.3_app.hex` — application image for OTA updates (`tools/aurix_update.py`).
- `TC387_v0.3.elf` — symbols for debugging.
- `aurix_update.py` — host OTA tool (`pip install pyserial intelhex`).
- `SHA256SUMS` — checksums.
