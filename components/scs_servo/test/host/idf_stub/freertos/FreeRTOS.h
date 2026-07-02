// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Minimal <freertos/FreeRTOS.h> stub: scs_bus.cpp only needs TickType_t and
// pdMS_TO_TICKS (used purely as an argument to the faked uart_* calls, which
// ignore the tick count on the host).

#pragma once

typedef unsigned int TickType_t;

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
