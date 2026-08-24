# som_aurix_fw — TC387 COM-HPC Platform Controller

Bare-metal firmware for the **Infineon AURIX TC387** acting as the module-side
platform management controller on a COM-HPC System-on-Module (SOM) hosting the
**AMD Strix Halo (FP11) x86 SoC**.

---

## Firmware Functionality Overview

### 1. Power Supply Sequencing

The TC387 manages the complete power-on and power-down sequencing of the AMD FP11
SoC platform in compliance with **AMD Electrical Data Sheet 58241, Section 16
("Power Supplies and Signal Sequencing")**.

Power rails are organised into five stages mapped to AMD's Group A–D power
sequencing model:

| Stage | SOM Enable Signal | Rails Controlled | AMD Group |
|---|---|---|---|
| 0 — EFUSE | `MAIN_12V_EFUSE_EN` | 12V\_MAIN path to SOM | COM-HPC pre-condition |
| 0a — VR3V3 | `VR_APU_3V3_EN` | VR\_APU\_3V3 standby rail | Group A (standby) |
| 1 — Group B | `PWR_GROUP_B_EN` | VDD\_MISC\_S5, VDD\_12\_S5, VDD\_18\_S5, VDDIO\_33\_S5 | Group B (S5) |
| 2 — Group C | `PWR_GROUP_C_EN` | VDD\_MEM, VDDIO\_MEM, VDD\_MEMQ (Ch A & B) | Group C (S3/S0) |
| 3 — Group D | `PWR_GROUP_D_EN` | VDDCR (via MP2825A / MP86979) | Group D (S0) |

Sequencing enforces the following per AMD 58241 §16.1.2:

- **12 V EFUSE** enabled first; PG verified before proceeding.
- **VR\_APU\_3V3** (Group A standby rail) ramped and verified stable before
  Group B enable.
- **Group B** fully stable before Group C exceeds 10 % of nominal.
- **Group C** fully stable before Group D exceeds 10 % of nominal.
- All Group D rails stable **≥ 1 ms** before `APU_PWR_GOOD` assertion
  (5 ms deglitch implemented; §16.1.1).
- `RSMRST_L` deasserted **≥ 10 ms** after Group B stable (AMD Table 28 T1).
- RTCCLK stabilisation window honoured (T1a: ≥ 16 ms RSMRST to PWR\_BTN).
- `COLD_RST` (SYS\_RESET\_L) held asserted for **≥ 28.5 ms after** `APU_PWR_GOOD`
  assertion before release (AMD Table 30 T7 / §16.1.5 point 3).
- SoC `APU_PWROK` polled after PWRGD assertion with a 100 ms ceiling
  (AMD T5/T6: 21.4 ms typical). Failure to assert triggers a startup fault.

All individual rail power-good (`_PG`) signals are verified at each stage.
A configurable timeout (per-rail, default 50–100 ms) and debounced PG monitoring
during `PM_STATE_ON` detect rail failures and initiate an emergency shutdown.

**Upstream PG verification**: at each ramp stage the firmware re-verifies all
previously-ramped groups remain stable. If any upstream rail drops PG during a
later stage, the sequencer faults immediately rather than continuing with a
partially-powered platform.

### 2. Power State Machine

The firmware implements a non-blocking cooperative state machine driven by
`PowerManager_Run()`, called from the main loop. States include:

| State | Description |
|---|---|
| `PM_STATE_OFF` | All rails disabled. Awaiting power-on request. |
| `PM_STATE_POWER_UP` | Transient entry state; disables VoltMon and begins ramp. |
| `PM_STATE_RAMP_ALW` | Ramping 12 V EFUSE (always-on stage). |
| `PM_STATE_RAMP_VR3V3` | Ramping VR\_APU\_3V3 standby rail. |
| `PM_STATE_RAMP_S5` | Ramping Group B (S5 rails), releasing RSMRST\_L and pulsing PWR\_BTN. |
| `PM_STATE_RAMP_S3` | Ramping Group C (memory rails). |
| `PM_STATE_RAMP_S0` | Ramping Group D (VDDCR core), BIOS ROM validation, PWROK wait. |
| `PM_STATE_ON` | System fully powered. Monitoring PWROK, SLP signals, THERMTRIP, RSTBTN#. |
| `PM_STATE_DN_S0_S3` | Powering down Group D (and Group C if full shutdown). |
| `PM_STATE_DN_S3_S5` | Waiting for S0i3 wake event or completing soft shutdown to S5. |
| `PM_STATE_S5` | Parked at S5 (Group B + EFUSE on). Awaiting wake or cold-reset dwell. |
| `PM_STATE_DN_S5_OFF` | Powering down Group B and EFUSE to reach OFF. |
| `PM_STATE_FAULT` | Fault with optional auto-retry (non-blocking delay) or latch-off. |
| `PM_STATE_WARM_RESET` | KBRST\_L asserted without dropping rails; re-validates BIOS ROM. |

### 3. S0i3 (Suspend-to-RAM) Support

The firmware supports AMD S0i3 low-power suspend via SLP\_S3 signalling:

- **Entry**: when SLP\_S3 asserts in `PM_STATE_ON`, the firmware disables
  Group D while keeping Group C (memory) powered, then transitions to
  `PM_STATE_DN_S3_S5` to wait for a wake event.
- **Wake sources**: physical power-button press or autonomous SLP\_S3
  deassertion (Wake-on-LAN, RTC alarm, etc.). On autonomous wake the
  firmware skips the PWR\_BTN pulse since the chipset has already initiated
  its own wake sequence.
- **T1' hold-off**: a configurable minimum entry time (`PM_T1_PRIME_MS`)
  suppresses false wake-edge evaluation immediately after S0i3 entry.
- **Safety checks during suspend**: upstream PG is periodically re-verified,
  and a configurable S0i3 timeout (`PM_S0I3_TIMEOUT_MS`) faults out if no
  wake event arrives within the limit.

### 4. Fault Handling and Retry Logic

On any power-good timeout, PG loss, or voltage-monitor fault:

1. All reset and power-down signals are asserted (COLD\_RST, KBRST\_L,
   RSMRST\_L, PWRGD deasserted), all rails disabled in reverse order.
2. If retries remain (`PM_MAX_RETRIES`, configurable), the firmware enters
   `PM_STATE_FAULT` with a **non-blocking** retry delay (`PM_RETRY_DELAY_MS`)
   so that THERMTRIP, VoltMon, and USB PD servicing continue unblocked
   during the wait.
3. After the delay expires, the state machine re-enters `PM_STATE_POWER_UP`
   for a full re-sequencing attempt.
4. If all retries are exhausted, the firmware latches off in
   `PM_STATE_FAULT` until a physical power cycle.

Fault causes are tracked via `PM_ResetCause_t`:
`PG_TIMEOUT`, `PG_LOSS`, `THERMAL`, `HOST_REQUEST`, `VOLTAGE`,
`BIOS_FAIL`, `COLD_RST`.

### 5. Power Button Handling

`CB_PWRBTN#` (active low) is debounced and supports two actions:

- **Short press** (< 4 s): in `PM_STATE_ON`, forwarded to the APU as an
  18 ms `APU_PWRBTN` pulse. In OFF or S5 states, triggers a power-on
  sequence.
- **Long press** (≥ 4 s, `PM_PWRBTN_HOLD_MS`): forced emergency shutdown
  from any active state, transitioning directly to `PM_STATE_OFF`.
- **Forced-off cooldown**: after a forced shutdown, power-on requests are
  suppressed for 2 s (`PM_FORCED_OFF_COOLDOWN_MS`) to reject button bounce.

### 6. Reset Button and Cold Reset

`CB_RSTBTN#` (active low) triggers a cold reset while in `PM_STATE_ON`:

- Debounced with `PM_RSTBTN_DEBOUNCE_POLLS` consecutive low reads.
- On confirmed press, `COLD_RST` is asserted for a minimum of
  `PM_COLD_RST_PULSE_MS`. If `CB_RSTBTN#` is still held, `COLD_RST`
  remains asserted until the button is released and the minimum pulse
  width has elapsed.
- No rail or state changes occur — the APU performs its own internal reset.
  PG-loss monitoring stays armed throughout.

**CF9-style cold reset detection**: the firmware observes `APU_RESET_L`
assertion from the SoC. If `SLP_S5` asserts within 500 ms of a detected
`APU_RESET_L` pulse, the event is classified as a CF9 cold reset. The
platform parks at S5 for a 3-second dwell before automatically
re-powering (Group C + D re-ramp).

### 7. THERMTRIP# Handling

`THERMTRIP_L` (active low) is monitored via two mechanisms:

- **ISR path**: `PowerManager_OnThermtripIsr()` sets a flag atomically
  (interrupt-safe). The main loop captures and clears this flag under a
  brief critical section (2-cycle interrupt disable window).
- **Polled debounce**: `PM_PG_DEBOUNCE_POLLS` consecutive active reads
  in `PM_STATE_ON` (with PWROK valid) trigger the THERMTRIP handler.

On confirmed assertion the platform suspends to **S5**: Group D and Group C
are powered down; Group B and the 12 V EFUSE remain active so the SOM stays
visible to the carrier. Recovery is permitted only after a valid wake event
(power button or API call).

### 8. Input Voltage Monitoring

`VIN_PWR_OK` is checked on every main-loop iteration. If it drops while the
system is powered (any state except OFF and FAULT), an immediate emergency
shutdown is triggered with all rails disabled and state set to OFF.

### 9. BIOS ROM Validation

Before releasing `COLD_RST` on cold boot (and during warm reset), the
firmware reads the `BSEL[2:0]` boot-select straps, asserts the SPI MUX to
take ownership of the BIOS ROM flash, and runs a validation check. If
validation fails, the boot is blocked and a fault is raised.

The AMD-defined validation method is TBD — the current implementation is a
pass-through stub that always returns success. The SPI MUX is released back
to the APU after validation completes.

### 10. Analog Voltage Monitoring (VoltMon)

The `VoltMon` module provides continuous ADC-based monitoring of all SOM
power rails using the TC387 EVADC peripheral:

- **ADC configuration**: 5.0 V external precision VREF, resistor-divider
  scaling (dividerScale = 1000 for all channels), queue-based conversion
  across multiple EVADC groups.
- **Monitored rails** (GP SOM target): VDDCR, VDDCR\_CCD, VDDCR\_SOC,
  VDDCR\_SR (Group 0); VDD\_MEM\_A/B, VDD\_MEMQ\_A/B, VDDIO\_MEM\_A/B
  (Groups 1–2); VDD\_MISC/\_S5, VDD\_1V2/\_S5, VDD\_1V8/\_S5 (Group 3);
  VDDIO\_3V3/\_S5, VDDIO\_AUDIO (Group 4).
- **Thresholds**: per-channel under-voltage and over-voltage thresholds at
  both WARNING (±8 %) and FAULT (±10 %) levels, configurable per rail.
  VID rails (VDDCR, VDDCR\_CCD) use OV-only thresholds since their voltage
  is dynamically set by the SoC.
- **SMA filtering**: optional simple moving average filter
  (`VOLTMON_SMA_ENABLE`, configurable tap count) to reject transient ADC
  noise before threshold comparison.
- **Fault integration**: FAULT-severity events invoke a registered callback
  (`PowerManager_OnVoltageFault`), which triggers the same PG-fault
  shutdown/retry path used by rail-level PG loss.
- **Lifecycle**: VoltMon is disabled during power sequencing to suppress
  false faults from rails that are intentionally off, and enabled only
  after `PM_STATE_ON` is reached. It is disabled again on any shutdown,
  fault, or sleep transition.
- **Eval board support**: a separate channel table with wide-open thresholds
  is provided for the TRB eval board (`TARGET_EVAL_BOARD`) to validate the
  EVADC driver path on boards without SOM power rails.

### 11. COM-HPC Interface Signals

The firmware manages the following COM-HPC module-to-carrier signals:

| Signal | Direction | Function |
|---|---|---|
| `VIN_PWR_OK` | In | Guards power-on; sequencing blocked until carrier confirms 12 V. Emergency shutdown if lost while powered. |
| `CB_PWRBTN#` | In | Debounced power button: short press = on/forward, long hold = forced shutdown |
| `CB_RSTBTN#` | In | Debounced reset button: triggers COLD\_RST pulse (held while button held) |
| `SLP_S3`, `SLP_S5` | In (from APU) | ACPI sleep-state transitions (S0i3, soft shutdown) |
| `APU_PWROK` | In (from APU) | SoC power-good feedback; polled during ramp and monitored in S0 |
| `APU_RESET_L` | In (from APU) | SoC reset status; observed for CF9 cold-reset detection |
| `APU_PWR_GOOD` | Out | Asserted when all power groups are stable (5 ms deglitch) |
| `COLD_RST` (`SYS_RESET_L`) | Out | APU system reset — released after PWRGD hold time |
| `WARM_RST` (`KBRST_L`) | Out | APU keyboard reset — warm reset path |
| `MMC_RSMRST_L` | Out | APU resume reset — deasserted after S5 rails stable + 10 ms |
| `PLTRST_L` | Out | Platform reset to carrier; mirrors SoC `RESET_L` per COM-HPC spec |
| `RSMRST_OUT_L` | Out | Mirrors AURIX `RSMRST_L` to COM-HPC carrier |
| `PROCHOT#` | Out | Asserted LOW when APU thermal event detected (COM-HPC) |
| `CATERR#` | Out | Catastrophic error indicator to carrier (default HIGH) |

### 12. COM-HPC Signal Mirroring

Per the COM-HPC specification, the firmware continuously mirrors two signals
from the module to the carrier on every main-loop iteration:

- **PLTRST#**: mirrors SoC `APU_RESET_L` → COM-HPC `PLTRST_L`. Held
  asserted when the state machine is below `PM_STATE_RAMP_S3` or in
  `PM_STATE_FAULT` (RESET\_L is indeterminate in these windows). Per
  COM-HPC spec, PLTRST# is not released while `CB_RSTBTN#` is low.
- **RSMRST\_OUT#**: mirrors AURIX `MMC_RSMRST_L` read-back → COM-HPC
  `RSMRST_OUT_L`.

### 13. FuSa Status Outputs

Two-bit FuSa status field to the carrier FuSa Safety Controller
(conditional on `FUSA_FEATURE_ENABLE`):

| `FUSA_STATUS[1:0]` | State | Condition |
|---|---|---|
| `00` | Power off | `MAIN_12V_EFUSE_EN` not asserted |
| `01` | Power good | System fully powered, `APU_PWR_GOOD` asserted |
| `10` | Fault | PG loss, PG timeout, voltage fault, or THERMTRIP emergency shutdown |
| `11` | Reset | SoC platform in reset (all ramp, power-up, power-down, and S5-suspend states) |

FuSa status is updated on every state transition. When `FUSA_FEATURE_ENABLE`
is disabled, the status pins are not driven.

### 14. COM-HPC Watchdog (ComHpcWdt)

A COM-HPC-compliant watchdog timer is enabled after the system reaches
`PM_STATE_ON` (configurable enable delay and timeout via
`COMHPC_WDT_DEFAULT_ENABLE_DELAY_S` and `COMHPC_WDT_DEFAULT_TIMEOUT_MS`).
The watchdog is disabled on any shutdown, fault, sleep, or THERMTRIP
transition to prevent false watchdog faults during expected power-state
changes.

### 15. PROCHOT / CATERR Management

`APU_PROCHOT_L` (P11.9) is configured as **open-drain** output so both the
TC387 and the APU can assert it independently without bus contention. Any LOW
observed on the pad (APU thermal throttle request or TC387-initiated throttle)
is propagated to the COM-HPC `PROCHOT#` output (P2.10) within 5 ms.

`CATERR#` (P2.11) is driven HIGH by default and can be asserted by calling
`SysMonitor_AssertCaterr()` on detection of a catastrophic platform fault.

Future: AMD APML interface (I2C1, P11.13/14) will provide telemetry-based
throttle control via `SysMonitor_AssertApuProchot()`.

### 16. UART MUX Control

The ASCLIN0 debug UART (P14.0 TX / P14.1 RX) is shared between the AURIX
and the x86 SoC via a board-level multiplexer controlled by `UART_MUX_SEL`
(P14.6):

| `UART_MUX_SEL` | Owner | Period |
|---|---|---|
| `1` (HIGH) | AURIX | Power-on through `SYS_RESET_L` release |
| `0` (LOW) | x86 SoC | After `SYS_RESET_L` is deasserted |

The AURIX holds the UART from the moment the firmware starts, ensuring all
power-sequencing diagnostic output is available on the shared connector during
bring-up.  The MUX switches to the x86 SoC immediately before `COLD_RST`
(`SYS_RESET_L`) is deasserted so the SoC owns the line from its first boot
cycle.  If `COLD_RST` is re-asserted (power-down, THERMTRIP, or fault), the
AURIX reclaims the MUX instantly so diagnostic messages remain visible while
the platform is not running.

### 17. USB PD Sideband Management

Two **Infineon CYPD6129** USB PD controllers are managed via I2C0 (400 kHz,
P13.1 SCL / P13.2 SDA) using the CCGx HPI (Host Processor Interface) register
protocol. Both devices share a wired-OR `USBC_PD_ALERT_L` interrupt line
(P10.7); the manager polls this at 5 ms intervals and services both I2C
addresses on any assertion.

Events handled: Type-C attach/detach, PD contract negotiation, VBUS
over-voltage, VBUS over-current.

`DP2_HPD` (P13.0) and `DP3_HPD` (P13.3) virtual HPD outputs are driven for
DisplayPort Alt-mode signalling.

---

## Code Structure

```
som_aurix_fw/
├── .project               # AURIX Development Studio — Makefile project descriptor
├── .cproject              # CDT indexer / IntelliSense include paths
├── .gitignore
├── Makefile               # Standalone build (HIGHTEC GCC, tricore-gcc)
├── Linker/
│   └── tc387.ld           # TC387 linker script (obtain from iLLD board package)
├── iLLD/                  # Infineon iLLD_TC3xx v1.20.0 — NOT committed; see below
└── Src/
    ├── BaseSw/
    │   └── Ifx_Cfg.h      # iLLD top-level config (MCU variant, XTAL, PLL)
    └── AppSw/
        ├── Main/
        │   ├── Cpu0_Main.c        # CPU0 entry point and main loop
        │   └── CpuIdle.c          # CPU1/2/3 idle stubs (required by iLLD startup)
        ├── Bsp/
        │   ├── AppPin.h/c         # Toolchain-agnostic GPIO pin reference type
        │   ├── Stm_Timer.h/c      # STM0 microsecond/millisecond tick
        │   ├── Uart_Debug.h/c     # ASCLIN0 blocking-write debug UART (P14.0/P14.1)
        │   └── I2c_Master.h/c     # I2C0 master wrapper (iLLD 1.20.0 write2/read2 API)
        ├── Platform/
        │   ├── Platform_Cfg.h     # Timing constants, I2C addresses, rail counts
        │   ├── Platform_PinCfg.h  # GPIO pin assignments (extern const AppPin_t)
        │   ├── Platform_PinCfg.c  # Pin definitions — all port/pin indices
        │   ├── Clk_Cfg.h/c        # SCU PLL init — 300 MHz from 20 MHz XTAL
        │   ├── Port_Init.h/c      # GPIO direction/mode init for all board signals
        │   └── SysMonitor.h/c     # PROCHOT# propagation, CATERR# default drive
        ├── PowerManager/
        │   ├── PowerManager.h     # State machine public API and PM_State_t enum
        │   ├── PowerManager.c     # Sequencing state machine, THERMTRIP handler,
        │   │                      # FuSa status drive, S0i3, cold/warm reset,
        │   │                      # retry logic, voltage-fault integration
        │   ├── PowerManager_Cfg.h # PwrRail_Cfg_t struct, rail count defines,
        │   │                      # extern rail table declarations
        │   ├── PowerManager_Cfg.c # Rail table definitions + runtime init
        │   │                      # (AMD 58241 §16-compliant sequencing order)
        │   ├── PwrGood_Mon.h      # Continuous PG polling interface
        │   ├── PwrGood_Mon.c      # PG debounce monitor (3-poll debounce)
        │   ├── VoltMon.h          # Analog voltage monitoring interface
        │   ├── VoltMon.c          # EVADC-based rail voltage monitor with
        │   │                      # per-channel UV/OV thresholds, SMA filter,
        │   │                      # and fault callback
        │   └── ComHpcWdt.h/c      # COM-HPC watchdog timer driver
        └── UsbPd/
            ├── UsbPd_Cfg.h/c      # CYPD6129 device config table
            ├── Cypd6129_Drv.h/c   # HPI register-level I2C driver
            └── UsbPd_Manager.h/c  # Dual-port event dispatch loop
```

---

## Quick Start 

```bash
# Prerequisites (one-time)
sudo apt install cmake ninja-build unzip

# Clone
git clone https://github.com/AMD-AECG-SSW-PUBLIC/som_aurix_fw
cd som_aurix_fw

# Configure — automatically fetches compiler and iLLD
cmake --preset som-debug

# Build
cmake --build --preset som-debug -j16

# Flash (WSL only)
cmake --build --preset som-debug --target flash
```

That's it. The first `cmake --preset` run downloads the TriCore GCC 4.9.4 toolchain and Infineon iLLD v1.20.0 automatically. Subsequent runs use the cached copies.

## Build Presets

| Preset | Board | Optimisation | Define |
|---|---|---|---|
| `som-debug` | GP System-on-Module | `-O0 -g3` | `TARGET_GP_SOM=1` |
| `som-release` | GP System-on-Module | `-O2` | `TARGET_GP_SOM=1` |
| `eval-debug` | Eval Board | `-O0 -g3` | `TARGET_EVAL_BOARD=1` |
| `eval-release` | Eval Board | `-O2` | `TARGET_EVAL_BOARD=1` |

Usage:

```bash
cmake --preset <preset>
cmake --build --preset <preset> -j16
```

The eval board preset automatically excludes UsbPd sources and selects the LFBGA292 pin map.


## Build Commands Reference

```bash
# Configure
cmake --preset som-debug

# Build
cmake --build --preset som-debug -j16

# Clean (keeps config, removes build artifacts)
cmake --build --preset som-debug --target clean

# Full clean (requires re-configure)
rm -rf build/

# Section sizes
cmake --build --preset som-debug --target size

# Disassembly listing
cmake --build --preset som-debug --target disasm
```

## Project Structure

```
som_aurix_fw/
├── CMakeLists.txt          # Build system (replaces Makefile)
├── CMakePresets.json        # Named build configurations
├── Makefile                 # Legacy build (kept as fallback)
├── cmake/
│   └── tricore-gcc.cmake   # Toolchain file — auto-downloads compiler
├── Src/
│   ├── AppSw/              # Application software
│   │   ├── Main/           # Entry point, CPU idle
│   │   ├── Bsp/            # Board support (UART, I2C, SPI, TLF, pins)
│   │   ├── Platform/       # Clock, ERU, FuSa SPI, watchdog
│   │   ├── PowerManager/   # Power sequencing state machine
│   │   ├── UsbPd/          # USB-PD / CYPD6129 (SoM only)
│   │   └── FwMgmt/         # Firmware update, PFlash, NvLog
│   └── BaseSw/             # Base software config
├── Linker/
│   └── tc387.ld            # Linker script (TC387 memory map)
├── Image/                  # Board images / documentation assets
└── tools/
    └── aurix_update.py     # Update utility
```

## Dependencies

All dependencies are fetched automatically on first configure.

**TriCore GCC 4.9.4** — Cross-compiler toolchain. Downloaded from [volumit/tricore_gcc494_linux_bins](https://github.com/volumit/tricore_gcc494_linux_bins) and extracted to `tools/toolchain/` (gitignored).

**Infineon iLLD v1.20.0** — Low Level Driver library. Cloned from [Infineon/illd_release_tc3x](https://github.com/Infineon/illd_release_tc3x) (tag `V1.20.0`), restructured into `iLLD/` (gitignored).

**AURIXFlasher** — Flash programming tool for miniWiggler. Bundled in `tools/flasher/`. This is a Windows-only tool from Infineon; native Linux flashing is not available at this time. On WSL, the flash target calls the .exe directly and handles path translation automatically.

## Prerequisites

- **CMake** ≥ 3.20
- **Ninja** build system
- **unzip** (for toolchain extraction)
- **Git** (for dependency fetching)
- **WSL** (for flashing only)

Install on Ubuntu/Debian:

```bash
sudo apt install cmake ninja-build unzip git
```

Fedora / RHEL:

```bash
sudo dnf install cmake ninja-build unzip git
```

Arch Linux:

```bash
sudo pacman -S cmake ninja unzip git
```

---

## Key Timing Parameters (AMD 58241 §16)

| Parameter | Value | AMD 58241 Reference |
|---|---|---|
| Min delay: S5 rails stable → RSMRST\_L rising | **10 ms** | Table 28, T1 |
| Min delay: RSMRST\_L rising → PWR\_BTN assertion | **16 ms** | Table 28, T1a (RTCCLK stable) |
| Min delay: PWR\_GOOD rising → RESET\_L rising | **28.5 ms** | Table 30, T7 |
| Min setup: all rails stable before PWR\_GOOD | **1 ms** (5 ms implemented) | §16.1.1 |
| PWR\_BTN cold-boot pulse width | **18 ms** (16 ms min) | Table 30, T2/T3 |
| PWR\_BTN S0i3-resume pulse width | **18 ms** (16 ms min) | Table 30, T2 |
| SoC PWROK assertion wait | **100 ms** ceiling (21.4 ms typical) | Table 30, T5/T6 |
| Group A stable before Group B > 10 % | Required | §16.1.2 |
| Group B stable before Group C > 10 % | Required | §16.1.2 |
| Group C stable before Group D > 10 % | Required | §16.1.2 |
| THERMTRIP# debounce | 3 consecutive polls | Implementation |
| PG fault debounce | 3 consecutive polls | Implementation |
| PWRBTN# debounce | Configurable polls | Implementation |
| RSTBTN# debounce | Configurable polls | Implementation |
| PWRBTN# forced-shutdown hold time | **4 s** | Implementation |
| Forced-off cooldown (bounce reject) | **2 s** | Implementation |
| Cold-reset S5 dwell time | **3 s** | Implementation |
| S0i3 minimum entry time (T1') | Configurable (`PM_T1_PRIME_MS`) | AMD datasheet |

---

## Configuration Defines

Key compile-time parameters in `Platform_Cfg.h` and `PowerManager_Cfg.h`:

| Define | Purpose |
|---|---|
| `PM_MAX_RETRIES` | Maximum auto-retry attempts on fault before latch-off |
| `PM_RETRY_DELAY_MS` | Non-blocking delay between retry attempts |
| `PM_PWRBTN_HOLD_MS` | Hold time for forced shutdown (default 4000 ms) |
| `PM_PWRBTN_DEBOUNCE_POLLS` | Debounce poll count for power button |
| `PM_RSTBTN_DEBOUNCE_POLLS` | Debounce poll count for reset button |
| `PM_PG_DEBOUNCE_POLLS` | Debounce poll count for PG-loss and THERMTRIP |
| `PM_COLD_RST_PULSE_MS` | Minimum COLD\_RST assertion width |
| `PM_FORCED_OFF_COOLDOWN_MS` | Cooldown after forced shutdown (default 2000 ms) |
| `PM_S0I3_TIMEOUT_MS` | Maximum time in S0i3 before fault |
| `PM_T1_PRIME_MS` | Minimum S0i3 entry time before evaluating wake edge |
| `PM_SLP_S3_TIMEOUT_MS` | Timeout waiting for SLP signal deassertion |
| `FUSA_FEATURE_ENABLE` | Enables FuSa status output pins and COM-HPC WDT |
| `VOLTMON_SMA_ENABLE` | Enables SMA filter on VoltMon ADC readings |
| `VOLTMON_SMA_TAPS` | SMA filter tap count (power of 2) |
| `TARGET_EVAL_BOARD` | Selects TRB eval-board ADC channel table |

---

## References

| Document | Description |
|---|---|
| AMD Publication **58241** Rev 0.50 (Jul 2023) | *Electrical Data Sheet for AMD Family 1Ah Models 70h-77h Processors* — power sequencing requirements in **Section 16**. Obtain under NDA from your AMD representative. |
| AMD Publication **58023** | *Infrastructure Roadmap (IRM) for FP11 Processors* — power supply specifications referenced by AMD 58241 §16.1.1. |
| Infineon **iLLD\_TC3xx v1.20.0** | Low Level Driver library for AURIX TC3xx — required for build. |
| Infineon **CYPD6129 HPI Specification** (002-24049) | CCGx Host Processor Interface register map used by `Cypd6129_Drv.c`. |
| **PICMG COM-HPC Specification** Rev 1.0 | Defines COM-HPC module power management signals (PWRGD, RSMRST, SLP\_S\*, PROCHOT\#, CATERR\#, PLTRST\#, RSMRST\_OUT\#). |
