/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 title's log channel: the ps5log functions that native/pw_diagnostics.c
 * and the title call (declared by native/ps5log/ps5log.h; this file is the
 * implementation for Orbis, so PS5LOG_IMPLEMENTATION is never defined), over
 * UDP datagrams to the development PC and the kernel log.
 *
 * Transport: one datagram per record, at most 512 bytes, through libSceNet
 * (sceNetSocket/sceNetSendto with a BSD sockaddr_in carrying sin_len; the
 * SDK's musl struct has no length byte and the kernel wants one). The
 * records keep ps5log's wire form, <seq>\t<mono_ns>\t<level>\t<text>, so a
 * receiver that prints lines shows them as the PS5's does; ps5log_raw sends
 * the bytes as they are, which is how the milestone check line travels.
 *
 * The kernel log (sceKernelDebugOutText) costs 8-15 ms a line and blocks
 * when nothing reads it, so only orbislog_klog() writes there: the title
 * uses it for its PS4WINE lines and the fatal records.
 *
 * The destination is compiled in (PW_ORBIS_NETLOG_HOST/PORT) and can be
 * overridden by DEV_SERVER=/DEV_PORT= lines in <root>/dev.conf (the title's PW_ROOT). */
#include "ps5log/ps5log.h"
#include "orbislog.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PW_ORBIS_NETLOG_HOST
#define PW_ORBIS_NETLOG_HOST ""
#endif
#ifndef PW_ORBIS_NETLOG_PORT
#define PW_ORBIS_NETLOG_PORT 18194
#endif
#define ORBISLOG_DATAGRAM 512

/* libSceNet and libkernel, by prototype: the SDK's headers declare them
 * with types this file does not need. */
extern int sceNetInit(void);
extern int sceNetSocket(const char *name, int family, int type, int protocol);
extern int sceNetSocketClose(int socket);
extern int sceNetInetPton(int family, const char *text, void *address);
extern int sceNetSendto(int socket, const void *data, size_t length, int flags, const void *address,
                        unsigned address_length);
extern int sceKernelDebugOutText(int channel, const char *text);

struct bsd_sockaddr_in {
    uint8_t len, family;
    uint16_t port;      /* network order */
    uint32_t address;   /* network order */
    uint8_t zero[8];
};

static struct {
    int socket;
    int enabled;
    uint64_t seq;
    ps5log_config cfg;
    ps5log_stats stats;
    struct bsd_sockaddr_in to;
    char title[32], app[48];
} g = { .socket = -1 };

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void ps5log_config_defaults(ps5log_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->server, sizeof(cfg->server), "%s", PW_ORBIS_NETLOG_HOST);
    cfg->port = PW_ORBIS_NETLOG_PORT;
    cfg->enabled = 1;
    cfg->udp = 1;
    cfg->connect_timeout_ms = 500;
    cfg->send_timeout_ms = 200;
}

/* DEV_SERVER=, DEV_PORT=, DEV_ENABLED= lines; anything else is ignored. */
int ps5log_parse_config(const char *text, size_t length, ps5log_config *cfg)
{
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') end++;
        {
            size_t n = end - at;
            char line[128];
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, text + at, n);
            line[n] = 0;
            if (!strncmp(line, "DEV_SERVER=", 11)) snprintf(cfg->server, sizeof(cfg->server), "%s", line + 11);
            else if (!strncmp(line, "DEV_PORT=", 9)) cfg->port = (uint16_t)atoi(line + 9);
            else if (!strncmp(line, "DEV_ENABLED=", 12)) cfg->enabled = atoi(line + 12);
        }
        at = end + 1;
    }
    return 0;
}

int ps5log_load_config(const char *const *paths, size_t count, ps5log_config *cfg, const char **used_path)
{
    char text[PS5LOG_CONF_MAX];
    if (used_path) *used_path = NULL;
    for (size_t i = 0; i < count; i++) {
        int fd = open(paths[i], O_RDONLY);
        ssize_t got;
        if (fd < 0) continue;
        got = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (got <= 0) continue;
        ps5log_parse_config(text, (size_t)got, cfg);
        if (used_path) *used_path = paths[i];
        return 0;
    }
    return -1;
}

static int open_channel(void)
{
    if (!g.cfg.enabled || !g.cfg.server[0]) return -1;
    if (g.socket < 0) {
        sceNetInit();
        g.socket = sceNetSocket("pw-orbislog", 2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0);
        if (g.socket < 0) { g.stats.disabled_reason = "sceNetSocket failed"; return -1; }
    }
    memset(&g.to, 0, sizeof(g.to));
    g.to.len = sizeof(g.to);
    g.to.family = 2;
    g.to.port = (uint16_t)((g.cfg.port >> 8) | (g.cfg.port << 8));
    if (sceNetInetPton(2, g.cfg.server, &g.to.address) != 1) {
        g.stats.disabled_reason = "bad DEV_SERVER";
        return -1;
    }
    g.stats.disabled_reason = NULL;
    return 0;
}

int ps5log_init(const ps5log_config *cfg, const char *title, const char *app, uint64_t boot_token)
{
    char hello[160];
    (void)boot_token;
    g.cfg = *cfg;
    snprintf(g.title, sizeof(g.title), "%s", title ? title : "");
    snprintf(g.app, sizeof(g.app), "%s", app ? app : "");
    g.enabled = open_channel() == 0;
    g.seq = 0;
    if (!g.enabled) return -1;
    snprintf(hello, sizeof(hello), "HELLO " PS5LOG_PROTOCOL " title=%s app=%s boot=0x%llx tag=orbis\n",
             g.title, g.app, (unsigned long long)mono_ns());
    ps5log_raw(hello, strlen(hello));
    return 0;
}

int ps5log_init_default(const char *title, const char *app)
{
    ps5log_config cfg;
    ps5log_config_defaults(&cfg);
    return ps5log_init(&cfg, title, app, 0);
}

int ps5log_enabled(void) { return g.enabled; }

int ps5log_reconnect(void)
{
    g.enabled = open_channel() == 0;
    return g.enabled ? 0 : -1;
}

/* One datagram, cut at the datagram size; a record longer than that keeps
 * its head. Never blocks: UDP sendto fails rather than waits. */
int ps5log_raw(const void *bytes, size_t length)
{
    if (!g.enabled) { g.stats.lines_dropped++; return -1; }
    if (length > ORBISLOG_DATAGRAM) length = ORBISLOG_DATAGRAM;
    if (sceNetSendto(g.socket, bytes, length, 0, &g.to, sizeof(g.to)) < 0) {
        g.stats.lines_dropped++;
        g.stats.last_errno = errno;
        return -1;
    }
    g.stats.lines_sent++;
    g.stats.bytes_sent += length;
    return 0;
}

int ps5log_line(const char *level, const char *text)
{
    char record[PS5LOG_MAX_LINE];
    int n = snprintf(record, sizeof(record), "%llu\t%llu\t%s\t%s\n", (unsigned long long)++g.seq,
                     (unsigned long long)mono_ns(), level ? level : PS5LOG_INFO, text ? text : "");
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(record)) n = (int)sizeof(record) - 1;
    return ps5log_raw(record, (size_t)n);
}

int ps5log_printf(const char *level, const char *format, ...)
{
    char text[PS5LOG_MAX_LINE];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return ps5log_line(level, text);
}

void ps5log_close(const char *reason)
{
    char bye[128];
    if (!g.enabled) return;
    snprintf(bye, sizeof(bye), "BYE seq=%llu reason=%s\n", (unsigned long long)g.seq, reason ? reason : "");
    ps5log_raw(bye, strlen(bye));
    /* The socket stays open: the title idles after the session and may log
     * again (pw_diagnostics_resume). */
}

void ps5log_stats_get(ps5log_stats *out) { *out = g.stats; }

void orbislog_klog(const char *line)
{
    char text[ORBISLOG_DATAGRAM + 2];
    size_t n = strlen(line);
    if (n > ORBISLOG_DATAGRAM) n = ORBISLOG_DATAGRAM;
    memcpy(text, line, n);
    if (!n || text[n - 1] != '\n') text[n++] = '\n';
    text[n] = 0;
    sceKernelDebugOutText(0, text);
}
