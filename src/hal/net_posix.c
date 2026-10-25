/**
 * @file net_posix.c
 * @brief BSD-socket implementation of hal/net.h (Linux, Raspberry Pi, ESP-IDF / lwIP)
 *
 * Retries on EINTR, maps EAGAIN / EINPROGRESS to "pending", and never raises SIGPIPE
 * (MSG_NOSIGNAL). poll() is used instead of select() so descriptors above FD_SETSIZE work.
 */

#include <errno.h>
#include <fcntl.h>
#if defined(ESP_PLATFORM)
#include <sys/poll.h>
#else
#include <poll.h>
#endif
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "../../include/hal/net.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

bool hal_net_init(void)     { return true; }
void hal_net_shutdown(void) {}

hal_sock_t hal_tcp_connect_start(uint32_t ip, uint16_t port)
{
    int k = socket(AF_INET, SOCK_STREAM, 0);
    if (k < 0) return HAL_SOCK_INVALID;
    int fl = fcntl(k, F_GETFL, 0);
    if (fl < 0 || fcntl(k, F_SETFL, fl | O_NONBLOCK) < 0) { close(k); return HAL_SOCK_INVALID; }
    int one = 1;
    setsockopt(k, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    /* EINTR on a non-blocking connect: the connection carries on in the background */
    if (connect(k, (const struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS && errno != EINTR) {
        close(k);
        return HAL_SOCK_INVALID;
    }
    return (hal_sock_t)k;
}

int hal_tcp_connect_poll(hal_sock_t s)
{
    struct pollfd pf;
    pf.fd = (int)s;
    pf.events = POLLOUT;
    pf.revents = 0;
    int rc = poll(&pf, 1, 0);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (rc == 0 || !(pf.revents & (POLLOUT | POLLERR | POLLHUP))) return 0;
    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);
    if (getsockopt((int)s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return -1;
    return (pf.revents & POLLOUT) ? 1 : -1;
}

int hal_tcp_send(hal_sock_t s, const void *buf, size_t len)
{
    for (;;) {
        ssize_t n = send((int)s, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
}

int hal_tcp_recv(hal_sock_t s, void *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv((int)s, buf, cap, 0);
        if (n > 0) return (int)n;
        if (n == 0) return -1;                              /* orderly close */
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
}

void hal_tcp_close(hal_sock_t s)
{
    if (s != HAL_SOCK_INVALID) close((int)s);
}

int hal_net_wait(const hal_sock_t *rd, int nrd, const hal_sock_t *wr, int nwr, uint32_t timeout_ms,
                 uint32_t *rd_ready, uint32_t *wr_ready)
{
    struct pollfd pf[2 * HAL_NET_WAIT_MAX];
    if (rd_ready) *rd_ready = 0;
    if (wr_ready) *wr_ready = 0;
    if (nrd < 0 || nwr < 0 || nrd > HAL_NET_WAIT_MAX || nwr > HAL_NET_WAIT_MAX) return -1;
    if (nrd == 0 && nwr == 0) return 0;

    int n = 0;
    for (int i = 0; i < nrd; i++, n++) { pf[n].fd = (int)rd[i]; pf[n].events = POLLIN;  pf[n].revents = 0; }
    for (int i = 0; i < nwr; i++, n++) { pf[n].fd = (int)wr[i]; pf[n].events = POLLOUT; pf[n].revents = 0; }
    int rc = poll(pf, (nfds_t)n, timeout_ms > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)timeout_ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (rc == 0) return 0;

    int ready = 0;
    for (int i = 0; i < nrd; i++) {
        if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) { if (rd_ready) *rd_ready |= 1u << i; ready++; }
    }
    for (int i = 0; i < nwr; i++) {
        if (pf[nrd + i].revents & (POLLOUT | POLLHUP | POLLERR)) { if (wr_ready) *wr_ready |= 1u << i; ready++; }
    }
    return ready;
}
