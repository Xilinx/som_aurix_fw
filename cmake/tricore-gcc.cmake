# =============================================================================
# CMake Toolchain File — TriCore GCC 4.9.4 for AURIX TC3xx
# =============================================================================
# Automatically downloads the prebuilt tricore-gcc 4.9.4 toolchain if no
# TriCore compiler is found on PATH or in tools/toolchain/.
# =============================================================================

# --- Cross-compilation basics ------------------------------------------------
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR tricore)

# --- Project root (one level up from cmake/) ---------------------------------
get_filename_component(_proj_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TC_TOOLCHAIN_DIR "${_proj_root}/tools/toolchain" CACHE PATH
    "Directory containing the tricore-gcc installation")

set(TC_PREFIX "tricore-elf-" CACHE STRING "TriCore toolchain prefix")

# --- Locate or download the toolchain ----------------------------------------

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
    # Download prebuilt GCC 4.9.4 from volumit/tricore_gcc494_linux_bins
    set(_tc_repo "https://github.com/volumit/tricore_gcc494_linux_bins.git")
    set(_tc_tmp "${_proj_root}/_tc_toolchain_tmp")

    if(NOT EXISTS "${_tc_tmp}")
        message(STATUS "Downloading tricore-gcc 4.9.4 toolchain...")
        execute_process(
            COMMAND git clone --depth 1
                    -c http.sslVerify=false
                    ${_tc_repo} ${_tc_tmp}
            RESULT_VARIABLE _rc
        )
        if(NOT _rc EQUAL 0)
            file(REMOVE_RECURSE "${_tc_tmp}")
            message(FATAL_ERROR
                "Failed to clone toolchain repo.\n"
                "Clone manually:\n"
                "  git clone --depth 1 ${_tc_repo} _tc_toolchain_tmp"
            )
        endif()
    endif()

    # Reassemble split zip and extract
    file(MAKE_DIRECTORY "${TC_TOOLCHAIN_DIR}")
    message(STATUS "Reassembling split zip archives...")
    execute_process(
        COMMAND bash -c "cat tricore_494_linux.zip.* > tricore_494_linux.zip"
        WORKING_DIRECTORY "${_tc_tmp}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Failed to reassemble zip parts")
    endif()

    message(STATUS "Extracting toolchain...")
    execute_process(
        COMMAND unzip -qo tricore_494_linux.zip -d "${TC_TOOLCHAIN_DIR}"
        WORKING_DIRECTORY "${_tc_tmp}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "Failed to extract toolchain. Make sure 'unzip' is installed:\n"
            "  sudo apt install unzip"
        )
    endif()

    # Clean up
    file(REMOVE_RECURSE "${_tc_tmp}")

    # Find the compiler in the extracted tree
    file(GLOB_RECURSE _tc_gcc_extracted "${TC_TOOLCHAIN_DIR}/*/${TC_PREFIX}gcc")
    if(NOT _tc_gcc_extracted)
        message(FATAL_ERROR
            "Toolchain extracted but ${TC_PREFIX}gcc not found in ${TC_TOOLCHAIN_DIR}"
        )
    endif()
    list(GET _tc_gcc_extracted 0 _tc_gcc_path)
    get_filename_component(_tc_bin_dir "${_tc_gcc_path}" DIRECTORY)

    # Make everything executable (bin/, libexec/cc1, etc.)
    execute_process(
        COMMAND chmod -R +x "${TC_TOOLCHAIN_DIR}"
    )

    message(STATUS "TriCore compiler installed: ${_tc_gcc_path}")
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