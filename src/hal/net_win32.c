/**
 * @file net_win32.c
 * @brief Winsock implementation of hal/net.h
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>

#include "../../include/hal/net.h"

bool hal_net_init(void)
{
    WSADATA wd;
    return WSAStartup(MAKEWORD(2, 2), &wd) == 0;       /* WSAStartup is reference counted */
}

void hal_net_shutdown(void)
{
    WSACleanup();
}

hal_sock_t hal_tcp_connect_start(uint32_t ip, uint16_t port)
{
    SOCKET k = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (k == INVALID_SOCKET) return HAL_SOCK_INVALID;
    u_long nb = 1;
    ioctlsocket(k, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(k, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    if (connect(k, (const struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(k);
        return HAL_SOCK_INVALID;
    }
    return (hal_sock_t)k;
}

int hal_tcp_connect_poll(hal_sock_t s)
{
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET((SOCKET)s, &w); FD_SET((SOCKET)s, &e);
    struct timeval tv = { 0, 0 };
    int n = select(0, NULL, &w, &e, &tv);
    if (n == SOCKET_ERROR) return -1;
    if (n == 0) return 0;
    if (FD_ISSET((SOCKET)s, &e)) return -1;
    if (!FD_ISSET((SOCKET)s, &w)) return 0;
    int err = 0, len = (int)sizeof(err);
    if (getsockopt((SOCKET)s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err != 0) return -1;
    return 1;
}

int hal_tcp_send(hal_sock_t s, const void *buf, size_t len)
{
    int n = send((SOCKET)s, (const char *)buf, (int)len, 0);
    if (n >= 0) return n;
    return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}

int hal_tcp_recv(hal_sock_t s, void *buf, size_t cap)
{
    int n = recv((SOCKET)s, (char *)buf, (int)cap, 0);
    if (n > 0) return n;
    if (n == 0) return -1;                                  /* orderly close */
    return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}

void hal_tcp_close(hal_sock_t s)
{
    if (s != HAL_SOCK_INVALID) closesocket((SOCKET)s);
}

int hal_net_wait(const hal_sock_t *rd, int nrd, const hal_sock_t *wr, int nwr, uint32_t timeout_ms,
                 uint32_t *rd_ready, uint32_t *wr_ready)
{
    if (rd_ready) *rd_ready = 0;
    if (wr_ready) *wr_ready = 0;
    if (nrd < 0 || nwr < 0 || nrd > HAL_NET_WAIT_MAX || nwr > HAL_NET_WAIT_MAX) return -1;
    if (nrd == 0 && nwr == 0) return 0;                     /* select() rejects three empty sets */

    fd_set rs, ws, es;
    FD_ZERO(&rs); FD_ZERO(&ws); FD_ZERO(&es);
    for (int i = 0; i < nrd; i++) FD_SET((SOCKET)rd[i], &rs);
    for (int i = 0; i < nwr; i++) { FD_SET((SOCKET)wr[i], &ws); FD_SET((SOCKET)wr[i], &es); }
    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    int n = select(0, nrd ? &rs : NULL, nwr ? &ws : NULL, nwr ? &es : NULL, &tv);
    if (n == SOCKET_ERROR) return -1;
    if (n == 0) return 0;

    int ready = 0;
    for (int i = 0; i < nrd; i++) {
        if (FD_ISSET((SOCKET)rd[i], &rs)) { if (rd_ready) *rd_ready |= 1u << i; ready++; }
    }
    for (int i = 0; i < nwr; i++) {
        if (FD_ISSET((SOCKET)wr[i], &ws) || FD_ISSET((SOCKET)wr[i], &es)) { if (wr_ready) *wr_ready |= 1u << i; ready++; }
    }
    return ready;
}
