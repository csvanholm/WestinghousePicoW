/**
 * @file http_server.h
 * @brief HTTP UI callback interface for the Wi-Fi configuration portal.
 *
 * Purpose:
 * - declares the SSI and CGI hooks used to render and process the web-based setup pages
 * - exposes the string encoding and parameter handling logic for the setup interface
 *
 * Relationships:
 * - http_server.c implements the HTML page logic and dynamic values shown by the portal
 * - access_point.c and main.c activate the server during configuration mode so users can save Wi-Fi and SMTP settings
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

#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

void ssi_init();
u16_t __time_critical_func(ssi_handler)(int iIndex, char *pcInsert, int iInsertLen);
void encode_value(char *src, char *dest);

void cgi_init(void);
const char *cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);
void url_decode(char *src, char *dest);

#endif // __HTTP_SERVER_H__
