cmake_minimum_required(VERSION 3.16)
project(tutuicmptunnel-kmod VERSION 1.5 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
  message(FATAL_ERROR "CMAKE_BUILD_TYPE is not set. Please set it to continue.\n"
                      "Example: cmake -DCMAKE_BUILD_TYPE=Release ..\n"
                      "Valid types: Release, Debug, RelWithDebInfo, MinSizeRel")
endif()

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Wno-error)
add_definitions(-D_GNU_SOURCE=1 -DPROJECT_NAME=${CMAKE_PROJECT_NAME} -DINSTALL_PREFIX_DIR=${CMAKE_INSTALL_PREFIX})
include_directories(common)
add_compile_definitions(
  VERSION_MAJOR=${CMAKE_PROJECT_VERSION_MAJOR}
  VERSION_MINOR=${CMAKE_PROJECT_VERSION_MINOR}
  VERSION_STR=\"${CMAKE_PROJECT_VERSION}\"
)

option(USE_LLVM_FOR_KMOD 1)
option(ENABLE_HARDEN_MODE "Enable security hardening compiler and linker flags" OFF)
if (ENABLE_HARDEN_MODE)
  message(STATUS "Secure mode enabled. Adding security hardening flags.")
  add_compile_definitions(_FORTIFY_SOURCE=2)
  set(HARDEN_C_FLAGS
    #-Werror
    -Werror=implicit
    -Werror=incompatible-pointer-types
    -Werror=int-conversion
    -fPIE
  )
  include(CheckCCompilerFlag)
  macro(check_and_add_c_flag FLAG)
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" VAR_SUFFIX ${FLAG})
    check_c_compiler_flag("${FLAG}" "C_HAS_${VAR_SUFFIX}")
    if(C_HAS_${VAR_SUFFIX})
      message(STATUS "  -> Compiler supports: ${FLAG}")
      list(APPEND HARDEN_C_FLAGS "${FLAG}")
    else()
      message(STATUS "  -> Compiler does NOT support: ${FLAG} (skipping)")
    endif()
  endmacro()

  check_and_add_c_flag("-fstack-clash-protection")
  check_and_add_c_flag("-fstack-protector-strong")
  check_and_add_c_flag("-Wtrampolines")
  check_and_add_c_flag("-Wbidi-chars=any")
  check_and_add_c_flag("-fno-delete-null-pointer-checks")
  check_and_add_c_flag("-fno-strict-overflow")
  check_and_add_c_flag("-fno-strict-aliasing")
  check_and_add_c_flag("-ftrivial-auto-var-init=zero")
  check_and_add_c_flag("-fstrict-flex-arrays=3")
  check_and_add_c_flag("-mbranch-protection=standard")
  check_and_add_c_flag("-fexceptions")
  check_and_add_c_flag("-fcf-protection=full")
  if (NOT C_HAS__fcf_protection_full)
    check_and_add_c_flag("-fcf-protection=branch")
  endif()

  set(HARDEN_LINKER_FLAGS
        -pie
        -Wl,-z,nodlopen
        -Wl,-z,noexecstack
        -Wl,-z,relro
        -Wl,-z,now
        -Wl,--as-needed
        -Wl,--no-copy-dt-needed-entries
        -Wl,-z,max-page-size=4096
        -Wl,--gc-sections
  )
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/kmod/dkms.conf.in"
  "${PROJECT_SOURCE_DIR}/kmod/dkms.conf"
  @ONLY
)

configure_file(
  "${PROJECT_SOURCE_DIR}/kmod/Makefile.in"
  "${PROJECT_SOURCE_DIR}/kmod/Makefile"
  @ONLY
)

#set(CMAKE_VERBOSE_MAKEFILE ON)

if(USE_LLVM_FOR_KMOD)
  list(APPEND KMOD_MAKE_ARGS "LLVM=${LLVM}")
endif()

include(CheckCXXSourceRuns)
execute_process(
  COMMAND ${CMAKE_MAKE_PROGRAM} --no-silent -v
  RESULT_VARIABLE MAKE_NOSILENT_RESULT
  OUTPUT_QUIET ERROR_QUIET
)

if(MAKE_NOSILENT_RESULT EQUAL 0)
  set(MAKE_NOSILENT_ARG "--no-silent")
else()
  set(MAKE_NOSILENT_ARG "")
endif()

add_subdirectory(common)
add_subdirectory(ktuctl)

# 查找libsodium
find_path(SODIUM_INCLUDE_DIR sodium.h PATH_SUFFIXES sodium)
find_library(SODIUM_LIBRARY sodium)

# 有libsodium就编译tuserver
if(SODIUM_INCLUDE_DIR AND SODIUM_LIBRARY)
  message(STATUS "Found libsodium - building tuserver")
  add_subdirectory(tuserver)
else()
  message(STATUS "libsodium not found - skipping tuserver")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INSTALL_DO_STRIP ON)
endif()

# vim: set sw=2 expandtab :
