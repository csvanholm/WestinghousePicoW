/**
 * @file tcp_test_server.h
 * @brief Public interface for the simple TCP validation server used during setup and network tests.
 *
 * Purpose:
 * - declares the TCP server state used to verify the Pico network path in setup mode
 * - provides a small reusable server interface for validating client connections during provisioning
 *
 * Relationships:
 * - tcp_test_server.c implements the socket handling behind this interface
 * - the setup flow can call this server to exercise the network stack before final Wi-Fi or SMTP configuration is accepted
 */
/**
 * This file is part of "Wi-Fi Configure.
 *
 * This software makes it unnecessary to know the network name, password
 * and - if required - IP address, network mask and default gateway when
 * compiling. These can be set directly on the Pico-W and also modified
 * afterwards.
 *
 * Copyright (c) 2024 Gerhard Schiller gerhard.schiller@pm.me
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __TCP_SERVER_H__
#define __TCP_SERVER_H__

#define TEST_PORT 4711
#define BUF_SIZE 2048
#define TEST_ITERATIONS 10
#define POLL_TIME_S 5

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    bool complete;
    char buffer_sent[BUF_SIZE];
    char buffer_recv[BUF_SIZE];
    int sent_len;
    int recv_len;
    int run_count;
} TCP_SERVER_T;

void run_tcp_server( void (*f)(void) );

#endif // __TCP_SERVER_H__
