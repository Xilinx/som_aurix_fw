# som_aurix_fw — TC387 COM-HPC Platform Controller

Bare-metal firmware for the Infineon AURIX TC387 acting as the module-side platform management controller on a COM-HPC System-on-Module (SOM) hosting the AMD Strix Halo (FP11) x86 SoC.

## What's New in v0.2

- **Multicore architecture** — CPU0/1/2/3 with LMU-based IPC, per-core peripheral ownership, TLF WDT handover
- **Over-the-Air firmware update (SOTA)** — PFlash A/B bank swap, UART transfer protocol, CRC-32 integrity, 3-retry rollback
- **NV event logging** — DFlash crash recorder with 4 rotating slots, survives power loss
- **FuSa SPI slave** — 96-register status map readable by carrier safety controller
- **Debug CLI** — 20+ interactive commands over UART
- **Self-test suite** — comprehensive validation of CRC, SOTA, Swap, FusaSPI, USB PD proxy, HPD, topology config
- **APU sideband protocol** — bidirectional Ryzen↔AURIX communication over ASCLIN4
- **TLF35585 support** — INITERR clearing, protected config writes, SoM-specific init sequence
- **APML temperature monitoring** — SB-TSI/SB-RMI over I2C1
- **CF9 cold reset suppression** — prevents unnecessary LPDDR5 retraining (35s→10s boot)
- **Recovery mode** — RSTBTN# held at boot enters dedicated update loop
- **USB PD expanded** — HPI register reader, APU proxy packing (AMD 40-bit format), virtual HPD, topology config, I2C diagnostics

## Firmware Functionality Overview

### 1. Power Supply Sequencing
The TC387 manages the complete power-on and power-down sequencing of the AMD FP11 SoC platform in compliance with AMD Electrical Data Sheet 58241, Section 16 ("Power Supplies and Signal Sequencing").

Power rails are organised into five stages mapped to AMD's Group A–D power sequencing model:

| Stage | SOM Enable Signal | Rails Controlled | AMD Group |
|-------|-------------------|------------------|-----------|
| 0 — EFUSE | MAIN_12V_EFUSE_EN | 12V_MAIN path to SOM | COM-HPC pre-condition |
| 0a — VR3V3 | VR_APU_3V3_EN | VR_APU_3V3 standby rail | Group A (standby) |
| 1 — Group B | PWR_GROUP_B_EN | VDD_MISC_S5, VDD_12_S5, VDD_18_S5, VDDIO_33_S5 | Group B (S5) |
| 2 — Group C | PWR_GROUP_C_EN | VDD_MEM, VDDIO_MEM, VDD_MEMQ (Ch A & B) | Group C (S3/S0) |
| 3 — Group D | PWR_GROUP_D_EN | VDDCR (via MP2825A / MP86979) | Group D (S0) |

Sequencing enforces the following per AMD 58241 §16.1.2:

- 12 V EFUSE enabled first; PG verified before proceeding.
- VR_APU_3V3 (Group A standby rail) ramped and verified stable before Group B enable.
- Group B fully stable before Group C exceeds 10 % of nominal.
- Group C fully stable before Group D exceeds 10 % of nominal.
- All Group D rails stable ≥ 1 ms before APU_PWR_GOOD assertion (5 ms deglitch implemented; §16.1.1).
- RSMRST_L deasserted ≥ 10 ms after Group B stable (AMD Table 28 T1).
- RTCCLK stabilisation window honoured (T1a: ≥ 16 ms RSMRST to PWR_BTN).
- COLD_RST (SYS_RESET_L) held asserted for ≥ 28.5 ms after APU_PWR_GOOD assertion before release (AMD Table 30 T7 / §16.1.5 point 3).
- SoC APU_PWROK polled after PWRGD assertion with a 100 ms ceiling (AMD T5/T6: 21.4 ms typical). Failure to assert triggers a startup fault.
- All individual rail power-good (_PG) signals are verified at each stage. A configurable timeout (per-rail, default 50–100 ms) and debounced PG monitoring during PM_STATE_ON detect rail failures and initiate an emergency shutdown.
- Upstream PG verification: at each ramp stage the firmware re-verifies all previously-ramped groups remain stable. If any upstream rail drops PG during a later stage, the sequencer faults immediately rather than continuing with a partially-powered platform.

### 2. Power State Machine
The firmware implements a non-blocking cooperative state machine driven by `PowerManager_Run()`, called from the CPU1 main loop (multicore) or CPU0 main loop (single-core).

| State | Description |
|-------|-------------|
| PM_STATE_OFF | All rails disabled. Awaiting power-on request. |
| PM_STATE_POWER_UP | Transient entry state; disables VoltMon and begins ramp. |
| PM_STATE_RAMP_ALW | Ramping 12 V EFUSE (always-on stage). |
| PM_STATE_RAMP_VR3V3 | Ramping VR_APU_3V3 standby rail. |
| PM_STATE_RAMP_S5 | Ramping Group B (S5 rails), releasing RSMRST_L and pulsing PWR_BTN. |
| PM_STATE_RAMP_S3 | Ramping Group C (memory rails). |
| PM_STATE_RAMP_S0 | Ramping Group D (VDDCR core), BIOS ROM validation, PWROK wait. |
| PM_STATE_ON | System fully powered. Monitoring PWROK, SLP signals, THERMTRIP, RSTBTN#. |
| PM_STATE_DN_S0_S3 | Powering down Group D (and Group C if full shutdown). |
| PM_STATE_DN_S3_S5 | Waiting for S0i3 wake event or completing soft shutdown to S5. |
| PM_STATE_S5 | Parked at S5 (Group B + EFUSE on). Awaiting wake or cold-reset dwell. |
| PM_STATE_DN_S5_OFF | Powering down Group B and EFUSE to reach OFF. |
| PM_STATE_FAULT | Fault with optional auto-retry (non-blocking delay) or latch-off. |
| PM_STATE_WARM_RESET | KBRST_L asserted without dropping rails; re-validates BIOS ROM. |

### 3. S0i3 (Suspend-to-RAM) Support
- **Entry:** when SLP_S3 asserts in PM_STATE_ON, the firmware disables Group D while keeping Group C (memory) powered, then transitions to PM_STATE_DN_S3_S5 to wait for a wake event.
- **Wake sources:** physical power-button press or autonomous SLP_S3 deassertion (Wake-on-LAN, RTC alarm, etc.). On autonomous wake the firmware skips the PWR_BTN pulse since the chipset has already initiated its own wake sequence.
- **T1' hold-off:** a configurable minimum entry time (PM_T1_PRIME_MS) suppresses false wake-edge evaluation immediately after S0i3 entry.
- **Safety checks during suspend:** upstream PG is periodically re-verified, and a configurable S0i3 timeout (PM_S0I3_TIMEOUT_MS) faults out if no wake event arrives within the limit.

### 4. Fault Handling and Retry Logic
On any power-good timeout, PG loss, or voltage-monitor fault:
- All reset and power-down signals are asserted (COLD_RST, KBRST_L, RSMRST_L, PWRGD deasserted), all rails disabled in reverse order.
- If retries remain (PM_MAX_RETRIES), the firmware enters PM_STATE_FAULT with a non-blocking retry delay so that THERMTRIP, VoltMon, and USB PD servicing continue unblocked during the wait.
- After the delay expires, the state machine re-enters PM_STATE_POWER_UP for a full re-sequencing attempt.
- If all retries are exhausted, the firmware latches off in PM_STATE_FAULT until a physical power cycle.
- Fault causes are tracked via `PM_ResetCause_t`: PG_TIMEOUT, PG_LOSS, THERMAL, HOST_REQUEST, VOLTAGE, BIOS_FAIL, COLD_RST.

### 5. Power Button Handling
CB_PWRBTN# (active low) is debounced and supports two actions:
- **Short press** (< 4 s): in PM_STATE_ON, forwarded to the APU as an 18 ms APU_PWRBTN pulse. In OFF or S5 states, triggers a power-on sequence.
- **Long press** (≥ 4 s): forced emergency shutdown from any active state, transitioning directly to PM_STATE_OFF.
- **Forced-off cooldown:** after a forced shutdown, power-on requests are suppressed for 2 s to reject button bounce.

### 6. Reset Button and Cold Reset
CB_RSTBTN# (active low) triggers a cold reset while in PM_STATE_ON:
- Debounced with configurable consecutive low reads.
- On confirmed press, COLD_RST is asserted for a minimum pulse width. If CB_RSTBTN# is still held, COLD_RST remains asserted until the button is released.
- No rail or state changes occur — the APU performs its own internal reset. PG-loss monitoring stays armed throughout.
- **CF9-style cold reset detection:** the firmware observes APU_RESET_L assertion from the SoC. SLP_S5 glitches during APU-initiated resets are suppressed for a configurable window (PM_CF9_SUPPRESS_MS, default 2000 ms) to prevent unnecessary rail teardown and LPDDR5 retraining.

### 7. THERMTRIP# Handling
- **ISR path:** sets a flag atomically; main loop captures and clears under a brief critical section.
- **Polled debounce:** consecutive active reads in PM_STATE_ON trigger the THERMTRIP handler.
- On confirmed assertion the platform suspends to S5. Recovery is permitted only after a valid wake event.

### 8. Input Voltage Monitoring
VIN_PWR_OK is checked on every main-loop iteration. If it drops while the system is powered, an immediate emergency shutdown is triggered.

### 9. BIOS ROM Validation
Before releasing COLD_RST on cold boot (and during warm reset), the firmware reads the BSEL[2:0] boot-select straps, asserts the SPI MUX to take ownership of the BIOS ROM flash, and runs a validation check. The SPI MUX is released back to the APU after validation completes.
- QSPI0 P22.7/9/10/11 tri-stated, APU_ROM_SPI_SEL P22.8 set to release bus to Ryzen.

### 10. Analog Voltage Monitoring (VoltMon)
Continuous ADC-based monitoring of all SOM power rails using the TC387 EVADC peripheral:
- **19 channels** across 5 EVADC groups on SoM (6 on eval board)
- Per-channel UV/OV thresholds at WARNING (±8%) and FAULT (±10%) levels
- Per-channel severity latch with recovery hysteresis (assert immediate, clear debounced)
- SMA filtering optional for ADC noise rejection
- FAULT-severity events invoke registered callback → PowerManager shutdown/retry path
- Edge-triggered detection: single callback per fault event, not per-sample

### 11. COM-HPC Interface Signals

| Signal | Direction | Function |
|--------|-----------|----------|
| VIN_PWR_OK | In | Guards power-on; emergency shutdown if lost while powered |
| CB_PWRBTN# | In | Debounced power button: short press = on/forward, long hold = forced shutdown |
| CB_RSTBTN# | In | Debounced reset button: triggers COLD_RST pulse |
| SLP_S3, SLP_S5 | In (from APU) | ACPI sleep-state transitions |
| APU_PWROK | In (from APU) | SoC power-good feedback |
| APU_RESET_L | In (from APU) | SoC reset status; CF9 cold-reset detection |
| SLEEP# | In | ACPI sleep button |
| RAPID_SD | In | Rapid shutdown trigger |
| LID# | In | Lid switch monitoring |
| TAMPER# | In | Tamper detection monitoring |
| CB_AC_PRESENT | In | AC power present from carrier |
| CB_BATLOW# | In | Battery low from carrier |
| APU_PCC_L | In | Regulator over-temperature |
| APU_PWR_GOOD | Out | Asserted when all power groups are stable |
| COLD_RST (SYS_RESET_L) | Out | APU system reset |
| WARM_RST (KBRST_L) | Out | APU keyboard reset |
| MMC_RSMRST_L | Out | APU resume reset |
| PLTRST_L | Out | Platform reset to carrier |
| RSMRST_OUT_L | Out | Mirrors AURIX RSMRST_L to COM-HPC carrier |
| PROCHOT# | Out | Asserted LOW when APU thermal event detected |
| CATERR# | Out | Catastrophic error indicator to carrier |
| WD_OUT | Out | Watchdog timeout indicator |

### 12. COM-HPC Signal Mirroring
Per the COM-HPC specification:
- **PLTRST#:** mirrors SoC APU_RESET_L → COM-HPC PLTRST_L. Not released while CB_RSTBTN# is low.
- **RSMRST_OUT#:** mirrors AURIX MMC_RSMRST_L read-back → COM-HPC RSMRST_OUT_L.

### 13. FuSa Status Outputs
Two-bit FuSa status field to the carrier FuSa Safety Controller:

| FUSA_STATUS[1:0] | State | Condition |
|-------------------|-------|-----------|
| 00 | Power off | MAIN_12V_EFUSE_EN not asserted |
| 01 | Power good | System fully powered, APU_PWR_GOOD asserted |
| 10 | Fault | PG loss, PG timeout, voltage fault, or THERMTRIP |
| 11 | Reset | SoC platform in reset |

### 14. COM-HPC Watchdog (ComHpcWdt)
A COM-HPC-compliant watchdog timer is enabled after PM_STATE_ON. The watchdog is disabled on any shutdown, fault, sleep, or THERMTRIP transition to prevent false watchdog faults during expected power-state changes.

### 15. PROCHOT / CATERR Management
- APU_PROCHOT_L configured as open-drain so both TC387 and APU can assert independently.
- CATERR# driven HIGH by default, assertable via `SysMonitor_AssertCaterr()`.

### 16. UART MUX Control
The ASCLIN0 debug UART is shared between the AURIX and the x86 SoC via a board-level multiplexer:
- **UART_MUX_SEL = 1 (HIGH):** AURIX owns the UART (power-on through SYS_RESET_L release)
- **UART_MUX_SEL = 0 (LOW):** x86 SoC owns the UART (after SYS_RESET_L is deasserted)

### 17. USB PD Sideband Management
Two Infineon CYPD6129 USB PD controllers managed via I2C0 (400 kHz, P13.1/P13.2). Pin-strapped to addresses 0x40 (R869 LOW) and 0x42 (R726 HIGH). Both devices share a wired-OR USBC_PD_ALERT_L interrupt line (P10.7).

**v0.2 USB PD subsystem includes:**
- **UsbPd_Hpi** — HPI register reader using 16-bit register addressing over I2C0
- **UsbPd_ApuProxy** — AMD 40-bit APU register format packing for Ryzen sideband
- **UsbPd_Hpd** — Virtual HPD driver for DP2_HPD (P13.0) and DP3_HPD (P13.3) with IRQ pulse timing per DP specification
- **UsbPd_Cfg** — DFlash-backed topology config with checksum protection, CYPD6129/6229 abstraction
- **UsbPd_Manager** — Dual-port event dispatch with HPI reads, APU proxy packing, HPD dispatch, NvLog integration
- **I2c_Slave** — Raw I2C1 slave driver for APU proxy register readback

### 18. Multicore Architecture (v0.2)
Four-core partitioning with deterministic peripheral ownership:

| Core | Rate | Modules |
|------|------|---------|
| CPU0 | 20 Hz | Startup, UART, DFlash, PFlash, NvLog, FwUpdate, BIST, DebugCli, BiosRom |
| CPU1 | 50 Hz | PowerManager, SysMonitor, UsbPd, ComHpcWdt, ERU, I2C0/I2C1 |
| CPU2 | 100 Hz | VoltMon (EVADC), TLF35585 (QSPI2), FusaSpi (QSPI3) |
| CPU3 | — | Sync event participant, reserved |

**IPC shared memory** in non-cached global LMU (`lmuram_nc` at 0xB0040000):
- `Ipc_CmdMailbox_t` — CPU0→CPU1 command dispatch with seqNum/ackNum handshake
- `Ipc_PmcStatus_t` — CPU1→all: PM state, reset cause, retry count, temperature
- `Ipc_FusaStatus_t` — CPU2→all: 24 voltage channels, TLF status, fault codes

**Synchronisation:** `IfxCpu_syncEvent` barrier where all cores block until every core emits, then proceed simultaneously.

**TLF WDT ownership handover:** CPU0 services during init, writes `g_ipcShared.wdtOwner = 2`, CPU2 takes over for runtime. Dual-path SPI: CPU2 uses ISRs, CPU0 polls SRR flags manually (interrupt-off safe during flash erases).

### 19. Over-the-Air Firmware Update (SOTA) (v0.2)
PFlash A/B bank management with UART-based transfer protocol:
- **PFlash driver:** erase, burst-write (256B pages), verify under the alternate address map
- **DFlash metadata:** SOTA metadata (magic, pending flag, boot counter, image CRC, active bank)
- **Transfer protocol:** SYNC→HEADER→erase→DATA chunks→verify→commit, per-chunk CRC-32 with retry
- **BootValid:** checks pending update on startup, increments boot counter, commits after PM_STATE_ON reached, 3-retry automatic revert
- **Swap controller:** reads current bank configuration
- **POST integrity check:** CRC-32 of active PFlash bank at boot vs stored reference
- **Recovery mode:** RSTBTN# held at boot enters dedicated update loop without starting the Ryzen
- **Python upload script:** `aurix_update.py` (pyserial) for pushing firmware from Ryzen or external PC

**Validated end-to-end on SoM:** 696 chunks (178KB) transferred from Ubuntu on the Ryzen over ASCLIN4, CRC verified, committed, AURIX rebooted.

### 20. NV Event Logging (NvLog) (v0.2)
Persistent crash/event recorder in DFlash:
- 4 × 32KB rotating slots in DFlash0
- 32-byte event records with XOR checksum
- RAM ring buffer (32 events) with periodic flush (5 s) and seal-on-shutdown
- Severity-≥ERROR events flush immediately
- Append-only design: count reconstructed by scanning at init
- Survives power loss — validated across 187+ boots on eval, 8+ boots on SoM

### 21. FuSa SPI Slave Register Map (v0.2)
QSPI3 slave on P2.4/5/6/7 (CS/MISO/MOSI/CLK), ALERT on P2.0:
- 96-register map: identity (magic, version), PM state, thermal/APML, TLF PMIC status, 24 VoltMon channels, USB PD state, NV log stats
- Updated every 100 ms on CPU2
- ALERT# pin asserted on voltage/PMIC faults, deasserted when cleared

### 22. Debug CLI (v0.2)
Interactive serial console over ASCLIN0 (115200 baud, 8N1):

| Command | Description |
|---------|-------------|
| help | List all commands |
| status | Full system status dump |
| poweron / poweroff | Power control via IPC to CPU1 |
| forceoff | Emergency shutdown |
| warmreset / coldreset | Reset control |
| clearfault | Clear fault state, return to OFF |
| tlf | TLF35585 register dump |
| vmon | VoltMon channel readings (via IPC in multicore) |
| nvlog [recent N] [slot S] | NvLog event inspection |
| fusa | FuSa SPI register map dump |
| bist | Run POST/periodic BIST |
| selftest [all\|crc\|sota\|swap\|fusa\|pm\|usbpd\|hpd\|topo\|usbpd_edge] | Self-test suite |
| i2cscan [bus] | I2C bus scan |
| i2cread [bus] [addr] [reg] | I2C register read (bus-aware: bus 0 = 16-bit, bus 1 = 8-bit) |
| i2cdiag | I2C bus diagnostic with GPIO probe and recovery |
| i2cbbscan | Bit-bang full I2C address sweep |
| i2csniff [secs] | Passive I2C bus sniffer |
| i2cslave [addr] [secs] | I2C slave mode listener |
| pdprobe | Probe all candidate PD controller addresses |
| cfgpins | Read HWCFG3/4/5 boot mode strapping |
| fwupdate | Enter SOTA update mode on debug UART |
| apmlread [addr] [reg] | APML SMBus register read (8-bit addressing) |

### 23. Self-Test Suite (v0.2)
Comprehensive validation exercisable from CLI `selftest`:

| Test | What it validates |
|------|-------------------|
| crc | CRC-32 table + incremental API against IEEE check value (0xCBF43926) |
| sota | Full cycle: erase → write → CRC → metadata pending → commit → POST verify |
| swap | DFlash metadata bank ID round-trip (0x55/0xAA) |
| fusa | Register map: magic, version, build config, PM state, uptime |
| fwup | 2-chunk emulated download: erase → per-chunk CRC → write → verify → commit |
| usbpd | APU proxy packing: USB4+DP active cable, TBT3 sink, detached, DP 2-lane passive |
| hpd | HPD state machine: HIGH/LOW/IRQ, IRQ-while-LOW rejection, port independence, DeassertAll |
| topo | Topology config: defaults, DFlash round-trip, corrupt checksum rejection |
| usbpd_edge | Edge cases: max custom fields, EMCA gating, port index stability, invalid port rejection |
| vmon_live | (SoM only) Real ADC readings, S5 rail presence, GND reference check |
| tlf_live | (SoM only) DEVSTAT=NORMAL, ERR/SS pins inactive |
| usbpd_live | (SoM only) PD controller I2C probe + HPI state read |
| pm_live | (SoM only) PM state, VIN_PWR_OK, SLP_S3/S5, APU_PWROK GPIO readback |

### 24. APU Sideband Protocol (v0.2)
Bidirectional Ryzen↔AURIX communication over ASCLIN4:
- Binary framed: `[CMD:8][LEN:8][PAYLOAD][CRC8]`
- Commands: PING, GET_STATUS, GET_VOLTAGES, GET_TEMP, GET_PM_STATE, GET_TLF_STATUS, GET_USBPD, SHUTDOWN, WARM_RESET, COLD_RESET, FUSA_REG_READ
- Coexists with FwUpdate: detects SYNC magic and hands off to SOTA mode
- Python companion script (`aurix_sideband.py`) with CLI subcommands: status, voltages, temp, pm, tlf, shutdown, reset, monitor

### 25. TLF35585 PMIC Driver (v0.2)
QSPI2 on P15.2/3/6/7 (CS/SCL/SDI/SDO):
- **EarlyInit:** immediate WDT service within 14 ms of power-on
- **Init:** INITERR clearing (W1C), protected config writes with settling delays and readback verify, SYSSF clear, WWD service, NORMAL transition
- **Runtime:** window watchdog service at 100 Hz (CPU2), fault detection with edge-triggered reporting
- **TLF35584/35585 compatible:** same SPI interface, SoM-specific INITERR handling

### 26. APML Temperature Monitoring (v0.2)
AMD APML interface over I2C1 (P11.13/P11.14):
- SB-TSI at 0x4C: CPU die temperature (integer + decimal registers)
- SB-RMI at 0x3C: remote management interface, revision readback
- 8-bit SMBus register addressing (distinct from 16-bit HPI protocol on bus 0)

### 27. CRC-32 Module (v0.2)
Shared CRC-32 implementation (binascii.crc32-compatible):
- Full 256-entry lookup table
- Init/Update/Final incremental API
- Used by BIST POST, SOTA transfer, FwUpdate chunk verification

### 28. Recovery Mode (v0.2)
RSTBTN# held at boot:
- UART MUX stays on AURIX
- PowerManager not started (Ryzen stays off)
- Enters dedicated FwUpdate loop
- NvLog event written for recovery entry

### 29. PMC MISC Monitoring (v0.2)
COM-HPC miscellaneous signals monitored with state-change logging:
- RAPID_SHUTDOWN (P20.10)
- LID# (P20.11)
- TAMPER# (P20.12)

---

## Code Structure

```
som_aurix_fw/
├── .project / .cproject       # AURIX Development Studio project descriptors
├── .gitignore
├── Makefile                   # Standalone build (HIGHTEC GCC)
├── CMakeLists.txt             # CMake build system
├── CMakePresets.json           # som-debug/release, eval-debug/release presets
├── Linker/
│   └── tc387.ld               # TC387 linker script with .ipc_shared section
├── iLLD/                      # Infineon iLLD_TC3xx v1.20.0 (not committed)
├── tools/
│   ├── aurix_update.py        # SOTA upload script (Python 3 + pyserial)
│   └── aurix_sideband.py      # APU sideband query tool
└── Src/
    ├── BaseSw/
    │   └── Ifx_Cfg.h          # iLLD top-level config
    └── AppSw/
        ├── Main/
        │   ├── Cpu0_Main.c    # CPU0: orchestrator, OTA, NV, CLI
        │   ├── Cpu1_Main.c    # CPU1: PMC, SysMonitor, UsbPd
        │   ├── Cpu2_Main.c    # CPU2: VoltMon, TLF, FusaSpi
        │   ├── Cpu23_Main.c   # CPU3: sync event, idle
        │   ├── Ipc.h/c        # Inter-core shared memory + commands
        │   ├── SelfTest.h/c   # Comprehensive self-test suite
        │   └── ApuSideband.h/c # Ryzen↔AURIX sideband protocol
        ├── Bsp/
        │   ├── AppPin.h/c     # Toolchain-agnostic GPIO pin reference
        │   ├── Stm_Timer.h/c  # STM0 microsecond/millisecond tick
        │   ├── Uart_Debug.h/c # ASCLIN0 debug UART with RX ISR
        │   ├── Uart_Xfer.h/c  # ASCLIN4 transfer UART (interrupt-driven RX)
        │   ├── I2c_Master.h/c # I2C0/I2C1 master (bus-aware, APML support)
        │   ├── I2c_Slave.h/c  # I2C1 slave driver for APU proxy
        │   └── DebugCli.h/c   # Interactive CLI (20+ commands)
        ├── Platform/
        │   ├── Platform_Cfg.h # All build-time parameters with #ifndef guards
        │   ├── Platform_PinCfg.h/c # GPIO pin assignments (AppPin_t)
        │   ├── Clk_Cfg.h/c   # SCU PLL init — 300 MHz
        │   ├── Port_Init.h/c # GPIO direction/mode init
        │   ├── SysMonitor.h/c # PROCHOT#, CATERR#, SB-TSI temperature
        │   ├── Eru_FaultIsr.h/c # ERU interrupt handlers
        │   └── ComHpcWdt.h/c # COM-HPC watchdog timer
        ├── PowerManager/
        │   ├── PowerManager.h/c     # State machine, fault handling, CF9 suppress
        │   ├── PowerManager_Cfg.h/c # Rail table (AMD 58241 §16)
        │   ├── PwrGood_Mon.h/c      # PG debounce monitor
        │   ├── VoltMon.h/c          # EVADC voltage monitor (19 channels)
        │   └── BiosRom.h/c          # BIOS ROM SPI bus gating
        ├── FwMgmt/
        │   ├── FwUpdate.h/c   # SOTA state machine (SYNC/HEADER/DATA/VERIFY/COMMIT)
        │   ├── BootValid.h/c  # Boot validation + commit logic
        │   ├── Swap.h/c       # PFlash bank swap controller
        │   ├── PFlash.h/c     # Program Flash driver (erase/write/verify)
        │   ├── DFlash.h/c     # Data Flash driver (SOTA metadata, NvLog, config)
        │   ├── NvLog.h/c      # NV event logger (4 rotating DFlash slots)
        │   ├── Crc32.h/c      # CRC-32 (IEEE 802.3, 256-entry table)
        │   └── Bist.h/c       # POST + periodic built-in self-test
        ├── FuSa/
        │   ├── FusaSpi.h/c    # QSPI3 SPI slave register map (96 registers)
        │   └── Tlf35585.h/c   # TLF35584/35585 PMIC driver (SPI + WDT)
        └── UsbPd/
            ├── UsbPd_Cfg.h/c      # CYPD6129 config (DFlash-backed topology)
            ├── UsbPd_Hpi.h/c      # HPI register reader (16-bit I2C addressing)
            ├── UsbPd_ApuProxy.h/c # AMD 40-bit APU register format packing
            ├── UsbPd_Hpd.h/c      # Virtual HPD driver (DP2/DP3)
            ├── UsbPd_Manager.h/c  # Dual-port event dispatch
            └── Cypd6129_Drv.h/c   # Low-level HPI I2C driver
```

## Peripheral Ownership (Multicore)

| Peripheral | Core | Module | Rationale |
|------------|------|--------|-----------|
| ASCLIN0 | CPU0 | Debug UART, CLI | Single writer avoids interleaving |
| ASCLIN4 | CPU0 | Uart_Xfer, FwUpdate, ApuSideband | OTA orchestrated by CPU0 |
| DFlash | CPU0 | NvLog, BootValid, UsbPd_Cfg | Single command interface |
| PFlash | CPU0 | FwUpdate, BIST | Erase/write during OTA |
| I2C0 | CPU1 | UsbPd (CYPD6129) | PD controllers on carrier |
| I2C1 | CPU1 | APML (SB-TSI/SB-RMI), I2C slave | APU sideband |
| ERU | CPU1 | THERMTRIP, WD_STROBE | Callbacks route to PowerManager |
| EVADC | CPU2 | VoltMon | FuSa ADC groups |
| QSPI2 | CPU2 | TLF35585 | WDT service at 100 Hz |
| QSPI3 | CPU2 | FusaSpi slave | Carrier safety controller |
| QSPI0 | None | BiosRom (tri-stated) | Released to Ryzen |

## Prerequisites

### TriCore GCC Toolchain (v4.9.4)
```bash
git clone --depth 1 https://github.com/volumit/tricore_gcc494_linux_bins.git _tc_toolchain_tmp
cd _tc_toolchain_tmp
cat tricore_494_linux.zip.* > tricore_494_linux.zip
mkdir -p ../tools/toolchain
unzip -qo tricore_494_linux.zip -d ../tools/toolchain
chmod -R +x ../tools/toolchain
cd .. && rm -rf _tc_toolchain_tmp
```

### Infineon iLLD (v1.20.0)
```bash
git clone --depth 1 -b V1.20.0 https://github.com/Infineon/illd_release_tc3x.git _illd_tmp
mkdir -p iLLD
cp -r _illd_tmp/src/BaseSw/Infra iLLD/
cp -r _illd_tmp/src/BaseSw/Service iLLD/
for dir in _illd_tmp/src/BaseSw/iLLD/TC3xx/Tricore/*/; do cp -r "$dir" iLLD/; done
rm -rf _illd_tmp
```

## Build

### CMake (recommended)
```bash
cmake --preset som-debug          # Configure for SoM
cmake --build --preset som-debug -j16   # Build
cmake --build --preset som-debug --target flash  # Flash via miniWiggler
```

| Preset | Board | Optimisation | Define |
|--------|-------|-------------|--------|
| som-debug | GP System-on-Module | -O0 -g3 | TARGET_GP_SOM=1 |
| som-release | GP System-on-Module | -O2 | TARGET_GP_SOM=1 |
| eval-debug | Eval Board | -O0 -g3 | TARGET_EVAL_BOARD=1 |
| eval-release | Eval Board | -O2 | TARGET_EVAL_BOARD=1 |

### Makefile
```bash
make BOARD=som -j$(nproc)     # SoM build
make BOARD=eval -j$(nproc)    # Eval board build
make flash                     # Flash via miniWiggler (requires UDAS.exe running)
```

## Firmware Update (Post-Production)

### From Ryzen (ASCLIN4 — production path)
```bash
python3 aurix_update.py -p /dev/ttyS4 -b 115200 -f new_firmware.hex
```

### From Debug UART (ASCLIN0 — development path)
```
aurix> fwupdate
# Then from PC: python3 aurix_update.py -p COM7 -b 115200 -f new_firmware.hex
```

### Recovery Mode
Hold RSTBTN# during power-on. The AURIX enters SOTA update loop without starting the Ryzen.

## Key Timing Parameters (AMD 58241 §16)

| Parameter | Value | AMD 58241 Reference |
|-----------|-------|---------------------|
| Min delay: S5 rails stable → RSMRST_L rising | 10 ms | Table 28, T1 |
| Min delay: RSMRST_L rising → PWR_BTN assertion | 16 ms | Table 28, T1a |
| Min delay: PWR_GOOD rising → RESET_L rising | 28.5 ms | Table 30, T7 |
| Min setup: all rails stable before PWR_GOOD | 5 ms | §16.1.1 |
| PWR_BTN cold-boot pulse width | 18 ms | Table 30, T2/T3 |
| SoC PWROK assertion wait | 100 ms ceiling | Table 30, T5/T6 |
| CF9 reset suppress window | 2000 ms | Implementation |
| THERMTRIP# debounce | 3 polls | Implementation |
| PWRBTN# forced-shutdown hold | 4 s | Implementation |
| Forced-off cooldown | 2 s | Implementation |
| Cold-reset S5 dwell | 3 s | Implementation |

## Configuration Defines

Key compile-time parameters in `Platform_Cfg.h`:

| Define | Purpose |
|--------|---------|
| PM_MAX_RETRIES | Maximum auto-retry attempts on fault before latch-off |
| PM_RETRY_DELAY_MS | Non-blocking delay between retry attempts |
| PM_PWRBTN_HOLD_MS | Hold time for forced shutdown (default 4000 ms) |
| PM_CF9_SUPPRESS_MS | CF9 cold reset SLP_S5 glitch suppress window (default 2000 ms) |
| PM_COLD_RST_PULSE_MS | Minimum COLD_RST assertion width |
| PM_FORCED_OFF_COOLDOWN_MS | Cooldown after forced shutdown (default 2000 ms) |
| PM_S0I3_TIMEOUT_MS | Maximum time in S0i3 before fault |
| FUSA_FEATURE_ENABLE | Enables FuSa status output pins, SPI slave, COM-HPC WDT |
| USBPD_FEATURE_ENABLE | Enables USB PD manager and HPI driver |
| MULTICORE_ENABLE | Enables CPU1/CPU2/CPU3 multicore architecture |
| VOLTMON_SMA_ENABLE | Enables SMA filter on VoltMon ADC readings |
| TARGET_EVAL_BOARD | Selects eval board ADC channel table and excludes SoM peripherals |
| SYSMON_CARRIER_WD_ENABLE | Enables COM-HPC carrier watchdog integration |
| MAIN_LOOP_PERIOD_MS | Main loop pacing period |
| MAIN_LOOP_OVERRUN_LOG_INTERVAL_MS | Minimum interval between overrun log messages |

## References

| Document | Description |
|---|---|
| Infineon **iLLD\_TC3xx v1.20.0** | Low Level Driver library for AURIX TC3xx — required for build. |
| Infineon **CYPD6129 HPI Specification** (002-24049) | CCGx Host Processor Interface register map used by `Cypd6129_Drv.c`. |
| **PICMG COM-HPC Specification** Rev 1.0 | Defines COM-HPC module power management signals (PWRGD, RSMRST, SLP\_S\*, PROCHOT\#, CATERR\#, PLTRST\#, RSMRST\_OUT\#). |
