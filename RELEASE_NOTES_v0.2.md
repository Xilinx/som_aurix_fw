## What's New in v0.2

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
- `TC387_v0.2_provision.hex` — first flash / factory programming: application + BMHD + UCB_SWAP + UCB_OTP0 (SOTA enabled). Use AURIXFlasher with `-ucb on`.
- `TC387_v0.2_app.hex` — application image for OTA updates (`tools/aurix_update.py`).
- `TC387_v0.2.elf` — symbols for debugging.
- `aurix_update.py` — host OTA tool (`pip install pyserial intelhex`).
- `SHA256SUMS` — checksums.
