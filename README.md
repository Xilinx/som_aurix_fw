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

Power rails are organised into four stages mapped to AMD's Group A–D power
sequencing model:

| Stage | SOM Enable Signal | Rails Controlled | AMD Group |
|---|---|---|---|
| 0 — EFUSE | `MAIN_12V_EFUSE_EN` | 12V\_MAIN path to SOM | COM-HPC pre-condition |
| 1 — Group B | `PWR_GROUP_B_EN` | VDD\_MISC\_S5, VDD\_12\_S5, VDD\_18\_S5, VDDIO\_33\_S5 | Group B (S5) |
| 2 — Group C | `PWR_GROUP_C_EN` | VDD\_MEM, VDDIO\_MEM, VDD\_MEMQ (Ch A & B) | Group C (S3/S0) |
| 3 — Group D | `PWR_GROUP_D_EN` | VDDCR (via MP2825A / MP86979) | Group D (S0) |

Sequencing enforces the following per AMD 58241 §16.1.2:

- **Group A** (VR\_APU\_3V3) verified stable before EFUSE enable.
- **Group B** fully stable before Group C exceeds 10 % of nominal.
- **Group C** fully stable before Group D exceeds 10 % of nominal.
- All Group D rails stable **≥ 1 ms** before `APU_PWR_GOOD` assertion
  (5 ms deglitch implemented; §16.1.1).
- `RSMRST_L` deasserted **≥ 10 ms** after Group B stable (AMD Table 28 T1).
- `COLD_RST` (SYS\_RESET\_L) held asserted for **≥ 28.5 ms after** `APU_PWR_GOOD`
  assertion before release (AMD Table 30 T7 / §16.1.5 point 3).

All individual rail power-good (`_PG`) signals are verified at each stage.
A configurable timeout (per-rail, default 50–100 ms) and debounced PG monitoring
during `PM_STATE_ON` detect rail failures and initiate an emergency shutdown.

### 2. COM-HPC Interface Signals

The firmware manages the following COM-HPC module-to-carrier signals:

| Signal | Direction | Function |
|---|---|---|
| `VIN_PWR_OK` | In | Guards power-on; sequencing blocked until carrier confirms 12 V |
| `CB_PWRBTN#` | In | Initiates power-on/off sequence |
| `SLP_S3`, `SLP_S5` | In (from APU) | ACPI sleep-state transitions |
| `APU_PWR_GOOD` | Out | Asserted when all power groups are stable |
| `COLD_RST` | Out | APU `SYS_RESET_L` — released after PWRGD hold time |
| `WARM_RST` | Out | APU `KBRST_L` — warm reset path |
| `MMC_RSMRST_L` | Out | APU `RSMRST_L` — resume reset, deasserted after S5 rails stable |
| `PLTRST_L` | Out | Platform reset to carrier peripherals (USB PD re-timers etc.) |
| `PROCHOT#` | Out | Asserted LOW when APU thermal event detected (COM-HPC) |
| `CATERR#` | Out | Catastrophic error indicator to carrier (default HIGH) |

### 3. THERMTRIP# Handling

`THERMTRIP_L` (P10.3, active low) is monitored with a 3-poll software debounce
on every main-loop iteration. On confirmed assertion:

- Platform suspends to **S5**: Group D and Group C are powered down; Group B
  and the 12 V EFUSE remain active so the SOM remains visible to the carrier.
- `COLD_RST` and `RSMRST_L` are held asserted.
- Recovery (re-enable Group C/D) is permitted only after `THERMTRIP_L`
  deasserts **and** a valid wake event (power button or API call) occurs.

### 4. USB PD Sideband Management

Two **Infineon CYPD6129** USB PD controllers are managed via I2C0 (400 kHz,
P13.1 SCL / P13.2 SDA) using the CCGx HPI (Host Processor Interface) register
protocol. Both devices share a wired-OR `USBC_PD_ALERT_L` interrupt line
(P10.7); the manager polls this at 5 ms intervals and services both I2C
addresses on any assertion.

Events handled: Type-C attach/detach, PD contract negotiation, VBUS
over-voltage, VBUS over-current.

`DP2_HPD` (P13.0) and `DP3_HPD` (P13.3) virtual HPD outputs are driven for
DisplayPort Alt-mode signalling.

### 5. FuSa Status Outputs

Two-bit FuSa status field to the carrier FuSa Safety Controller:

| `FUSA_STATUS[1:0]` | State | Condition |
|---|---|---|
| `00` | Power off | `MAIN_12V_EFUSE_EN` not asserted |
| `01` | Power good | System fully powered, `APU_PWR_GOOD` asserted |
| `10` | Fault | PG loss, PG timeout, or THERMTRIP emergency shutdown |
| `11` | Reset | SoC platform in reset (all ramp, power-down, and S5-suspend states) |

### 6. PROCHOT / CATERR Management

`APU_PROCHOT_L` (P11.9) is configured as **open-drain** output so both the
TC387 and the APU can assert it independently without bus contention. Any LOW
observed on the pad (APU thermal throttle request or TC387-initiated throttle)
is propagated to the COM-HPC `PROCHOT#` output (P2.10) within 5 ms.

`CATERR#` (P2.11) is driven HIGH by default and can be asserted by calling
`SysMonitor_AssertCaterr()` on detection of a catastrophic platform fault.

Future: AMD APML interface (I2C1, P11.13/14) will provide telemetry-based
throttle control via `SysMonitor_AssertApuProchot()`.

### 7. UART MUX Control

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
        │   │                      # FuSa status drive
        │   ├── PowerManager_Cfg.h # PwrRail_Cfg_t struct, rail count defines,
        │   │                      # extern rail table declarations
        │   ├── PowerManager_Cfg.c # Rail table definitions + runtime init
        │   │                      # (AMD 58241 §16-compliant sequencing order)
        │   ├── PwrGood_Mon.h      # Continuous PG polling interface
        │   └── PwrGood_Mon.c      # PG debounce monitor (3-poll debounce)
        └── UsbPd/
            ├── UsbPd_Cfg.h/c      # CYPD6129 device config table
            ├── Cypd6129_Drv.h/c   # HPI register-level I2C driver
            └── UsbPd_Manager.h/c  # Dual-port event dispatch loop
```

---

## Dependencies

### AURIX Development Studio (ADS)

- **Version**: 1.10 or later
- **Download**: [Infineon AURIX Development Studio](https://www.infineon.com/aurixdevelopmentstudio)
- **Toolchain**: TASKING VX-toolset for TriCore (bundled with ADS)
  - Compiler: `cctc` (C TriCore Compiler)
  - Linker: `ltc`
  - Builder: `amk`

### iLLD — Infineon Low Level Drivers

- **Version required**: `iLLD_TC3xx v1.20.0`
- **Obtain from**: Infineon MyICP / AURIX partner portal
- **Install path**: unpack into `iLLD/` at the project root
  (excluded from this repository by `.gitignore`)

### Target Device

- **MCU**: Infineon AURIX TC387 (TC38xA family)
- **Crystal**: 20 MHz (configured in `Src/BaseSw/Ifx_Cfg.h`)
- **CPU clock**: 300 MHz (configured via PLL in `Src/AppSw/Platform/Clk_Cfg.c`)

---

## How to Build

### Option A — AURIX Development Studio (recommended)

1. **Clone this repository**
   ```bash
   git clone https://github.com/AMD-AECG-SSW-PUBLIC/som_aurix_fw
   ```

2. **Unpack iLLD** into the project root:
   ```
   som_aurix_fw/
   └── iLLD/        ← unpack iLLD_TC3xx_1_20_0 here
   ```

3. **Obtain the TC387 linker script** from the iLLD board package or HIGHTEC
   examples and place it at `Linker/tc387.ld`.

4. **Import into ADS**
   ```
   File -> Import -> General -> Existing Projects into Workspace
   -> Browse to: som_aurix_fw/
   -> Finish
   ```

5. **Configure source folders** (if not resolved automatically)
   ```
   Right-click project -> Properties -> C/C++ General -> Source Location
   -> Confirm Src/AppSw/* folders are listed
   ```

6. **Fill in GPIO pin assignments** (if board schematic differs)
   Open `Src/AppSw/Platform/Platform_PinCfg.c` and verify that all
   `AppPin_t` definitions match the target board schematic. The current
   values are populated from the `GP_AURIX_Subsystem_PinDefn.xlsx` pin map.

7. **Build**
   ```
   Project -> Build Project  (Ctrl+B)
   ```
   Output: `TriCore Debug/TC387_COMHPC_PMC.elf`

### Option B — Command Line (HIGHTEC GCC)

Requires `tricore-gcc` on `PATH`:
```bash
cd som_aurix_fw
make                    # Debug build (default)
make CONFIG=Release     # Release build (optimised)
make clean
```

---

## Key Timing Parameters (AMD 58241 §16)

| Parameter | Value | AMD 58241 Reference |
|---|---|---|
| Min delay: S5 rails stable -> RSMRST\_L rising | **10 ms** | Table 28, T1 |
| Min delay: PWR\_GOOD rising -> RESET\_L rising | **28.5 ms** | Table 30, T7 |
| Min setup: all rails stable before PWR\_GOOD | **1 ms** (5 ms implemented) | §16.1.1 |
| Group A stable before Group B > 10 % | Required | §16.1.2 |
| Group B stable before Group C > 10 % | Required | §16.1.2 |
| Group C stable before Group D > 10 % | Required | §16.1.2 |
| THERMTRIP# debounce | 3 consecutive polls | Implementation |
| PG fault debounce | 3 consecutive polls | Implementation |

---

## References

| Document | Description |
|---|---|
| AMD Publication **58241** Rev 0.50 (Jul 2023) | *Electrical Data Sheet for AMD Family 1Ah Models 70h-77h Processors* — power sequencing requirements in **Section 16**. Obtain under NDA from your AMD representative. |
| AMD Publication **58023** | *Infrastructure Roadmap (IRM) for FP11 Processors* — power supply specifications referenced by AMD 58241 §16.1.1. |
| Infineon **iLLD\_TC3xx v1.20.0** | Low Level Driver library for AURIX TC3xx — required for build. |
| Infineon **CYPD6129 HPI Specification** (002-24049) | CCGx Host Processor Interface register map used by `Cypd6129_Drv.c`. |
| **PICMG COM-HPC Specification** Rev 1.0 | Defines COM-HPC module power management signals (PWRGD, RSMRST, SLP\_S\*, PROCHOT\#, CATERR\#). |

---

## License

Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
Internal use only. Not for external distribution.
