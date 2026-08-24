set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR tricore)
# --- Project root (one level up from cmake/) ---------------------------------
get_filename_component(_proj_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TC_TOOLCHAIN_DIR "${_proj_root}/tools/toolchain" CACHE PATH
    "Directory containing the tricore-gcc installation")
set(TC_PREFIX "tricore-elf-" CACHE STRING "TriCore toolchain prefix")
# --- Locate the toolchain (must be pre-installed — see README) ---------------
# Check 1: already on PATH?
find_program(_tc_gcc_on_path "${TC_PREFIX}gcc")
# Check 2: in tools/toolchain/?
file(GLOB_RECURSE _tc_gcc_in_tools "${TC_TOOLCHAIN_DIR}/*/${TC_PREFIX}gcc")
if(_tc_gcc_on_path)
    message(STATUS "TriCore compiler found on PATH: ${_tc_gcc_on_path}")
    set(_tc_bin_dir "")
elseif(_tc_gcc_in_tools)
    list(GET _tc_gcc_in_tools 0 _tc_gcc_path)
    get_filename_component(_tc_bin_dir "${_tc_gcc_path}" DIRECTORY)
    message(STATUS "TriCore compiler found: ${_tc_gcc_path}")
else()
    message(FATAL_ERROR
        "TriCore GCC toolchain not found.\n"
        "Install it before configuring (see README):\n"
        "  1. git clone --depth 1 https://github.com/volumit/tricore_gcc494_linux_bins.git _tc_toolchain_tmp\n"
        "  2. cd _tc_toolchain_tmp && cat tricore_494_linux.zip.* > tricore_494_linux.zip\n"
        "  3. unzip -qo tricore_494_linux.zip -d ${TC_TOOLCHAIN_DIR}\n"
        "  4. chmod -R +x ${TC_TOOLCHAIN_DIR}\n"
        "  5. rm -rf _tc_toolchain_tmp\n"
        "Or place ${TC_PREFIX}gcc on your PATH.\n"
    )
endif()
# --- Set compiler paths -------------------------------------------------------
if(_tc_bin_dir)
    set(CMAKE_C_COMPILER       "${_tc_bin_dir}/${TC_PREFIX}gcc")
    set(CMAKE_CXX_COMPILER     "${_tc_bin_dir}/${TC_PREFIX}g++")
    set(CMAKE_ASM_COMPILER     "${_tc_bin_dir}/${TC_PREFIX}gcc")
    set(CMAKE_OBJCOPY          "${_tc_bin_dir}/${TC_PREFIX}objcopy" CACHE FILEPATH "objcopy")
    set(CMAKE_OBJDUMP          "${_tc_bin_dir}/${TC_PREFIX}objdump" CACHE FILEPATH "objdump")
    set(CMAKE_SIZE             "${_tc_bin_dir}/${TC_PREFIX}size"    CACHE FILEPATH "size")
    set(CMAKE_NM               "${_tc_bin_dir}/${TC_PREFIX}nm"      CACHE FILEPATH "nm")
else()
    set(CMAKE_C_COMPILER       "${TC_PREFIX}gcc")
    set(CMAKE_CXX_COMPILER     "${TC_PREFIX}g++")
    set(CMAKE_ASM_COMPILER     "${TC_PREFIX}gcc")
    set(CMAKE_OBJCOPY          "${TC_PREFIX}objcopy" CACHE FILEPATH "objcopy")
    set(CMAKE_OBJDUMP          "${TC_PREFIX}objdump" CACHE FILEPATH "objdump")
    set(CMAKE_SIZE             "${TC_PREFIX}size"    CACHE FILEPATH "size")
    set(CMAKE_NM               "${TC_PREFIX}nm"      CACHE FILEPATH "nm")
endif()
# --- Tell CMake not to try running a test executable (cross-compile) ---------
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# --- Search-path policy for cross-compilation --------------------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)