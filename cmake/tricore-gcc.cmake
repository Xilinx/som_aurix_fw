# =============================================================================
# CMake Toolchain File — HighTec / tricore-elf GCC for AURIX TC3xx
# =============================================================================
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/tricore-gcc.cmake ...
#   (or use CMakePresets.json which sets this automatically)
# =============================================================================

# --- Cross-compilation basics ------------------------------------------------
set(CMAKE_SYSTEM_NAME      Generic)        # bare-metal, no OS
set(CMAKE_SYSTEM_PROCESSOR tricore)

# --- Toolchain prefix --------------------------------------------------------
# Works with both "tricore-gcc" (HighTec) and "tricore-elf-gcc" (open-source).
# Override at configure time:  -DTC_PREFIX=tricore-elf-
set(TC_PREFIX "tricore-" CACHE STRING "TriCore toolchain prefix")

set(CMAKE_C_COMPILER       "${TC_PREFIX}gcc")
set(CMAKE_CXX_COMPILER     "${TC_PREFIX}g++")
set(CMAKE_ASM_COMPILER      "${TC_PREFIX}gcc")
set(CMAKE_OBJCOPY           "${TC_PREFIX}objcopy" CACHE FILEPATH "objcopy")
set(CMAKE_OBJDUMP           "${TC_PREFIX}objdump" CACHE FILEPATH "objdump")
set(CMAKE_SIZE              "${TC_PREFIX}size"    CACHE FILEPATH "size")
set(CMAKE_NM                "${TC_PREFIX}nm"      CACHE FILEPATH "nm")

# --- Tell CMake not to try running a test executable (cross-compile) ---------
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# --- Search-path policy for cross-compilation --------------------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)