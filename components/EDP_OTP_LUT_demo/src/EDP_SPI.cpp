/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "EDP_SPI.h"

#include <M5Unified.h>

#include <algorithm>
#include <cstring>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace edp_spi {
namespace {

constexpr char TAG[] = "edp_spi";

constexpr gpio_num_t PIN_MOSI = GPIO_NUM_14;
constexpr gpio_num_t PIN_SCLK = GPIO_NUM_15;
constexpr gpio_num_t PIN_CS   = GPIO_NUM_16;
constexpr gpio_num_t PIN_DC   = GPIO_NUM_17;
constexpr gpio_num_t PIN_BUSY = GPIO_NUM_18;

constexpr spi_host_device_t SPI_HOST = SPI2_HOST;
constexpr int SPI_CLOCK_HZ           = 20 * 1000 * 1000;
constexpr size_t SPI_CHUNK_SIZE      = 4092;

// M5IOE1 uses zero-based pin indexes.
constexpr uint8_t IOE_EPD_ENABLE = 2;
constexpr uint8_t IOE_EPD_RESET  = 4;

spi_device_handle_t spi_device = nullptr;
uint8_t* staging_buffer        = nullptr;
bool ready                     = false;
bool writing                   = false;
bool write_ok                  = true;

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void set_error(esp_err_t error, const char* operation)
{
    if (write_ok) {
        ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    }
    write_ok = false;
}

void transmit(const uint8_t* bytes, size_t length, bool is_data, bool invert)
{
    if (!write_ok || !writing || length == 0) {
        return;
    }

    gpio_set_level(PIN_DC, is_data);

    while (length != 0 && write_ok) {
        const size_t chunk_size = std::min(length, SPI_CHUNK_SIZE);
        const uint8_t* source   = bytes;

        if (invert) {
            for (size_t i = 0; i < chunk_size; ++i) {
                staging_buffer[i] = static_cast<uint8_t>(~bytes[i]);
            }
            source = staging_buffer;
        } else if (chunk_size > 4) {
            std::memcpy(staging_buffer, bytes, chunk_size);
            source = staging_buffer;
        }

        spi_transaction_t transaction{};
        transaction.length = chunk_size * 8;

        if (chunk_size <= 4 && source == bytes) {
            transaction.flags = SPI_TRANS_USE_TXDATA;
            std::memcpy(transaction.tx_data, source, chunk_size);
        } else {
            transaction.tx_buffer = source;
        }

        const esp_err_t error = spi_device_polling_transmit(spi_device, &transaction);
        if (error != ESP_OK) {
            set_error(error, "spi_device_polling_transmit");
            return;
        }

        bytes += chunk_size;
        length -= chunk_size;
    }
}

}  // namespace

bool begin_write()
{
    write_ok = ready;
    if (!ready || writing) {
        return false;
    }

    const esp_err_t error = spi_device_acquire_bus(spi_device, portMAX_DELAY);
    if (error != ESP_OK) {
        set_error(error, "spi_device_acquire_bus");
        return false;
    }

    gpio_set_level(PIN_CS, 0);
    writing = true;
    return true;
}

bool end_write()
{
    if (writing) {
        gpio_set_level(PIN_CS, 1);
        spi_device_release_bus(spi_device);
        writing = false;
    }
    return write_ok;
}

void write_command(uint8_t command)
{
    transmit(&command, 1, false, false);
}

void write_data(const uint8_t* data, size_t length, bool invert)
{
    transmit(data, length, true, invert);
}

void write_register(uint8_t command, std::initializer_list<uint8_t> data)
{
    write_command(command);
    write_data(data.begin(), data.size());
}

void write_u16(uint16_t value)
{
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
    };
    write_data(bytes, sizeof(bytes));
}

bool wait_ready(uint32_t timeout_ms)
{
    vTaskDelay(pdMS_TO_TICKS(1));
    const uint32_t start = millis();

    while (gpio_get_level(PIN_BUSY)) {
        if (millis() - start >= timeout_ms) {
            ESP_LOGE(TAG, "BUSY timeout after %lu ms", static_cast<unsigned long>(timeout_ms));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

void hardware_reset()
{
    auto& ioe = M5.getIOExpander(0);

    ioe.setHighImpedance(IOE_EPD_ENABLE, false);
    ioe.setDirection(IOE_EPD_ENABLE, true);
    ioe.digitalWrite(IOE_EPD_ENABLE, true);

    ioe.setHighImpedance(IOE_EPD_RESET, false);
    ioe.setDirection(IOE_EPD_RESET, true);
    ioe.digitalWrite(IOE_EPD_RESET, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioe.digitalWrite(IOE_EPD_RESET, true);
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool begin()
{
    M5.Display.releaseBus();

    gpio_config_t output_config{};
    output_config.pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC);
    output_config.mode         = GPIO_MODE_OUTPUT;
    output_config.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&output_config) != ESP_OK) {
        return false;
    }
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 0);

    gpio_config_t busy_config{};
    busy_config.pin_bit_mask = 1ULL << PIN_BUSY;
    busy_config.mode         = GPIO_MODE_INPUT;
    busy_config.pull_up_en   = GPIO_PULLUP_ENABLE;
    busy_config.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&busy_config) != ESP_OK) {
        return false;
    }

    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num     = PIN_MOSI;
    bus_config.miso_io_num     = -1;
    bus_config.sclk_io_num     = PIN_SCLK;
    bus_config.quadwp_io_num   = -1;
    bus_config.quadhd_io_num   = -1;
    bus_config.data4_io_num    = -1;
    bus_config.data5_io_num    = -1;
    bus_config.data6_io_num    = -1;
    bus_config.data7_io_num    = -1;
    bus_config.max_transfer_sz = SPI_CHUNK_SIZE;
    bus_config.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_SCLK;
    bus_config.isr_cpu_id      = ESP_INTR_CPU_AFFINITY_AUTO;

    esp_err_t error = spi_bus_initialize(SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(error));
        return false;
    }

    spi_device_interface_config_t device_config{};
    device_config.mode           = 0;
    device_config.clock_speed_hz = SPI_CLOCK_HZ;
    device_config.spics_io_num   = -1;
    device_config.flags          = SPI_DEVICE_HALFDUPLEX;
    device_config.queue_size     = 1;

    error = spi_bus_add_device(SPI_HOST, &device_config, &spi_device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(error));
        spi_bus_free(SPI_HOST);
        return false;
    }

    staging_buffer =
        static_cast<uint8_t*>(heap_caps_malloc(SPI_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (staging_buffer == nullptr) {
        ESP_LOGE(TAG, "failed to allocate SPI DMA buffer");
        spi_bus_remove_device(spi_device);
        spi_device = nullptr;
        spi_bus_free(SPI_HOST);
        return false;
    }

    ready = true;
    hardware_reset();
    ESP_LOGI(TAG, "SPI2 ready: mode 0, 20 MHz");
    return wait_ready();
}

}  // namespace edp_spi
