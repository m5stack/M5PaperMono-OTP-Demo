/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <M5Unified.h>
#include <EDP_OTP_LUT_demo.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char TAG[] = "otp_lut_demo";

}  // namespace

extern "C" void app_main(void)
{
    auto config          = M5.config();
    config.clear_display = false;
    M5.begin(config);

    ESP_LOGI(TAG, "board=%d; touch-driven raw EPD demo", static_cast<int>(M5.getBoard()));
    if (!demo_begin()) {
        ESP_LOGE(TAG, "failed to display initial upper-left block");
    }

    while (true) {
        M5.update();
        const auto& touch = M5.Touch.getDetail();
        if (touch.wasPressed()) {
            ESP_LOGI(TAG, "touch x=%d y=%d", touch.x, touch.y);
            if (!demo_next()) {
                ESP_LOGI(TAG, "no further demo step");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
