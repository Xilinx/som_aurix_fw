# =============================================================================
# AURIX TC387 COM-HPC Controller — CMake Build
# =============================================================================
#
# Usage (via presets — recommended):
#   cmake --preset som-debug          # configure for SoM, Debug
#   cmake --build --preset som-debug  # build
#
#   cmake --preset eval-release       # configure for Eval Board, Release
#   cmake --build --preset eval-release
#
# Usage (manual):
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/tricore-gcc.cmake \
#         -DBOARD=som -DCMAKE_BUILD_TYPE=Debug -G Ninja
#   cmake --build build
#
# Dependencies:
#   iLLD_TC3xx v1.20.0 must be unpacked into ./iLLD/ before configuring.
#   Download from Infineon MyICP / AURIX partner portal.
#   See README.md for details.
#
# Custom targets:
#   cmake --build build --target size     # print section sizes
#   cmake --build build --target disasm   # generate disassembly listing
#   cmake --build build --target flash    # program via AURIXFlasher
# =============================================================================
cmake_minimum_required(VERSION 3.20)

project(TC387_COMHPCController
    LANGUAGES C ASM
    VERSION   1.0.0
)

# =============================================================================
# Options
# =============================================================================
set(BOARD "som" CACHE STRING "Board variant: som | eval")
set_property(CACHE BOARD PROPERTY STRINGS som eval)

set(CPU "tc38xx" CACHE STRING "TriCore CPU variant for -mcpu flag")

# =============================================================================
# Target executable
# =============================================================================
set(TARGET_NAME ${PROJECT_NAME})
add_executable(${TARGET_NAME})

# =============================================================================
# iLLD dependency — auto-fetched from Infineon GitHub if ./iLLD/ is missing
# =============================================================================
set(ILLD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/iLLD")
set(ILLD_REPO "https://github.com/Infineon/illd_release_tc3x.git"
    CACHE STRING "iLLD Git repository URL")
set(ILLD_TAG "V1.20.0"
    CACHE STRING "iLLD Git branch or tag to fetch")

if(EXISTS "${ILLD_ROOT}/Cpu/Std/Ifx_Types.h")
    message(STATUS "Using local iLLD: ${ILLD_ROOT}")
else()
    # Clone to a temp directory
    set(_tmp "${CMAKE_CURRENT_SOURCE_DIR}/_illd_tmp")
    if(NOT EXISTS "${_tmp}")
        message(STATUS "Fetching iLLD from ${ILLD_REPO} (${ILLD_TAG})...")
        execute_process(
            COMMAND git clone --depth 1 --branch ${ILLD_TAG}
                    -c http.sslVerify=false
                    ${ILLD_REPO} ${_tmp}
            RESULT_VARIABLE _rc
        )
        if(NOT _rc EQUAL 0)
            file(REMOVE_RECURSE "${_tmp}")
            message(FATAL_ERROR
                "Failed to clone iLLD (exit ${_rc}).\n"
                "Clone manually:\n"
                "  git clone --depth 1 -b ${ILLD_TAG} ${ILLD_REPO} _illd_tmp\n"
                "Then re-run cmake."
            )
        endif()
    endif()

    # Merge the Infineon repo layout into the flat iLLD/ structure:
    #   src/BaseSw/Infra/                      → iLLD/Infra/
    #   src/BaseSw/Service/                    → iLLD/Service/
    #   src/BaseSw/iLLD/TC3xx/Tricore/*        → iLLD/  (Cpu/, Port/, _Impl/, etc.)
    set(_bsw "${_tmp}/src/BaseSw")

    message(STATUS "Assembling iLLD directory...")
    file(MAKE_DIRECTORY "${ILLD_ROOT}")
    file(COPY "${_bsw}/Infra"   DESTINATION "${ILLD_ROOT}")
    file(COPY "${_bsw}/Service" DESTINATION "${ILLD_ROOT}")

    # Flatten module sources from iLLD/TC3xx/Tricore/ up into iLLD/
    file(GLOB _module_dirs "${_bsw}/iLLD/TC3xx/Tricore/*")
    foreach(_dir IN LISTS _module_dirs)
        file(COPY "${_dir}" DESTINATION "${ILLD_ROOT}")
    endforeach()

    # Clean up temp clone
    file(REMOVE_RECURSE "${_tmp}")

    if(NOT EXISTS "${ILLD_ROOT}/Cpu/Std/Ifx_Types.h")
        message(FATAL_ERROR "iLLD assembly failed — Ifx_Types.h not found")
    endif()
    message(STATUS "iLLD ready: ${ILLD_ROOT}")
endif()

# =============================================================================
# Application sources
# =============================================================================
set(APP_SRC_DIRS
    Src/BaseSw
    Src/AppSw/Main
    Src/AppSw/Bsp
    Src/AppSw/Platform
    Src/AppSw/PowerManager
    Src/AppSw/UsbPd
    Src/AppSw/FwMgmt
)

foreach(dir IN LISTS APP_SRC_DIRS)
    file(GLOB _srcs "${dir}/*.c")
    target_sources(${TARGET_NAME} PRIVATE ${_srcs})
endforeach()

# --- Board-specific source filtering ----------------------------------------
if(BOARD STREQUAL "eval")
    # Exclude UsbPd sources for eval board
    get_target_property(_all_srcs ${TARGET_NAME} SOURCES)
    set(_exclude_patterns
        "Src/AppSw/UsbPd/UsbPd_Manager.c"
        "Src/AppSw/UsbPd/UsbPd_Cfg.c"
        "Src/AppSw/UsbPd/Cypd6129_Drv.c"
    )
    foreach(pat IN LISTS _exclude_patterns)
        list(FILTER _all_srcs EXCLUDE REGEX "${pat}")
    endforeach()
    set_target_properties(${TARGET_NAME} PROPERTIES SOURCES "${_all_srcs}")
endif()

# =============================================================================
# iLLD sources
# =============================================================================
set(ILLD_SRC_DIRS
    ${ILLD_ROOT}/Infra/Ssw/TC3xx/Tricore
    ${ILLD_ROOT}/Infra/Platform/Tricore/Compilers
    ${ILLD_ROOT}/Cpu/Std
    ${ILLD_ROOT}/Cpu/Irq
    ${ILLD_ROOT}/Cpu/Trap
    ${ILLD_ROOT}/Asclin/Std
    ${ILLD_ROOT}/Asclin/Asc
    ${ILLD_ROOT}/I2c/Std
    ${ILLD_ROOT}/I2c/I2c
    ${ILLD_ROOT}/Qspi/Std
    ${ILLD_ROOT}/Qspi/SpiMaster
    ${ILLD_ROOT}/Qspi/SpiSlave
    ${ILLD_ROOT}/Evadc/Std
    ${ILLD_ROOT}/Evadc/Adc
    ${ILLD_ROOT}/Port/Std
    ${ILLD_ROOT}/Scu/Std
    ${ILLD_ROOT}/Stm/Std
    ${ILLD_ROOT}/Src/Std
    ${ILLD_ROOT}/Dma/Std
    ${ILLD_ROOT}/Dma/Dma
    ${ILLD_ROOT}/Pms/Std
    ${ILLD_ROOT}/Flash/Std
    ${ILLD_ROOT}/Service/CpuGeneric/StdIf
    ${ILLD_ROOT}/Service/CpuGeneric/SysSe/Bsp
    ${ILLD_ROOT}/Service/CpuGeneric/SysSe/Comm
    ${ILLD_ROOT}/Service/CpuGeneric/SysSe/General
    ${ILLD_ROOT}/Service/CpuGeneric/SysSe/Time
    ${ILLD_ROOT}/_Lib/DataHandling
    ${ILLD_ROOT}/_Lib/InternalMux
    ${ILLD_ROOT}/_Impl
)

# Collect .c sources from iLLD directories
foreach(dir IN LISTS ILLD_SRC_DIRS)
    file(GLOB _srcs "${dir}/*.c")
    target_sources(${TARGET_NAME} PRIVATE ${_srcs})
endforeach()

# Collect assembly sources (.S, .sx) from iLLD directories
foreach(dir IN LISTS ILLD_SRC_DIRS)
    file(GLOB _asm "${dir}/*.S" "${dir}/*.sx")
    target_sources(${TARGET_NAME} PRIVATE ${_asm})
endforeach()

file(GLOB_RECURSE _pinmap "${ILLD_ROOT}/_PinMap/*_TC38x_LFBGA292.c")

target_sources(${TARGET_NAME} PRIVATE ${_pinmap})

# =============================================================================
# Include directories
# =============================================================================
target_include_directories(${TARGET_NAME} PRIVATE
    # Application
    Src/AppSw/Main
    Src/AppSw/Bsp
    Src/AppSw/Platform
    Src/AppSw/PowerManager
    Src/AppSw/UsbPd
    Src/AppSw/FwMgmt
    Src/BaseSw
    # iLLD
    ${ILLD_ROOT}
    ${ILLD_ROOT}/Infra/Platform
    ${ILLD_ROOT}/Infra/Ssw/TC3xx/Tricore
    ${ILLD_ROOT}/Infra/Sfr/TC38x
    ${ILLD_ROOT}/Service/CpuGeneric
    ${ILLD_ROOT}/_Impl
    ${ILLD_ROOT}/_Impl/TC38x
    ${ILLD_ROOT}/_PinMap
    ${ILLD_ROOT}/_PinMap/TC38x
    ${ILLD_ROOT}/Cpu/Std
    ${ILLD_ROOT}/Scu/Std
    ${ILLD_ROOT}/Port/Std
    ${ILLD_ROOT}/Stm/Std
    ${ILLD_ROOT}/Asclin/Std
    ${ILLD_ROOT}/Asclin/Asc
    ${ILLD_ROOT}/I2c/Std
    ${ILLD_ROOT}/I2c/I2c
    ${ILLD_ROOT}/Qspi/Std
    ${ILLD_ROOT}/Qspi/SpiMaster
    ${ILLD_ROOT}/Qspi/SpiSlave
    ${ILLD_ROOT}/Dma/Std
    ${ILLD_ROOT}/Dma/Dma
    ${ILLD_ROOT}/Evadc/Std
    ${ILLD_ROOT}/Evadc/Adc
    ${ILLD_ROOT}/Cpu/Irq
    ${ILLD_ROOT}/Cpu/Trap
    ${ILLD_ROOT}/Src/Std
    ${ILLD_ROOT}/Pms/Std
    ${ILLD_ROOT}/Flash/Std
    ${ILLD_ROOT}/_Lib/DataHandling
    ${ILLD_ROOT}/_Lib/InternalMux
    ${ILLD_ROOT}/Service/CpuGeneric/_Utilities
    ${ILLD_ROOT}/Service/CpuGeneric/SysSe/Bsp
)

# =============================================================================
# Compile definitions
# =============================================================================
target_compile_definitions(${TARGET_NAME} PRIVATE
    IFX_CFG_TC3XX_DEVICE=IFX_CFG_TC38XA
    $<$<CONFIG:Debug>:CFG_DEBUG=1>
    $<$<CONFIG:Release>:NDEBUG>
)

if(BOARD STREQUAL "eval")
    target_compile_definitions(${TARGET_NAME} PRIVATE TARGET_EVAL_BOARD=1)
else()
    target_compile_definitions(${TARGET_NAME} PRIVATE TARGET_GP_SOM=1)
endif()

# =============================================================================
# Compiler flags
# =============================================================================
target_compile_options(${TARGET_NAME} PRIVATE
    -mcpu=${CPU}
    -std=gnu99
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g3 -O0>
    $<$<CONFIG:Release>:-O2>
)

# Tell CMake to treat .S and .sx files as assembler-with-cpp
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}  # fallback
    PROPERTIES LANGUAGE ASM
)

# ASM files need the same flags (gcc -x assembler-with-cpp)
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=${CPU} -x assembler-with-cpp")

# =============================================================================
# Linker configuration
# =============================================================================
set(LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/Linker/tc387.ld)

target_link_options(${TARGET_NAME} PRIVATE
    -mcpu=${CPU}
    -T ${LDSCRIPT}
    -Wl,--gc-sections
    -Wl,-Map=$<TARGET_FILE_DIR:${TARGET_NAME}>/${TARGET_NAME}.map
    -Wl,--cref
    -nostartfiles
    -Wl,--start-group -lc -lm -Wl,--end-group
)

# Re-link if linker script changes
set_target_properties(${TARGET_NAME} PROPERTIES
    LINK_DEPENDS ${LDSCRIPT}
    SUFFIX       ".elf"
)

# =============================================================================
# Post-build: generate .hex and .bin
# =============================================================================
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex
        $<TARGET_FILE:${TARGET_NAME}>
        $<TARGET_FILE_DIR:${TARGET_NAME}>/${TARGET_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:${TARGET_NAME}>
        $<TARGET_FILE_DIR:${TARGET_NAME}>/${TARGET_NAME}.bin
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:${TARGET_NAME}>
    COMMENT "Generating .hex and .bin, printing size"
)

# =============================================================================
# Convenience targets: size, disasm, flash
# =============================================================================
add_custom_target(size
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:${TARGET_NAME}>
    DEPENDS ${TARGET_NAME}
    COMMENT "Section sizes"
)

add_custom_target(disasm
    COMMAND ${CMAKE_OBJDUMP} -d -S $<TARGET_FILE:${TARGET_NAME}>
        > $<TARGET_FILE_DIR:${TARGET_NAME}>/${TARGET_NAME}.lss
    DEPENDS ${TARGET_NAME}
    COMMENT "Disassembly → ${TARGET_NAME}.lss"
)

# =============================================================================
# Flash support (WSL / Windows AURIXFlasher)
# =============================================================================
# Auto-discover AURIXFlasher.exe if not set explicitly.
# This runs at configure time — results are printed and cached.
set(FLASHER "" CACHE FILEPATH "Path to AURIXFlasher.exe (auto-detected if empty)")

if(NOT FLASHER OR FLASHER STREQUAL "")
    # Check repo-local flasher first
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/flasher/AURIXFlasher.exe")
        set(FLASHER "${CMAKE_CURRENT_SOURCE_DIR}/tools/flasher/AURIXFlasher.exe")
    else()
        # Fall back to system-installed flasher
        file(GLOB _flasher_candidates
            "/mnt/c/Infineon/AURIX-Studio-*/tools/AurixFlasherSoftwareTool_*/AURIXFlasher.exe"
            "/mnt/c/Program Files/Infineon/*/AURIXFlasher.exe"
            "/opt/Infineon/*/AURIXFlasher.exe"
            "/usr/local/bin/AURIXFlasher"
        )
        if(_flasher_candidates)
            list(GET _flasher_candidates 0 FLASHER)
        endif()
    endif()
endif()

# Auto-discover HEX staging directory (Windows-accessible path for WSL)
set(HEX_STAGING_DIR "" CACHE PATH "Windows-accessible dir for .hex staging (auto-detected if empty)")

if(NOT HEX_STAGING_DIR OR HEX_STAGING_DIR STREQUAL "")
    if(DEFINED ENV{USER} AND EXISTS "/mnt/c/Users/$ENV{USER}")
        set(HEX_STAGING_DIR "/mnt/c/Users/$ENV{USER}")
    elseif(DEFINED ENV{USERNAME} AND EXISTS "/mnt/c/Users/$ENV{USERNAME}")
        set(HEX_STAGING_DIR "/mnt/c/Users/$ENV{USERNAME}")
    else()
        set(HEX_STAGING_DIR "/tmp")
    endif()
endif()

# Convert /mnt/c/... to C:/... for the Windows-side flasher
string(REGEX REPLACE "^/mnt/c/" "C:/" HEX_WIN "${HEX_STAGING_DIR}/TC387.hex")

# --- Print environment summary at configure time ----------------------------
# This replaces "make setup" — you see it once when you run cmake --preset ...
message(STATUS "===============================")
message(STATUS " AURIX Build Environment")
message(STATUS "===============================")
message(STATUS "Board:            ${BOARD}")
message(STATUS "Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "Toolchain prefix: ${TC_PREFIX}")
if(FLASHER)
    message(STATUS "Flasher:          ${FLASHER}")
else()
    message(STATUS "Flasher:          NOT FOUND — set -DFLASHER=/path/to/AURIXFlasher.exe")
endif()
message(STATUS "HEX staging dir:  ${HEX_STAGING_DIR}")
message(STATUS "HEX Windows path: ${HEX_WIN}")
message(STATUS "===============================")

# --- flash target: build, copy hex, program ----------------------------------
add_custom_target(flash
    COMMAND ${CMAKE_COMMAND} -E echo "[FLASH] Copying hex to staging..."
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE_DIR:${TARGET_NAME}>/${TARGET_NAME}.hex
        ${HEX_STAGING_DIR}/TC387.hex
    COMMAND ${CMAKE_COMMAND} -E echo "[FLASH] Programming..."
    COMMAND ${FLASHER}
        -hex ${HEX_WIN}
        -erase on -prog on -ver on -ucb on -start on
    DEPENDS ${TARGET_NAME}
    COMMENT "Programming via AURIXFlasher"
)