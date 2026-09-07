# Cross-compilation toolchain file for the Sipeed M1S (Bouffalo BL808,
# riscv64, glibc) using the buildroot_custom SDK toolchain.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-m1s.cmake \
#         -DTOOLCHAIN_ROOT=/path/to/riscv64-buildroot-linux-gnu_sdk-buildroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

if(NOT TOOLCHAIN_ROOT)
  set(TOOLCHAIN_ROOT "$ENV{AEROLITH_TOOLCHAIN_ROOT}")
endif()
if(NOT TOOLCHAIN_ROOT)
  message(FATAL_ERROR
    "TOOLCHAIN_ROOT not set. Pass -DTOOLCHAIN_ROOT=/path/to/riscv64-buildroot-linux-gnu_sdk-buildroot "
    "or set the AEROLITH_TOOLCHAIN_ROOT environment variable.")
endif()

set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/bin/riscv64-unknown-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/riscv64-unknown-linux-gnu-g++")

set(CMAKE_SYSROOT "${TOOLCHAIN_ROOT}/riscv64-buildroot-linux-gnu/sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
