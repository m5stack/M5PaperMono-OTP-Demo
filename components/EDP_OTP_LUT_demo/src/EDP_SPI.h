/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace edp_spi {

bool begin();
bool begin_write();
bool end_write();
bool wait_ready(uint32_t timeout_ms = 15000);

void hardware_reset();
void write_command(uint8_t command);
void write_data(const uint8_t* data, size_t length, bool invert = false);
void write_register(uint8_t command, std::initializer_list<uint8_t> data);
void write_u16(uint16_t value);

}  // namespace edp_spi
