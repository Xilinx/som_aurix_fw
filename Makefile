# =============================================================================
# Makefile — TC387_COMHPCController
# Target:    AURIX TC387 (TC38xA)
# Toolchain: HIGHTEC GCC for TriCore
#
# Usage:
#   make                  — build Debug for System-on-Module (default)
#   make BOARD=eval       — build Debug for Eval Board
#   make CONFIG=Release   — build Release (optimised)
#   make BOARD=eval CONFIG=Release — build Release for Eval Board
#   make clean            — remove build artefacts
#   make size             — print section sizes for the last build
#   make disasm           — generate disassembly listing
#
# Board variants:
#   BOARD=som  (default)  — targets the GP System-on-Module (defines TARGET_GP_SOM)
#   BOARD=eval            — targets the Eval Board (defines TARGET_EVAL_BOARD),
#                           excludes SysMonitor, ComHpcWdt, and UsbPd sources
#
# Prerequisites:
#   - HIGHTEC GCC TriCore toolchain on PATH  (tricore-gcc, tricore-objcopy, ...)
#   - iLLD_TC3xx library unpacked into ./iLLD/
#   - Linker/tc387.ld    (from iLLD examples or HIGHTEC board package)
# =============================================================================

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
TC_PREFIX   := tricore-elf-
CC          := $(TC_PREFIX)gcc
AS          := $(TC_PREFIX)gcc -x assembler-with-cpp
LD          := $(TC_PREFIX)gcc
OBJCOPY     := $(TC_PREFIX)objcopy
OBJDUMP     := $(TC_PREFIX)objdump
SIZE        := $(TC_PREFIX)size
NM          := $(TC_PREFIX)nm

# -----------------------------------------------------------------------------
# Target device
# -----------------------------------------------------------------------------
# TC387 belongs to the TC38xA sub-family.
CPU         := tc38xx

# -----------------------------------------------------------------------------
# Build output directories
# -----------------------------------------------------------------------------
CONFIG      ?= Debug
BUILD_DIR   := Build/$(CONFIG)
OBJ_DIR     := $(BUILD_DIR)/obj
BIN_DIR     := $(BUILD_DIR)/bin

# Final artefact base name (extensions added below)
TARGET      := $(BIN_DIR)/TC387_COMHPCController



# -----------------------------------------------------------------------------
# Application source directories
# -----------------------------------------------------------------------------
APP_SRC_DIRS := \
	Src/BaseSw              \
	Src/AppSw/Main          \
	Src/AppSw/Bsp           \
	Src/AppSw/Platform      \
	Src/AppSw/PowerManager  \
	Src/AppSw/UsbPd         \
	Src/AppSw/FwMgmt

# -----------------------------------------------------------------------------
# iLLD source directories
# Add subdirectories here as iLLD modules are needed.
# Adjust paths to match your iLLD_TC3xx tree layout.
# -----------------------------------------------------------------------------
ILLD_ROOT   := iLLD

ILLD_SRC_DIRS := \
	$(ILLD_ROOT)/Infra/Ssw/TC3xx/Tricore        \
	$(ILLD_ROOT)/Infra/Platform/Tricore/Compilers \
	$(ILLD_ROOT)/Cpu/Std                          \
	$(ILLD_ROOT)/Cpu/Irq                          \
	$(ILLD_ROOT)/Cpu/Trap                         \
	$(ILLD_ROOT)/Asclin/Std                       \
	$(ILLD_ROOT)/Asclin/Asc                       \
	$(ILLD_ROOT)/I2c/Std                          \
	$(ILLD_ROOT)/I2c/I2c                          \
	$(ILLD_ROOT)/Qspi/Std                         \
	$(ILLD_ROOT)/Qspi/SpiMaster                   \
	$(ILLD_ROOT)/Evadc/Std                        \
	$(ILLD_ROOT)/Evadc/Adc                        \
	$(ILLD_ROOT)/Port/Std                         \
	$(ILLD_ROOT)/Scu/Std                          \
	$(ILLD_ROOT)/Stm/Std                          \
	$(ILLD_ROOT)/Src/Std                          \
	$(ILLD_ROOT)/Qspi/SpiSlave				      \
	$(ILLD_ROOT)/Dma/Std                          \
	$(ILLD_ROOT)/Dma/Dma                          \
	$(ILLD_ROOT)/Pms/Std                          \
	$(ILLD_ROOT)/Flash/Std						  \
	$(ILLD_ROOT)/Service/CpuGeneric/StdIf         \
	$(ILLD_ROOT)/Service/CpuGeneric/SysSe/Bsp     \
	$(ILLD_ROOT)/Service/CpuGeneric/SysSe/Comm    \
	$(ILLD_ROOT)/Service/CpuGeneric/SysSe/General \
	$(ILLD_ROOT)/Service/CpuGeneric/SysSe/Time    \
	$(ILLD_ROOT)/_Lib/DataHandling                \
	$(ILLD_ROOT)/_Lib/InternalMux                 \
	$(ILLD_ROOT)/_Impl							 

# Collect .c files from application and iLLD directories
APP_SRCS    := $(foreach d, $(APP_SRC_DIRS),  $(wildcard $(d)/*.c))
ILLD_SRCS   := $(foreach d, $(ILLD_SRC_DIRS), $(wildcard $(d)/*.c))
ILLD_SRCS   += $(wildcard $(ILLD_ROOT)/_PinMap/*_TC38x_LFBGA292.c)

BOARD ?= som

ifeq ($(BOARD),eval)
    BOARD_DEFINE += -DTARGET_EVAL_BOARD=1
else
    BOARD_DEFINE += -DTARGET_GP_SOM=1
endif

ALL_SRCS    := $(APP_SRCS) $(ILLD_SRCS)

# Collect .S / .sx startup assembly files (iLLD startup code)
ASM_SRCS    := $(foreach d, $(ILLD_SRC_DIRS), $(wildcard $(d)/*.S) $(wildcard $(d)/*.sx))

# Derive object file paths preserving directory structure
APP_OBJS    := $(patsubst %.c,   $(OBJ_DIR)/%.o, $(APP_SRCS))
ILLD_OBJS   := $(patsubst %.c,   $(OBJ_DIR)/%.o, $(ILLD_SRCS))
ASM_OBJS    := $(patsubst %.S,   $(OBJ_DIR)/%.o, \
	           $(patsubst %.sx,  $(OBJ_DIR)/%.o, $(ASM_SRCS)))
ALL_OBJS    := $(APP_OBJS) $(ILLD_OBJS) $(ASM_OBJS)

# -----------------------------------------------------------------------------
# Include paths
# -----------------------------------------------------------------------------
INCLUDES := \
	-I Src/AppSw/Main           \
	-I Src/AppSw/Bsp            \
	-I Src/AppSw/Platform       \
	-I Src/AppSw/PowerManager   \
	-I Src/AppSw/UsbPd          \
	-I Src/AppSw/FwMgmt         \
	-I Src/BaseSw               \
	-I $(ILLD_ROOT)             \
	-I $(ILLD_ROOT)/Infra/Platform  \
	-I $(ILLD_ROOT)/Infra/Ssw/TC3xx/Tricore \
	-I $(ILLD_ROOT)/Infra/Sfr/TC38x \
	-I $(ILLD_ROOT)/Service/CpuGeneric \
	-I $(ILLD_ROOT)/_Impl       \
	-I $(ILLD_ROOT)/_Impl/TC38x \
	-I $(ILLD_ROOT)/_PinMap \
	-I $(ILLD_ROOT)/_PinMap/TC38x \
	-I $(ILLD_ROOT)/Cpu/Std     \
	-I $(ILLD_ROOT)/Scu/Std     \
	-I $(ILLD_ROOT)/Port/Std    \
	-I $(ILLD_ROOT)/Stm/Std     \
	-I $(ILLD_ROOT)/Asclin/Std  \
	-I $(ILLD_ROOT)/Asclin/Asc  \
	-I $(ILLD_ROOT)/I2c/Std     \
	-I $(ILLD_ROOT)/I2c/I2c     \
	-I $(ILLD_ROOT)/Qspi/Std    \
	-I $(ILLD_ROOT)/Qspi/SpiMaster \
	-I $(ILLD_ROOT)/Qspi/SpiSlave   \
	-I $(ILLD_ROOT)/Dma/Std             \
	-I $(ILLD_ROOT)/Dma/Dma             \
	-I $(ILLD_ROOT)/Evadc/Std         \
	-I $(ILLD_ROOT)/Evadc/Adc         \
	-I $(ILLD_ROOT)/Cpu/Irq     \
	-I $(ILLD_ROOT)/Cpu/Trap    \
	-I $(ILLD_ROOT)/Src/Std		\
	-I $(ILLD_ROOT)/Pms/Std     \
	-I $(ILLD_ROOT)/Flash/Std   \
	-I $(ILLD_ROOT)/_Lib/DataHandling \
	-I $(ILLD_ROOT)/_Lib/InternalMux \
	-I $(ILLD_ROOT)/Service/CpuGeneric/_Utilities \
	-I $(ILLD_ROOT)/Service/CpuGeneric/SysSe/Bsp

# -----------------------------------------------------------------------------
# Preprocessor defines
# -----------------------------------------------------------------------------
DEFINES_COMMON := \
	-DIFX_CFG_TC3XX_DEVICE=IFX_CFG_TC38XA \
	-DDEVICE_TC38X \
	-DIFX_PIN_PACKAGE_LFBGA292
	
DEFINES_DBG := $(DEFINES_COMMON) -DCFG_DEBUG=1
DEFINES_REL := $(DEFINES_COMMON) -DNDEBUG

# -----------------------------------------------------------------------------
# Compiler flags
# -----------------------------------------------------------------------------
MCPU_FLAGS  := -mcpu=$(CPU)

CFLAGS_COMMON := \
	$(BOARD_DEFINE) 			\
	$(MCPU_FLAGS)               \
	-std=gnu99                  \
	-ffunction-sections         \
	-fdata-sections             \
	-Wall                       \
	-Wextra                     \
	-Wno-unused-parameter       \
	$(INCLUDES)

CFLAGS_DBG  := $(CFLAGS_COMMON) $(DEFINES_DBG) -g3 -O0
CFLAGS_REL  := $(CFLAGS_COMMON) $(DEFINES_REL) -O2

ifeq ($(CONFIG),Release)
	CFLAGS  := $(CFLAGS_REL)
else
	CFLAGS  := $(CFLAGS_DBG)
endif

# Assembler uses same flags as C compiler (gcc -x assembler-with-cpp)
ASFLAGS     := $(CFLAGS)

# -----------------------------------------------------------------------------
# Linker flags
# -----------------------------------------------------------------------------
# tc387.ld: use the linker script from the iLLD board package or HIGHTEC
# examples. It defines the TC387 memory map (PFlash, DSPR, CSA stack, etc.).
LDSCRIPT    := Linker/tc387.ld

LDFLAGS     := \
	$(MCPU_FLAGS)               \
	-T $(LDSCRIPT)              \
	-Wl,--gc-sections           \
	-Wl,-Map=$(TARGET).map      \
	-Wl,--cref                  \
	-nostartfiles               \
	-Wl,--start-group -lc -lm -Wl,--end-group

# -----------------------------------------------------------------------------
# Build rules
# -----------------------------------------------------------------------------
.PHONY: all clean size disasm

all: $(TARGET).elf
	@echo ""
	@echo "Build complete: $(TARGET).elf"
	@$(SIZE) --format=berkeley $<

$(TARGET).elf: $(ALL_OBJS) | $(BIN_DIR)
	@echo "[LD]  $@"
	@$(LD) $(LDFLAGS) -o $@ $^
	@$(OBJCOPY) -O ihex   $@ $(TARGET).hex
	@$(OBJCOPY) -O binary $@ $(TARGET).bin

# C source -> object
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Assembly source -> object (.S)
$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "[AS]  $<"
	@$(AS) $(ASFLAGS) -c -o $@ $<

# Assembly source -> object (.sx)
$(OBJ_DIR)/%.o: %.sx
	@mkdir -p $(dir $@)
	@echo "[AS]  $<"
	@$(AS) $(ASFLAGS) -c -o $@ $<

$(BIN_DIR):
	@mkdir -p $@

clean:
	rm -rf Build/ build/

size: $(TARGET).elf
	$(SIZE) --format=berkeley $<

disasm: $(TARGET).elf
	$(OBJDUMP) -d -S $< > $(TARGET).lss
	@echo "Disassembly: $(TARGET).lss"

FLASHER_SEARCH_PATHS := \
    /mnt/c/Infineon/AURIX-Studio-*/tools/AurixFlasherSoftwareTool_*/AURIXFlasher.exe \
    /mnt/c/Program\ Files/Infineon/*/AURIXFlasher.exe \
    /opt/Infineon/*/AURIXFlasher.exe \
    /usr/local/bin/AURIXFlasher

FLASHER ?= $(firstword $(wildcard $(FLASHER_SEARCH_PATHS)))
HEX_STAGING_DIR ?= $(firstword $(wildcard /mnt/c/Users/$(USER)) $(wildcard /mnt/c/Users/$(USERNAME)) /tmp)
HEX_WIN = $(subst /mnt/c/,C:/,$(HEX_STAGING_DIR))/TC387.hex

.PHONY: setup flash

setup:
	@echo "==============================="
	@echo " AURIX Build Environment Setup"
	@echo "==============================="
	@if [ -n "$(FLASHER)" ] && [ -f "$(FLASHER)" ]; then \
		echo "[OK] Flasher found: $(FLASHER)"; \
	else \
		echo "[!!] AURIXFlasher not found. Searched:"; \
		echo "     $(FLASHER_SEARCH_PATHS)"; \
		echo "     Set manually: make flash FLASHER=/path/to/AURIXFlasher.exe"; \
	fi
	@if [ -d "$(HEX_STAGING_DIR)" ]; then \
		echo "[OK] HEX staging dir: $(HEX_STAGING_DIR)"; \
		echo "[OK] HEX Windows path: $(HEX_WIN)"; \
	else \
		echo "[!!] HEX staging dir not found. Set HEX_STAGING_DIR=/your/path"; \
	fi
	@echo "==============================="

flash: $(TARGET).elf
	@if [ -z "$(FLASHER)" ] || [ ! -f "$(FLASHER)" ]; then \
		echo "[ERROR] AURIXFlasher not found. Run 'make setup' or set FLASHER="; \
		exit 1; \
	fi
	@cp $(TARGET).hex $(HEX_STAGING_DIR)/TC387.hex
	@echo "[FLASH] Programming..."
	@$(FLASHER) -hex $(HEX_WIN) -erase on -prog on -ver on -ucb on -start on -port das

