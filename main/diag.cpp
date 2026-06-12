// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "diag.hpp"

#include <cstdint>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "diag";

// Called by the heap on ANY failed allocation. ESP_EARLY_LOG* because this
// can fire from timing-sensitive contexts; keep the handler dead simple.
void on_alloc_fail(std::size_t size, std::uint32_t caps, const char* function_name)
{
    ESP_EARLY_LOGE(kTag,
                   "ALLOC_FAIL size=%u caps=0x%08x caller=%s | INT free=%u largest=%u | DMA largest=%u",
                   static_cast<unsigned>(size), static_cast<unsigned>(caps),
                   function_name != nullptr ? function_name : "?",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
}

} // namespace

void diag_register_alloc_fail_hook()
{
    const esp_err_t err = heap_caps_register_failed_alloc_callback(&on_alloc_fail);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed_alloc_callback registration failed: %s", esp_err_to_name(err));
    }
}

void diag_heap(const char* label)
{
    ESP_LOGI(kTag, "[%s] INT free=%u largest=%u min=%u | DMA largest=%u | PSRAM free=%u",
             label != nullptr ? label : "-",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void diag_stack_hwm()
{
#if configUSE_TRACE_FACILITY == 1
    UBaseType_t n = uxTaskGetNumberOfTasks();
    auto* buf = static_cast<TaskStatus_t*>(
        heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGW(kTag, "hwm: TaskStatus_t buffer alloc failed (n=%u)", static_cast<unsigned>(n));
        return;
    }
    n = uxTaskGetSystemState(buf, n, nullptr);
    ESP_LOGI(kTag, "=== stack HWM (bytes of headroom never used; smaller = closer to overflow) ===");
    for (UBaseType_t i = 0; i < n; ++i) {
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        const int core = static_cast<int>(buf[i].xCoreID) == INT32_MAX
                             ? -1
                             : static_cast<int>(buf[i].xCoreID);
#else
        const int core = -1;
#endif
        // ESP-IDF xtensa port: StackType_t = uint8_t → HWM unit is bytes.
        ESP_LOGI(kTag, "  %-18s core=%2d prio=%2u hwm=%6u B",
                 buf[i].pcTaskName, core,
                 static_cast<unsigned>(buf[i].uxCurrentPriority),
                 static_cast<unsigned>(buf[i].usStackHighWaterMark));
    }
    heap_caps_free(buf);
#else
    ESP_LOGW(kTag, "hwm: CONFIG_FREERTOS_USE_TRACE_FACILITY disabled — rebuild with it enabled");
#endif
}

} // namespace stackchan::app
