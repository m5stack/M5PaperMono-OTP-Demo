/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "EDP_OTP_LUT_demo.h"
#include "EDP_SPI.h"

#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char TAG[] = "otp_lut_demo";

// Native controller resolution. Monochrome pixels are packed MSB first:
// 800 pixels / 8 = 100 bytes per row, 480 rows per frame.
constexpr uint16_t DISPLAY_WIDTH  = 800;
constexpr uint16_t DISPLAY_HEIGHT = 480;
constexpr size_t BYTES_PER_ROW    = DISPLAY_WIDTH / 8;
constexpr size_t FRAME_SIZE       = BYTES_PER_ROW * DISPLAY_HEIGHT;

// SSD1677 commands used by all three refresh examples.
constexpr uint8_t CMD_SOFT_RESET        = 0x12;
constexpr uint8_t CMD_DEEP_SLEEP        = 0x10;
constexpr uint8_t CMD_MASTER_ACTIVATION = 0x20;
constexpr uint8_t CMD_UPDATE_CONTROL    = 0x22;
constexpr uint8_t CMD_WRITE_RAM_1       = 0x24;
constexpr uint8_t CMD_WRITE_RAM_2       = 0x26;
constexpr uint8_t CMD_DATA_ENTRY_MODE   = 0x11;
constexpr uint8_t CMD_RAM_X_RANGE       = 0x44;
constexpr uint8_t CMD_RAM_Y_RANGE       = 0x45;
constexpr uint8_t CMD_RAM_X_COUNTER     = 0x4E;
constexpr uint8_t CMD_RAM_Y_COUNTER     = 0x4F;

struct Rect {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
};

// The controller RAM axes are rotated relative to the visible display:
// RAM X selects the visible vertical position and RAM Y selects horizontal.
// The array order below is therefore the visible order, not numeric RAM order.
constexpr Rect BLOCKS[4] = {
    {0, 0, 400, 240},      // upper-left
    {0, 240, 400, 240},    // upper-right
    {400, 0, 400, 240},    // lower-left
    {400, 240, 400, 240},  // lower-right
};

uint8_t* mono_frame   = nullptr;
uint8_t* gray_plane_1 = nullptr;
uint8_t* gray_plane_2 = nullptr;

// Partial updates require a known monochrome image in the controller RAM.
// A four-gray update invalidates that monochrome baseline.
bool driver_ready   = false;
bool baseline_ready = false;
uint8_t demo_step   = 0;

Rect full_screen()
{
    return {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
}

bool align_rect(Rect& rect)
{
    if (rect.width == 0 || rect.height == 0 || rect.x >= DISPLAY_WIDTH || rect.y >= DISPLAY_HEIGHT) {
        return false;
    }

    const uint32_t right  = std::min<uint32_t>(DISPLAY_WIDTH, rect.x + rect.width);
    const uint32_t bottom = std::min<uint32_t>(DISPLAY_HEIGHT, rect.y + rect.height);

    // RAM X is byte-packed, so every transfer must start and end on a byte.
    rect.x &= ~7u;
    rect.width  = static_cast<uint16_t>(((right + 7u) & ~7u) - rect.x);
    rect.height = static_cast<uint16_t>(bottom - rect.y);
    return rect.width != 0 && rect.height != 0;
}

bool allocate_frames()
{
    if (mono_frame != nullptr) {
        return true;
    }

    // One plane is sufficient for monochrome. Four-gray mode uses two 1-bit
    // planes, giving four possible bit combinations for each pixel.
    uint8_t* memory = static_cast<uint8_t*>(heap_caps_malloc(FRAME_SIZE * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (memory == nullptr) {
        ESP_LOGE(TAG, "failed to allocate %u-byte frame buffer", static_cast<unsigned>(FRAME_SIZE * 3));
        return false;
    }

    mono_frame   = memory;
    gray_plane_1 = memory + FRAME_SIZE;
    gray_plane_2 = gray_plane_1 + FRAME_SIZE;
    return true;
}

void fill_screen(bool white)
{
    // In monochrome RAM, 1 is white and 0 is black.
    std::memset(mono_frame, white ? 0xFF : 0x00, FRAME_SIZE);
}

void fill_rect(Rect rect, bool white)
{
    if (!align_rect(rect)) {
        return;
    }

    const size_t x_offset   = rect.x / 8;
    const size_t row_length = rect.width / 8;

    for (uint16_t y = rect.y; y < rect.y + rect.height; ++y) {
        uint8_t* row = mono_frame + y * BYTES_PER_ROW + x_offset;
        std::memset(row, white ? 0xFF : 0x00, row_length);
    }
}

void make_bw_quadrants()
{
    // Two black and two white quadrants form a simple full-refresh test.
    fill_screen(true);
    fill_rect(BLOCKS[0], false);
    fill_rect(BLOCKS[3], false);
}

void make_gray_quadrants()
{
    // Four-gray encoding used by the OTP waveform:
    // white=00, light gray=10, dark gray=01, black=11 (plane 1, plane 2).
    for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
        const bool top_half  = y < DISPLAY_HEIGHT / 2;
        uint8_t* plane_1_row = gray_plane_1 + y * BYTES_PER_ROW;
        uint8_t* plane_2_row = gray_plane_2 + y * BYTES_PER_ROW;

        // Right half: light gray above, dark gray below.
        std::memset(plane_1_row, top_half ? 0xFF : 0x00, BYTES_PER_ROW / 2);
        std::memset(plane_2_row, top_half ? 0x00 : 0xFF, BYTES_PER_ROW / 2);

        // Left half: white above, black below.
        std::memset(plane_1_row + BYTES_PER_ROW / 2, top_half ? 0x00 : 0xFF, BYTES_PER_ROW / 2);
        std::memset(plane_2_row + BYTES_PER_ROW / 2, top_half ? 0x00 : 0xFF, BYTES_PER_ROW / 2);
    }
}

void set_ram_window(Rect rect)
{
    const uint16_t x_end = rect.x + rect.width - 1;
    const uint16_t y_end = rect.y + rect.height - 1;

    // 0x03 advances both RAM counters after each byte/row is written.
    edp_spi::write_register(CMD_DATA_ENTRY_MODE, {0x03});

    edp_spi::write_command(CMD_RAM_X_RANGE);
    edp_spi::write_u16(rect.x);
    edp_spi::write_u16(x_end);

    edp_spi::write_command(CMD_RAM_Y_RANGE);
    edp_spi::write_u16(rect.y);
    edp_spi::write_u16(y_end);

    edp_spi::write_command(CMD_RAM_X_COUNTER);
    edp_spi::write_u16(rect.x);

    edp_spi::write_command(CMD_RAM_Y_COUNTER);
    edp_spi::write_u16(rect.y);
}

void write_ram(uint8_t command, const uint8_t* frame, Rect rect, bool invert = false)
{
    set_ram_window(rect);
    edp_spi::write_command(command);

    const size_t x_offset    = rect.x / 8;
    const size_t row_length  = rect.width / 8;
    const uint8_t* first_row = frame + rect.y * BYTES_PER_ROW + x_offset;

    // A full-width area is contiguous in memory and can be sent in one call.
    if (row_length == BYTES_PER_ROW) {
        edp_spi::write_data(first_row, row_length * rect.height, invert);
        return;
    }

    for (uint16_t y = 0; y < rect.height; ++y) {
        edp_spi::write_data(first_row + y * BYTES_PER_ROW, row_length, invert);
    }
}

bool software_reset()
{
    // Software reset is used for full updates. It is intentionally omitted
    // when waking for a partial update so the RAM baseline is retained.
    if (!edp_spi::wait_ready() || !edp_spi::begin_write()) {
        return false;
    }
    edp_spi::write_command(CMD_SOFT_RESET);
    if (!edp_spi::end_write()) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return edp_spi::wait_ready();
}

bool init_mono_mode()
{
    if (!software_reset() || !edp_spi::begin_write()) {
        return false;
    }

    edp_spi::write_register(0x18, {0x80});                          // Internal temperature sensor.
    edp_spi::write_register(0x0C, {0xAE, 0xC7, 0xC3, 0xC0, 0x80});  // Booster soft-start.
    edp_spi::write_register(0x01, {0xDF, 0x01, 0x02});              // 480 gate outputs.
    edp_spi::write_register(0x3C, {0x01});                          // Border waveform.
    edp_spi::write_register(0x21, {0x00});                          // Normal RAM display mode.
    set_ram_window(full_screen());
    return edp_spi::end_write();
}

bool deep_sleep()
{
    if (!edp_spi::begin_write()) {
        return false;
    }
    // Deep Sleep Mode 1 keeps the RAM contents needed by the next partial
    // update. A hardware reset is required to leave this mode.
    edp_spi::write_register(CMD_DEEP_SLEEP, {0x01});
    if (!edp_spi::end_write()) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

bool wake_for_partial_update()
{
    // Hardware reset exits deep sleep. No software reset is sent here.
    edp_spi::hardware_reset();
    if (!edp_spi::wait_ready() || !edp_spi::begin_write()) {
        return false;
    }
    edp_spi::write_register(0x3C, {0x80});  // Float the border during partial refresh.
    return edp_spi::end_write();
}

bool refresh_mono_full()
{
    ESP_LOGI(TAG, "full monochrome refresh: OTP mode 1");
    edp_spi::hardware_reset();
    if (!init_mono_mode() || !edp_spi::begin_write()) {
        return false;
    }

    // First synchronize the controller drive state with an inverted frame.
    edp_spi::write_register(CMD_UPDATE_CONTROL, {0xF8});
    write_ram(CMD_WRITE_RAM_1, mono_frame, full_screen(), true);
    edp_spi::write_command(CMD_MASTER_ACTIVATION);
    if (!edp_spi::end_write() || !edp_spi::wait_ready()) {
        return false;
    }

    if (!edp_spi::begin_write()) {
        return false;
    }
    // 0x14 loads and runs the built-in OTP Mode 1 waveform. Both RAM planes
    // receive the same image so it becomes a valid partial-update baseline.
    edp_spi::write_register(CMD_UPDATE_CONTROL, {0x14});
    write_ram(CMD_WRITE_RAM_2, mono_frame, full_screen());
    write_ram(CMD_WRITE_RAM_1, mono_frame, full_screen());
    edp_spi::write_command(CMD_MASTER_ACTIVATION);
    if (!edp_spi::end_write() || !edp_spi::wait_ready()) {
        return false;
    }

    baseline_ready = true;
    return deep_sleep();
}

bool load_white_baseline()
{
    // Load both RAM planes with white but do not issue master activation.
    // Consequently, the first visible operation after boot is a partial
    // update rather than a full-screen flash.
    fill_screen(true);
    edp_spi::hardware_reset();
    if (!init_mono_mode() || !edp_spi::begin_write()) {
        return false;
    }

    write_ram(CMD_WRITE_RAM_2, mono_frame, full_screen());
    write_ram(CMD_WRITE_RAM_1, mono_frame, full_screen());
    if (!edp_spi::end_write()) {
        return false;
    }

    baseline_ready = true;
    return deep_sleep();
}

bool refresh_partial()
{
    if (!baseline_ready || !wake_for_partial_update() || !edp_spi::begin_write()) {
        return false;
    }

    // The complete next frame is written to RAM 1 only. Command value 0xFF
    // runs the controller's built-in partial-update sequence; no LUT data is
    // uploaded by this demo.
    write_ram(CMD_WRITE_RAM_1, mono_frame, full_screen());
    edp_spi::write_register(0x21, {0x00});
    edp_spi::write_register(CMD_UPDATE_CONTROL, {0xFF});
    edp_spi::write_command(CMD_MASTER_ACTIVATION);
    if (!edp_spi::end_write() || !edp_spi::wait_ready()) {
        return false;
    }

    return deep_sleep();
}

bool init_gray_mode()
{
    edp_spi::hardware_reset();
    if (!software_reset() || !edp_spi::begin_write()) {
        return false;
    }

    edp_spi::write_register(0x0C, {0xAE, 0xC7, 0xC3, 0xC0, 0x80});  // Booster soft-start.
    edp_spi::write_register(0x01, {0xDF, 0x01, 0x02});              // 480 gate outputs.

    // Four-gray data is streamed with RAM X decreasing and RAM Y increasing.
    edp_spi::write_register(CMD_DATA_ENTRY_MODE, {0x02});

    edp_spi::write_command(CMD_RAM_X_RANGE);
    edp_spi::write_u16(DISPLAY_WIDTH - 1);
    edp_spi::write_u16(0);

    edp_spi::write_command(CMD_RAM_Y_RANGE);
    edp_spi::write_u16(0);
    edp_spi::write_u16(DISPLAY_HEIGHT - 1);

    edp_spi::write_command(CMD_RAM_X_COUNTER);
    edp_spi::write_u16(DISPLAY_WIDTH - 1);
    edp_spi::write_command(CMD_RAM_Y_COUNTER);
    edp_spi::write_u16(0);

    edp_spi::write_register(0x3C, {0x01});  // Border waveform.
    edp_spi::write_register(0x18, {0x80});  // Internal temperature sensor.
    edp_spi::write_register(0x1A, {0x5A});  // Gray-mode temperature value.
    return edp_spi::end_write();
}

bool refresh_gray_full()
{
    ESP_LOGI(TAG, "full 4-gray refresh: OTP waveform");
    if (!init_gray_mode() || !edp_spi::begin_write()) {
        return false;
    }

    // Each plane contributes one bit to the four-level pixel value. 0xD7
    // starts the controller's built-in four-gray OTP waveform.
    edp_spi::write_command(CMD_WRITE_RAM_1);
    edp_spi::write_data(gray_plane_1, FRAME_SIZE);
    edp_spi::write_command(CMD_WRITE_RAM_2);
    edp_spi::write_data(gray_plane_2, FRAME_SIZE);
    edp_spi::write_register(CMD_UPDATE_CONTROL, {0xD7});
    edp_spi::write_command(CMD_MASTER_ACTIVATION);
    if (!edp_spi::end_write() || !edp_spi::wait_ready()) {
        return false;
    }

    baseline_ready = false;
    return deep_sleep();
}

}  // namespace

bool demo_begin()
{
    driver_ready   = false;
    baseline_ready = false;
    demo_step      = 0;

    // Initialize transport, allocate the three frame planes, and establish a
    // white RAM baseline without visibly refreshing the panel.
    if (!edp_spi::begin() || !allocate_frames() || !load_white_baseline()) {
        ESP_LOGE(TAG, "display initialization failed");
        return false;
    }

    driver_ready = true;
    fill_rect(BLOCKS[0], false);
    ESP_LOGI(TAG, "partial block: upper-left");
    return refresh_partial();
}

bool demo_next()
{
    if (!driver_ready) {
        return false;
    }

    // Steps 0..3 move one black quadrant using the partial OTP waveform.
    if (demo_step < 3) {
        fill_rect(BLOCKS[demo_step], true);
        fill_rect(BLOCKS[demo_step + 1], false);
        ++demo_step;
        ESP_LOGI(TAG, "partial block step: %u", static_cast<unsigned>(demo_step));
        return refresh_partial();
    }

    // Step 4 demonstrates a monochrome full refresh.
    if (demo_step == 3) {
        make_bw_quadrants();
        ++demo_step;
        return refresh_mono_full();
    }

    // Step 5 demonstrates a full refresh using all four gray levels.
    if (demo_step == 4) {
        make_gray_quadrants();
        ++demo_step;
        return refresh_gray_full();
    }

    // Four-gray mode cannot be used as a monochrome differential baseline.
    // Rebuild a white monochrome baseline before restarting the partial demo.
    fill_screen(true);
    if (!refresh_mono_full()) {
        return false;
    }

    fill_rect(BLOCKS[0], false);
    demo_step = 0;
    return refresh_partial();
}
