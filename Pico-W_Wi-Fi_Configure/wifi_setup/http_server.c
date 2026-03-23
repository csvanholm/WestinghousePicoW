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


#include "lwip/apps/httpd.h"
#include "http_server.h"
#include "pico/cyw43_arch.h"
#include "access_point.h"

/*
 * This file contains the code for SSI and CGI handling.
 *
 * Server Side Includes (SSI):
 * The tags (enclosed in "<!--" and "-->") embedded in web pages are
 * replaced by the server with dynamic text before the document is
 * delivered to the client. See ssi_handler().
 *
 * The Common Gateway Interface (CGI) allows a web server to delegate the
 * execution of a request.
 * This is exploited here by redirecting a call for a non-existent page to a
 * routine. See cgi_handler()
 */
const char * __not_in_flash("httpd") ssi_tags[] = {
    "SSID",    // 0
    "PASSWD",  // 1
    "B0",      // 2
    "B1",      // 3
    "B2",      // 4
    "B3",      // 5
    "B4",      // 6
    "B5",      // 7
    "B6",      // 8
    "B7",      // 9
    "B8",      // 10
    "B9",      // 11
    "B10",     // 12
    "B11",     // 13
    "SMTPSRV", // 14
    "SMTPPRT", // 15
    "SENDEML", // 16
    "SENDPWD", // 17
    "RCPTEML", // 18
    "TOKN",    // 19
    "ERRMSG",  // 20
    "ERRSTYL", // 21
    "WIFIMSG", // 22
    "LANMSG",  // 23
    "MAILMSG", // 24
    "TOKNMSG", // 25
    "TOKNROW", // 26
    "OKSSID",  // 27
    "OKSMTP",  // 28
    "OKRCPT",  // 29
};

#define HIGHLIGHT "STYLE=\"background-color: #72A4D2;\""
static uint8_t  lan[3][4];
bool _need_ip;
bool _need_gw;

static bool ip_err = false;
static bool mask_err = false;
static bool gw_err = false;
static bool smtp_err = false;
static bool token_err = false;
static char validation_message[256];
static char wifi_validation_message[96];
static char lan_validation_message[96];
static char mail_validation_message[128];
static char token_validation_message[96];

static void clear_messages(void)
{
    validation_message[0] = '\0';
    wifi_validation_message[0] = '\0';
    lan_validation_message[0] = '\0';
    mail_validation_message[0] = '\0';
    token_validation_message[0] = '\0';
}

static void append_message(char *buffer, size_t buffer_len, const char *message)
{
    size_t used;

    if ((buffer == NULL) || (buffer_len == 0) || (message == NULL) || (message[0] == '\0')) {
        return;
    }

    used = strlen(buffer);
    if (used >= (buffer_len - 1)) {
        return;
    }

    if (used > 0) {
        int printed = snprintf(buffer + used,
                               buffer_len - used,
                               " %s",
                               message);
        if (printed < 0) {
            buffer[used] = '\0';
        }
    } else {
        snprintf(buffer, buffer_len, "%s", message);
    }
}

static void append_validation_message(const char *message)
{
    append_message(validation_message, sizeof(validation_message), message);
}

static bool has_at_sign(const char *text)
{
    if (text == NULL) {
        return false;
    }
    return strchr(text, '@') != NULL;
}

/*
 * ssi_init()
 *
 * Check ssi-tags for length and inizialize the ssi handler
 */

void ssi_init()
{
    size_t i;
    for (i = 0; i < LWIP_ARRAYSIZE(ssi_tags); i++) {
        LWIP_ASSERT("tag too long for LWIP_HTTPD_MAX_TAG_NAME_LEN",
                    strlen(ssi_tags[i]) <= LWIP_HTTPD_MAX_TAG_NAME_LEN);
    }

    http_set_ssi_handler(ssi_handler, ssi_tags, LWIP_ARRAYSIZE(ssi_tags));
}

/*
 * ssi_handler()
 *
 * SSI is triggered by the file extension ".shtml"
 */

u16_t __time_critical_func(ssi_handler)(int iIndex, char *pcInsert, int iInsertLen)
{
    // SSID and password may contain quotation marks which must be
    // converted to "&quote;" for the web site.
    // So we make the buffer twice the maximum size.
    // (and hope a full-length password doesn't contain
    // more than nine quotation marks)
    // static char webStr[PASSWD_MAX_LEN * 2];
    char webStr[PASSWD_MAX_LEN * 2];

    size_t printed = 0;
    switch (iIndex) {
        case 0: /* "SSID" */
            if(*(_c->ssid) == '\0'){
                printed = snprintf(pcInsert, iInsertLen, "%s", HIGHLIGHT);
                break;
            }
            encode_value(_c->ssid, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\"", webStr);
            break;
        case 1: /* "password" */
        {
            encode_value(_c->passwd, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\"", webStr);
        }
        break;

        case 2: /* "static ip address a */
        case 3: /* "static ip address b */
        case 4: /* "static ip address c */
        case 5: /* "static ip address d */
            if(_c->ip.addr == IPADDR_NONE){
                if(_need_ip)
                    printed = snprintf(pcInsert, iInsertLen, "%s", HIGHLIGHT);
            }
            else if(ip_err){
                printed = snprintf(pcInsert, iInsertLen, "value=\"%d\" %s",
                ip4_addr_get_byte(&(_c->ip), iIndex - 2), HIGHLIGHT);
            }
            else{
                printed = snprintf(pcInsert, iInsertLen, "value=\"%d\"",
                ip4_addr_get_byte(&(_c->ip), iIndex - 2));
            }
        break;

        case 6: /* "net mask address a */
        case 7: /* "net mask address b */
        case 8: /* "net mask address c */
        case 9: /* "net mask address d */
            if(_c->mask.addr == IPADDR_NONE){
                if(_need_ip)
                    printed = snprintf(pcInsert, iInsertLen, "%s", HIGHLIGHT);
            }
            else if(mask_err){
                printed = snprintf(pcInsert, iInsertLen, "value=\"%d\" %s",
                                   ip4_addr_get_byte(&(_c->mask), iIndex - 6), HIGHLIGHT);
            }
            else{
                printed = snprintf(pcInsert, iInsertLen, "value=\"%d\"",
                                   ip4_addr_get_byte(&(_c->mask), iIndex - 6));
            }
            break;

        case 10: /* "def gateway address a */
        case 11: /* "def gateway address b */
        case 12: /* "def gateway address c */
        case 13: /* "def gateway address d */
            if(_c->gw.addr == IPADDR_NONE){
                if(_need_gw)
                    printed = snprintf(pcInsert, iInsertLen, "%s", HIGHLIGHT);
            }
            else if(gw_err){
                printed = snprintf(pcInsert, iInsertLen, "value=\"%d\" %s",
                                   ip4_addr_get_byte(&(_c->gw), iIndex - 10), HIGHLIGHT);
            }
            else{
                printed = snprintf(pcInsert, iInsertLen, "value=\"%d\"",
                                   ip4_addr_get_byte(&(_c->gw), iIndex - 10));
            }
            break;

        case 14: /* smtp server */
            encode_value(_c->smtp_server, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && (_c->smtp_server[0] == '\0')) ? HIGHLIGHT : "");
            break;

        case 15: /* smtp port */
            printed = snprintf(pcInsert, iInsertLen, "value=\"%u\" %s", (unsigned)_c->smtp_port,
                               smtp_err ? HIGHLIGHT : "");
            break;

        case 16: /* sender email */
            encode_value(_c->sender_email, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && !has_at_sign(_c->sender_email)) ? HIGHLIGHT : "");
            break;

        case 17: /* sender password */
            encode_value(_c->sender_password, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && (_c->sender_password[0] == '\0')) ? HIGHLIGHT : "");
            break;

        case 18: /* recipient email */
            encode_value(_c->recipient_email, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && !has_at_sign(_c->recipient_email)) ? HIGHLIGHT : "");
            break;

        case 19: /* setup token */
            if (token_err) {
                printed = snprintf(pcInsert, iInsertLen, "%s", HIGHLIGHT);
            } else {
                printed = snprintf(pcInsert, iInsertLen, "%s", "");
            }
            break;

        case 20: /* validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", validation_message);
            break;

        case 21: /* error banner visibility */
            printed = snprintf(pcInsert, iInsertLen, "%s",
                               (validation_message[0] == '\0') ? "style=\"display:none;\"" : "");
            break;

        case 22: /* wifi validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", wifi_validation_message);
            break;

        case 23: /* lan validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", lan_validation_message);
            break;

        case 24: /* mail validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", mail_validation_message);
            break;

        case 25: /* token validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", token_validation_message);
            break;

        case 26: /* token row visibility */
#ifdef SETUP_PORTAL_TOKEN
            printed = snprintf(pcInsert, iInsertLen, "%s", "");
#else
            printed = snprintf(pcInsert, iInsertLen, "style=\"display:none;\"");
#endif
            break;

        case 27: /* saved SSID for done page */
            encode_value(_c->ssid, webStr);
            printed = snprintf(pcInsert, iInsertLen, "%s", webStr);
            break;

        case 28: /* saved SMTP host for done page */
            encode_value(_c->smtp_server, webStr);
            printed = snprintf(pcInsert, iInsertLen, "%s", webStr);
            break;

        case 29: /* saved recipient for done page */
            encode_value(_c->recipient_email, webStr);
            printed = snprintf(pcInsert, iInsertLen, "%s", webStr);
            break;
    }
    LWIP_ASSERT("sane length", printed <= 0xFFFF);
    return (u16_t)printed;
}

/*
 * encode_value()
 *
 * SSID and password may contain quotation marks which must be
 * converted to "&quote;" for the web site.
 */

void encode_value(char *src, char *dest)
{
    while(*src){
        if(*src == '"'){
            *dest++ = '&';
            *dest++ = 'q';
            *dest++ = 'u';
            *dest++ = 'o';
            *dest++ = 't';
            *dest++ = ';';
            src++;
        }
        else{
            *dest++ = *src++;
        }
    }
    *dest = '\0';
}

/* Html request for "/setup.cgi" will start cgi_handler_setup */
static const tCGI cgi_handlers[] = {
    {"/setup.cgi", cgi_handler},
};

/*
 * cgi_init()
 *
 * initialize the CGI handler
 */

void
cgi_init(void)
{
    http_set_cgi_handlers(cgi_handlers, 1);
}

/*
 * cgi_handler()
 *
 * This cgi handler triggered by a request for "/setup.cgi"
 */

const char *
cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    memset(lan, 0, sizeof(lan));
    ip_err   = false;
    mask_err = false;
    gw_err   = false;
    smtp_err = false;
    token_err = false;
    clear_messages();

#ifdef SETUP_PORTAL_TOKEN
    bool token_present = false;
    bool token_match = false;
#endif

    for (int i = 0; i < iNumParams; i++){
        if(strcmp(pcParam[i], "ssid") == 0){
            url_decode(pcValue[i], _c->ssid);
        }
        else if(strcmp(pcParam[i], "passwd") == 0){
            url_decode(pcValue[i], _c->passwd);
        }
        else if(strcmp(pcParam[i], "smtp_server") == 0){
            url_decode(pcValue[i], _c->smtp_server);
        }
        else if(strcmp(pcParam[i], "smtp_port") == 0){
            if (pcValue[i][0] != '\0') {
                int port = atoi(pcValue[i]);
                if ((port <= 0) || (port > 65535)) {
                    smtp_err = true;
                } else {
                    _c->smtp_port = (uint16_t)port;
                }
            }
        }
        else if(strcmp(pcParam[i], "sender_email") == 0){
            url_decode(pcValue[i], _c->sender_email);
        }
        else if(strcmp(pcParam[i], "sender_password") == 0){
            url_decode(pcValue[i], _c->sender_password);
        }
        else if(strcmp(pcParam[i], "recipient_email") == 0){
            url_decode(pcValue[i], _c->recipient_email);
        }
        else if(strcmp(pcParam[i], "setup_token") == 0){
    #ifdef SETUP_PORTAL_TOKEN
            token_present = true;
            token_match = (strcmp(pcValue[i], SETUP_PORTAL_TOKEN) == 0);
    #endif
        }
        else if(pcParam[i][0] == 'B'){
            uint8_t index = atoi(&(pcParam[i][1]));
            int val = atoi(pcValue[i]);
            switch(index){
                case 0:
                case 1:
                case 2:
                case 3: // IP address
                    if(pcValue[i][0] == '\0' && _need_ip)
                        ip_err = true;
                    else if(val < 0 || val > 255)
                        ip_err = true;
                    else
                        lan[index/4][index%4] = val;
                    break;

                case 4:
                case 5:
                case 6:
                case 7: // net mask
                    if(pcValue[i][0] == '\0' && _need_ip)
                        mask_err = true;
                    else if(val < 0 || val > 255)
                        mask_err = true;
                    else
                        lan[index/4][index%4] = val;
                    break;

                case 8:
                case 9:
                case 10:
                case 11: // default gateway
                    if(pcValue[i][0] == '\0' && _need_gw)
                        gw_err = true;
                    else if(val < 0 || val > 255)
                        gw_err = true;
                    else
                        lan[index/4][index%4] = val;
                    break;

            }
        }
    }
    IP4_ADDR(&(_c->ip),   lan[0][0], lan[0][1], lan[0][2], lan[0][3]);
    if(!_c->ip.addr)
        _c->ip.addr = IPADDR_NONE;

    IP4_ADDR(&(_c->mask), lan[1][0], lan[1][1], lan[1][2], lan[1][3]);
    if(!_c->mask.addr)
        _c->mask.addr = IPADDR_NONE;

    IP4_ADDR(&(_c->gw),   lan[2][0], lan[2][1], lan[2][2], lan[2][3]);
    if(!_c->gw.addr)
        _c->gw.addr = IPADDR_NONE;


    DEBUG_printf("IP %s\n", ip4addr_ntoa(&(_c->ip)));
    DEBUG_printf("NM %s\n", ip4addr_ntoa(&(_c->mask)));
    DEBUG_printf("GW %s\n", ip4addr_ntoa(&(_c->gw)));
    if ((_c->smtp_server[0] == '\0') ||
        (_c->smtp_port == 0) ||
        (_c->sender_email[0] == '\0') ||
        (_c->sender_password[0] == '\0') ||
        (_c->recipient_email[0] == '\0') ||
        !has_at_sign(_c->sender_email) ||
        !has_at_sign(_c->recipient_email)) {
        smtp_err = true;
    }

    if ((_c->ssid[0] == '\0') || (_c->passwd[0] == '\0')) {
        append_message(wifi_validation_message,
                       sizeof(wifi_validation_message),
                       "Wi-Fi SSID and password are required.");
    }

    if (ip_err || mask_err || gw_err) {
        append_message(lan_validation_message,
                       sizeof(lan_validation_message),
                       "Check IP, netmask, and gateway octets.");
        append_validation_message("LAN settings contain missing or invalid octets.");
    }
    if (smtp_err) {
        append_message(mail_validation_message,
                       sizeof(mail_validation_message),
                       "Enter SMTP host, port, sender email, sender password, and recipient email.");
        append_validation_message("SMTP settings are incomplete or email addresses are invalid.");
    }

#ifdef SETUP_PORTAL_TOKEN
    if ((!token_present) || (!token_match)) {
        token_err = true;
    }
    if (token_err) {
        append_message(token_validation_message,
                       sizeof(token_validation_message),
                       "Provide the correct setup token to save changes.");
        append_validation_message("Setup token is missing or incorrect.");
    }
#endif

    if(!(ip_err || mask_err || gw_err || smtp_err || token_err))
        DEBUG_printf("Configure OK\n");
    else
        DEBUG_printf("Configure ERROR\n");

    if(!(ip_err || mask_err || gw_err || smtp_err || token_err)){
        _c->magic = MAGIC;
        _c->version = CONFIG_VERSION;
        isConfigured = true;
        return "/done.html";
    }
    else{
        return "/index.shtml";
    }
}

/*
 * url_decode()
 *
 * Chars, not allowed in an url, but contained in a get request
 * are encoded.
 * " " as "+" and all others as hex encoded ascii code.
 */

void url_decode(char *src, char *dest)
{
    while(*src){
        if(*src == '+'){
            *dest = ' ';
            src++;
            dest++;
        }
        else if(*src == '%'){
            char a[3];

            a[0] = *++src;
            a[1] = *++src;
            a[2] = '\0';
            *dest++ = strtol(a, NULL, 16);
            src++;
        }
        else{
            *dest++ = *src++;
        }
    }
    *dest = '\0';
}


