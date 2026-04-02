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

#ifndef ACCESS_POINT_H
#define ACCESS_POINT_H

#include "flash_program.h"

/*
 * define's you may change to suit your needs...
 */
#define AP_SSID     "WESTINGHOUSE_CONFIG"
//#define AP_PASSWD "my secret" // define, if you want a password
#define MAGIC       0xCAFF      // used to check if the flash contains valid data
#define LEGACY_MAGIC 0xCAFE     // previous config schema magic
#define CONFIG_VERSION 3
#define SETUP_GPIO  22          // pull this GPIO to GND to force the steup mode
#define SETUP_DELAY 3           // duration for wich SETUP_GPIO must be held low

#define DEBUG   // Uncomment for debug output

#ifdef DEBUG
#define DEBUG_printf(...) printf(__VA_ARGS__)
#else
#define DEBUG_printf(...)
#endif

/*
 * define's you should not modify
 */
//#define TCP_PORT 67

#define SSID_MAX_LEN        32
#define PASSWD_MAX_LEN      63
#define SMTP_SERVER_MAX_LEN 63
#define EMAIL_MAX_LEN       63
#define SMTP_PASS_MAX_LEN   63
#define SETUP_TOKEN_MAX_LEN 31


typedef struct _config {
    uint16_t magic;
    uint16_t version;
    char     ssid[SSID_MAX_LEN + 1];
    char     passwd[PASSWD_MAX_LEN + 1];
    char     smtp_server[SMTP_SERVER_MAX_LEN + 1];
    uint16_t smtp_port;
    char     sender_email[EMAIL_MAX_LEN + 1];
    char     sender_password[SMTP_PASS_MAX_LEN + 1];
    char     recipient_email[EMAIL_MAX_LEN + 1];
} config;

extern config *_c;
extern bool isConfigured;

bool forceSetup();
void run_access_point(config *config);

#endif // ACCESS_POINT_H
