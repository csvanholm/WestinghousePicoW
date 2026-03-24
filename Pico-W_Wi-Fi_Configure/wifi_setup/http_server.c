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
#include "firmware_version.h"

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
    "SMTPSRV", // 2
    "SMTPPRT", // 3
    "SENDEML", // 4
    "SENDPWD", // 5
    "RCPTEML", // 6
    "TOKN",    // 7
    "ERRMSG",  // 8
    "ERRSTYL", // 9
    "WIFIMSG", // 10
    "MAILMSG", // 11
    "TOKNMSG", // 12
    "TOKNROW", // 13
    "OKSSID",  // 14
    "OKSMTP",  // 15
    "OKRCPT",  // 16
    "FWVER",   // 17
};

#define HIGHLIGHT "STYLE=\"background-color: #72A4D2;\""
static bool smtp_err = false;
static bool token_err = false;
static char validation_message[256];
static char wifi_validation_message[96];
static char mail_validation_message[128];
static char token_validation_message[96];

static void clear_messages(void)
{
    validation_message[0] = '\0';
    wifi_validation_message[0] = '\0';
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

static char to_lower_ascii(char c)
{
    if ((c >= 'A') && (c <= 'Z')) {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    size_t text_len;
    size_t needle_len;

    if ((text == NULL) || (needle == NULL) || (needle[0] == '\0')) {
        return false;
    }

    text_len = strlen(text);
    needle_len = strlen(needle);
    if (needle_len > text_len) {
        return false;
    }

    for (size_t i = 0; i <= (text_len - needle_len); i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            if (to_lower_ascii(text[i + j]) != to_lower_ascii(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

static bool ends_with_case_insensitive(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if ((text == NULL) || (suffix == NULL) || (suffix[0] == '\0')) {
        return false;
    }

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return false;
    }

    for (size_t i = 0; i < suffix_len; i++) {
        if (to_lower_ascii(text[text_len - suffix_len + i]) != to_lower_ascii(suffix[i])) {
            return false;
        }
    }

    return true;
}

static bool smtp_sender_matches_provider(const char *smtp_server, const char *sender_email)
{
    if ((smtp_server == NULL) || (sender_email == NULL)) {
        return false;
    }

    // If the host suggests a major provider, enforce matching sender domains.
    if (contains_case_insensitive(smtp_server, "gmail")) {
        return ends_with_case_insensitive(sender_email, "@gmail.com");
    }

    if (contains_case_insensitive(smtp_server, "yahoo")) {
        return ends_with_case_insensitive(sender_email, "@yahoo.com") ||
               ends_with_case_insensitive(sender_email, "@ymail.com") ||
               ends_with_case_insensitive(sender_email, "@rocketmail.com");
    }

    if (contains_case_insensitive(smtp_server, "outlook") ||
        contains_case_insensitive(smtp_server, "office365") ||
        contains_case_insensitive(smtp_server, "hotmail")) {
        return ends_with_case_insensitive(sender_email, "@outlook.com") ||
               ends_with_case_insensitive(sender_email, "@hotmail.com") ||
               ends_with_case_insensitive(sender_email, "@live.com") ||
               ends_with_case_insensitive(sender_email, "@msn.com");
    }

    return true;
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

        case 2: /* smtp server */
            encode_value(_c->smtp_server, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && (_c->smtp_server[0] == '\0')) ? HIGHLIGHT : "");
            break;

        case 3: /* smtp port */
            printed = snprintf(pcInsert, iInsertLen, "value=\"%u\" %s", (unsigned)_c->smtp_port,
                               smtp_err ? HIGHLIGHT : "");
            break;

        case 4: /* sender email */
            encode_value(_c->sender_email, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && !has_at_sign(_c->sender_email)) ? HIGHLIGHT : "");
            break;

        case 5: /* sender password */
            encode_value(_c->sender_password, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && (_c->sender_password[0] == '\0')) ? HIGHLIGHT : "");
            break;

        case 6: /* recipient email */
            encode_value(_c->recipient_email, webStr);
            printed = snprintf(pcInsert, iInsertLen, "value=\"%s\" %s", webStr,
                               (smtp_err && !has_at_sign(_c->recipient_email)) ? HIGHLIGHT : "");
            break;

        case 7: /* setup token */
            if (token_err) {
                printed = snprintf(pcInsert, iInsertLen, "%s", HIGHLIGHT);
            } else {
                printed = snprintf(pcInsert, iInsertLen, "%s", "");
            }
            break;

        case 8: /* validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", validation_message);
            break;

        case 9: /* error banner visibility */
            printed = snprintf(pcInsert, iInsertLen, "%s",
                               (validation_message[0] == '\0') ? "style=\"display:none;\"" : "");
            break;

        case 10: /* wifi validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", wifi_validation_message);
            break;

        case 11: /* mail validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", mail_validation_message);
            break;

        case 12: /* token validation message */
            printed = snprintf(pcInsert, iInsertLen, "%s", token_validation_message);
            break;

        case 13: /* token row visibility */
#ifdef SETUP_PORTAL_TOKEN
            printed = snprintf(pcInsert, iInsertLen, "%s", "");
#else
            printed = snprintf(pcInsert, iInsertLen, "style=\"display:none;\"");
#endif
            break;

        case 14: /* saved SSID for done page */
            encode_value(_c->ssid, webStr);
            printed = snprintf(pcInsert, iInsertLen, "%s", webStr);
            break;

        case 15: /* saved SMTP host for done page */
            encode_value(_c->smtp_server, webStr);
            printed = snprintf(pcInsert, iInsertLen, "%s", webStr);
            break;

        case 16: /* saved recipient for done page */
            encode_value(_c->recipient_email, webStr);
            printed = snprintf(pcInsert, iInsertLen, "%s", webStr);
            break;

        case 17: /* firmware/version footer */
            printed = snprintf(pcInsert,
                               iInsertLen,
                               "FW %s | Config v%u | Build %s %s",
                               APP_FIRMWARE_VERSION,
                               (unsigned)CONFIG_VERSION,
                               __DATE__,
                               __TIME__);
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
    bool provider_mismatch = false;

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
    }
    if ((_c->smtp_server[0] == '\0') ||
        (_c->smtp_port == 0) ||
        (_c->sender_email[0] == '\0') ||
        (_c->sender_password[0] == '\0') ||
        (_c->recipient_email[0] == '\0') ||
        !has_at_sign(_c->sender_email) ||
        !has_at_sign(_c->recipient_email)) {
        smtp_err = true;
    }

    if (!smtp_err && !smtp_sender_matches_provider(_c->smtp_server, _c->sender_email)) {
        smtp_err = true;
        provider_mismatch = true;
    }

    if ((_c->ssid[0] == '\0') || (_c->passwd[0] == '\0')) {
        append_message(wifi_validation_message,
                       sizeof(wifi_validation_message),
                       "Wi-Fi SSID and password are required.");
    }

    if (smtp_err) {
        append_message(mail_validation_message,
                       sizeof(mail_validation_message),
                       "Enter SMTP host, port, sender email, sender password, and recipient email.");
        append_validation_message("SMTP settings are incomplete or email addresses are invalid.");
        if (provider_mismatch) {
            append_message(mail_validation_message,
                           sizeof(mail_validation_message),
                           "Sender email must match the SMTP provider domain (e.g., Gmail host with @gmail.com sender)."
                           );
            append_validation_message("SMTP host and sender account appear to belong to different providers.");
        }
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

    if(!(smtp_err || token_err))
        DEBUG_printf("Configure OK\n");
    else
        DEBUG_printf("Configure ERROR\n");

    if(!(smtp_err || token_err)){
        _c->magic = MAGIC;
        _c->version = CONFIG_VERSION;
        isConfigured = true;
        return "/done.shtml";
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


