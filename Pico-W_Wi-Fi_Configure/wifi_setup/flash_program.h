/**
 * @file flash_program.h
 * @brief Flash persistence interface for the setup configuration store.
 *
 * Purpose:
 * - defines the reserved flash page and helper functions used to save Wi-Fi and SMTP configuration
 * - keeps the device settings durable across power cycles and runtime restarts
 *
 * Relationships:
 * - flash_program.c implements the persistence helpers used by access_point.c and main.c
 * - access_point.h stores the configuration schema that gets serialized into this flash region
 */
/**
 * This file is part of "Wi-Fi Configure.
 *
 * This software eliminates the need to know the network name, password and,
 * if required, IP address, network mask and default gateway at compile time.
 * These can be set directly on the Pico-W and also changed afterwards.
 *
 * Copyright (c) 2024 Gerhard Schiller gerhard.schiller@pm.me
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FLASH_PROGRAMM_H
#define FLASH_PROGRAMM_H

// The wifi-configuration data goes into page 0
// (Pages go from 0 to FLASH_PAGES_PER_SECTOR)
#define WIFI_CONFIG_PAGE 0

void show_stats();
void flash_erase_page(size_t pageStart, size_t numPages);
void flash_write_page(uint8_t *data, uint16_t buf_len, size_t pageStart);
void flash_read(uint8_t *data,uint16_t len, size_t pageStart);

#endif // FLASH_PROGRAMM_H
