# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Host-side (ESP-IDF independent) unit tests for the avatar bytecode VM.
# Builds `decoder.cpp` + `vm.cpp` straight against plain g++/clang. The only
# non-host include the sources reach for is <M5GFX.h> (pulled in by
# avatar/canvas.hpp); we satisfy it with the same WASM shim used by the browser
# preview build (wasm/shim), so no ESP-IDF headers are needed at all.
#
# `storage.cpp` / `default_bytecode.cpp` are intentionally NOT built here:
# storage.cpp needs <esp_log.h> + <nvs.h> and default_bytecode.cpp resolves
# linker symbols emitted by ESP-IDF's EMBED_FILES machinery — neither is pure
# logic worth host-testing.

cmake_minimum_required(VERSION 3.16)
project(avatar_vm_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(VM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_ROOT ${VM_ROOT}/../..)
set(EXPECTED_INC ${VM_ROOT}/../tl_expected/expected/include)
set(M5GFX_SHIM_INC ${REPO_ROOT}/wasm/shim)

add_library(avatar_vm STATIC
    ${VM_ROOT}/decoder.cpp
    ${VM_ROOT}/vm.cpp
)
target_include_directories(avatar_vm
    PUBLIC ${VM_ROOT}/include ${EXPECTED_INC}
    # M5GFX shim last so a real ESP-IDF M5GFX (if somehow on the path) wins;
    # on a bare host only the shim is found.
    PUBLIC ${M5GFX_SHIM_INC}
)
target_compile_options(avatar_vm PRIVATE -Wall -Wextra)

enable_testing()

add_executable(avatar_vm_test_decoder test_decoder.cpp)
target_link_libraries(avatar_vm_test_decoder PRIVATE avatar_vm)
target_compile_options(avatar_vm_test_decoder PRIVATE -Wall -Wextra)
add_test(NAME decoder COMMAND avatar_vm_test_decoder)

add_executable(avatar_vm_test_vm test_vm.cpp)
target_link_libraries(avatar_vm_test_vm PRIVATE avatar_vm)
target_compile_options(avatar_vm_test_vm PRIVATE -Wall -Wextra)
add_test(NAME vm COMMAND avatar_vm_test_vm)

add_executable(avatar_vm_test_fuzz test_fuzz.cpp)
target_link_libraries(avatar_vm_test_fuzz PRIVATE avatar_vm)
target_compile_options(avatar_vm_test_fuzz PRIVATE -Wall -Wextra)
add_test(NAME fuzz COMMAND avatar_vm_test_fuzz)
