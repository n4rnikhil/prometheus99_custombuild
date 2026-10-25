/**
 * @file net.h
 * @brief Minimal non-blocking TCP client layer (IPv4) used by the Modbus drivers
 *
 * Two implementations: net_win32.c (Winsock) and net_posix.c (BSD sockets: Linux,
 * Raspberry Pi, ESP-IDF/lwIP). Nothing here allocates memory. Every call returns
 * immediately; waiting is done only by hal_net_wait(), which the discovery thread uses
 * and which the 1 kHz fieldbus loop never calls.
 */

#ifndef INDUSTRIAL_HMI_HAL_NET_H
#define INDUSTRIAL_HMI_HAL_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef intptr_t hal_sock_t;
#define HAL_SOCK_INVALID ((hal_sock_t)-1)
#define HAL_NET_WAIT_MAX 32          /* sockets per list in hal_net_wait() */

bool hal_net_init(void);             /* reference counted; call once per user */
void hal_net_shutdown(void);

/* Start a non-blocking connect (TCP_NODELAY set). HAL_SOCK_INVALID = failed at once. */
hal_sock_t hal_tcp_connect_start(uint32_t ip_host_order, uint16_t port);
/* 1 = connected, 0 = still connecting, -1 = failed. Never waits. */
int hal_tcp_connect_poll(hal_sock_t s);
/* Bytes accepted by the stack (may be less than len), 0 = would block, -1 = error. */
int hal_tcp_send(hal_sock_t s, const void *buf, size_t len);
/* Bytes received (> 0), 0 = nothing available now, -1 = closed by peer or error. */
int hal_tcp_recv(hal_sock_t s, void *buf, size_t cap);
void hal_tcp_close(hal_sock_t s);

/* Wait until a socket in rd[] is readable or one in wr[] is writable / has failed.
 * Bit i of *rd_ready / *wr_ready is set for rd[i] / wr[i]. Either list may be empty and
 * either mask pointer may be NULL. Returns the number of ready sockets, 0 on timeout
 * or signal, -1 on error. Each list holds at most HAL_NET_WAIT_MAX sockets. */
int hal_net_wait(const hal_sock_t *rd, int nrd, const hal_sock_t *wr, int nwr, uint32_t timeout_ms,
                 uint32_t *rd_ready, uint32_t *wr_ready);

/* Dotted-quad helpers (portable, no libc resolver). Address in host byte order. */
bool hal_ip_parse(const char *text, uint32_t *ip_host_order);   /* "a.b.c.d" or "localhost" */
void hal_ip_format(uint32_t ip_host_order, char out[16]);

#endif /* INDUSTRIAL_HMI_HAL_NET_H */
