# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Host-side (plain g++/clang) unit tests for the SCS0009 servo protocol.
#
# scs_bus.cpp / scs_servo.cpp are compiled *unchanged* against a tiny set of
# ESP-IDF header stubs (idf_stub/) whose UART driver is backed by an in-memory
# FakeUart wire. That lets the tests exercise the real packet framing +
# checksum + response-parsing code — the logic most in need of coverage — on
# the host, with the actual production sources, not a reimplementation.
#
# The header-only PathGenerator and raw<->deg conversions need no stubs at all.

cmake_minimum_required(VERSION 3.16)
project(scs_servo_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(SCS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(EXPECTED_INC ${SCS_ROOT}/../tl_expected/expected/include)
set(IDF_STUB_INC ${CMAKE_CURRENT_SOURCE_DIR}/idf_stub)

# Real production sources + the fake-UART-backed IDF stubs.
add_library(scs_servo_host STATIC
    ${SCS_ROOT}/scs_bus.cpp
    ${SCS_ROOT}/scs_servo.cpp
    ${IDF_STUB_INC}/fake_uart.cpp
)
target_include_directories(scs_servo_host
    PUBLIC ${SCS_ROOT}/include ${EXPECTED_INC} ${IDF_STUB_INC}
)
target_compile_options(scs_servo_host PRIVATE -Wall -Wextra)

enable_testing()

add_executable(scs_test_protocol test_protocol.cpp)
target_link_libraries(scs_test_protocol PRIVATE scs_servo_host)
target_include_directories(scs_test_protocol PRIVATE ${IDF_STUB_INC})
target_compile_options(scs_test_protocol PRIVATE -Wall -Wextra)
add_test(NAME protocol COMMAND scs_test_protocol)

# Pure header-only maths — no stubs needed, but reuse the same lib for the
# tl::expected include path.
add_executable(scs_test_math test_math.cpp)
target_link_libraries(scs_test_math PRIVATE scs_servo_host)
target_compile_options(scs_test_math PRIVATE -Wall -Wextra)
add_test(NAME math COMMAND scs_test_math)
