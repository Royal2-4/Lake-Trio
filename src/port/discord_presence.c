/*
 * Lake Trio Discord Rich Presence transport (desktop builds only).
 * Discord's documented local IPC protocol; no client secret or OAuth.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "discord_presence.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE s_connection = INVALID_HANDLE_VALUE;
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int s_connection = -1;
#endif

#define DISCORD_APP_ID "1572450125815611413"
#define DISCORD_ICON_KEY "game_icon"
#define DISCORD_MAX_FRAME 16384u

static bool s_ready;
static bool s_changed = true;
static bool s_registered_shutdown;
static uint32_t s_retry_at;
static uint32_t s_last_send;
static uint32_t s_nonce;
static time_t s_started;
static char s_location[120];

static bool PresenceConnected(void)
{
#ifdef _WIN32
    return s_connection != INVALID_HANDLE_VALUE;
#else
    return s_connection >= 0;
#endif
}

static void PresenceDisconnect(void)
{
#ifdef _WIN32
    if (PresenceConnected()) CloseHandle(s_connection);
    s_connection = INVALID_HANDLE_VALUE;
#else
    if (PresenceConnected()) close(s_connection);
    s_connection = -1;
#endif
    s_ready = false;
    s_changed = true;
}

static void PresenceShutdown(void)
{
    PresenceDisconnect();
}

static uint32_t ReadLittle32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void WriteLittle32(unsigned char *p, uint32_t n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
    p[2] = (unsigned char)(n >> 16);
    p[3] = (unsigned char)(n >> 24);
}

static bool WriteBytes(const unsigned char *data, size_t length)
{
    while (length) {
#ifdef _WIN32
        DWORD written = 0;
        DWORD chunk = length > 4096 ? 4096 : (DWORD)length;
        if (!WriteFile(s_connection, data, chunk, &written, NULL) || written == 0)
            return false;
#else
        ssize_t written = send(s_connection, data, length,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
                               );
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
#endif
        data += written;
        length -= (size_t)written;
    }
    return true;
}

static bool SendFrame(uint32_t opcode, const char *payload)
{
    unsigned char header[8];
    size_t length = strlen(payload);
    if (!PresenceConnected() || length > DISCORD_MAX_FRAME) return false;
    WriteLittle32(header, opcode);
    WriteLittle32(header + 4, (uint32_t)length);
    return WriteBytes(header, sizeof(header)) &&
           WriteBytes((const unsigned char *)payload, length);
}

/* Returns 1 for a frame, 0 for no complete frame, or -1 on disconnect/error. */
static int ReceiveFrame(uint32_t *opcode, char *payload)
{
    unsigned char header[8];
    uint32_t length;
    size_t total;
#ifdef _WIN32
    DWORD available = 0, peeked = 0;
    if (!PeekNamedPipe(s_connection, header, sizeof(header), &peeked,
                       &available, NULL)) return -1;
    if (peeked < sizeof(header)) return 0;
#else
    int available = 0;
    ssize_t peeked = recv(s_connection, header, sizeof(header), MSG_PEEK | MSG_DONTWAIT);
    if (peeked == 0) return -1;
    if (peeked < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (peeked < 0) return -1;
    if (peeked < (ssize_t)sizeof(header)) return 0;
    if (ioctl(s_connection, FIONREAD, &available) != 0) return -1;
#endif
    length = ReadLittle32(header + 4);
    if (length >= DISCORD_MAX_FRAME) return -1;
    total = sizeof(header) + length;
    if ((size_t)available < total) return 0;
#ifdef _WIN32
    DWORD taken = 0;
    if (!ReadFile(s_connection, header, sizeof(header), &taken, NULL) ||
        taken != sizeof(header)) return -1;
    if (length && (!ReadFile(s_connection, payload, length, &taken, NULL) ||
                   taken != length)) return -1;
#else
    if (recv(s_connection, header, sizeof(header), MSG_WAITALL) != (ssize_t)sizeof(header))
        return -1;
    if (length && recv(s_connection, payload, length, MSG_WAITALL) != (ssize_t)length)
        return -1;
#endif
    *opcode = ReadLittle32(header);
    payload[length] = '\0';
    return 1;
}

static bool TryConnect(void)
{
    int i;
#ifdef _WIN32
    char path[64];
    for (i = 0; i < 10; ++i) {
        snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);
        s_connection = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                                   0, NULL, OPEN_EXISTING, 0, NULL);
        if (PresenceConnected()) return true;
    }
#else
    const char *roots[] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"),
                            getenv("TMP"), getenv("TEMP"), "/tmp" };
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    size_t r;
    for (r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
        if (!roots[r] || !roots[r][0]) continue;
        for (i = 0; i < 10; ++i) {
            struct sockaddr_un addr;
            int fd;
            int length = snprintf(path, sizeof(path), "%s/discord-ipc-%d", roots[r], i);
            if (length < 0 || (size_t)length >= sizeof(path)) continue;
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return false;
#ifdef SO_NOSIGPIPE
            { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            memcpy(addr.sun_path, path, (size_t)length + 1);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                s_connection = fd;
                return true;
            }
            close(fd);
        }
    }
#endif
    return false;
}

static void EscapeJson(const char *source, char *target, size_t cap)
{
    size_t k = 0;
    while (*source && k + 7 < cap) {
        unsigned char c = (unsigned char)*source++;
        if (c == '\"' || c == '\\') {
            target[k++] = '\\';
            target[k++] = (char)c;
        } else if (c < 32) {
            target[k++] = ' ';
        } else {
            target[k++] = (char)c;
        }
    }
    target[k] = '\0';
}

static bool SendActivity(void)
{
    char json[1024];
    char location[256];
    char state[320];
    unsigned long pid;
#ifdef _WIN32
    pid = GetCurrentProcessId();
#else
    pid = (unsigned long)getpid();
#endif
    EscapeJson(s_location[0] ? s_location : "Sinnoh", location, sizeof(location));
    snprintf(state, sizeof(state), "%s", "At the main menu");
    if (s_location[0]) snprintf(state, sizeof(state), "Exploring %s", location);
    snprintf(json, sizeof(json),
             "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"laketrio-%u\","
             "\"args\":{\"pid\":%lu,\"activity\":{\"type\":0,"
             "\"details\":\"Sinnoh Adventure\",\"state\":\"%s\","
             "\"timestamps\":{\"start\":%lld},"
             "\"assets\":{\"large_image\":\"%s\",\"large_text\":\"Lake Trio\"}}}}",
             ++s_nonce, pid, state, (long long)s_started, DISCORD_ICON_KEY);
    return SendFrame(1, json);
}

void DiscordPresence_SetLocation(const char *location)
{
    char safe[sizeof(s_location)];
    size_t i;
    if (!location) location = "";
    for (i = 0; i < sizeof(safe) - 1 && location[i]; ++i)
        safe[i] = location[i];
    safe[i] = '\0';
    if (strcmp(safe, s_location) != 0) {
        memcpy(s_location, safe, i + 1);
        s_changed = true;
    }
}

void DiscordPresence_ClearLocation(void)
{
    DiscordPresence_SetLocation("");
}

void DiscordPresence_Tick(uint32_t milliseconds)
{
    char response[DISCORD_MAX_FRAME];
    uint32_t opcode;
    int i;
    const char *disabled = getenv("LAKETRIO_DISCORD_DISABLE");
    if (disabled && disabled[0] == '1' && disabled[1] == '\0') {
        if (PresenceConnected()) PresenceDisconnect();
        return;
    }
    if (!s_registered_shutdown) {
        s_registered_shutdown = true;
        s_started = time(NULL);
        atexit(PresenceShutdown);
    }
    if (!PresenceConnected()) {
        if ((int32_t)(milliseconds - s_retry_at) < 0) return;
        s_retry_at = milliseconds + 5000;
        if (!TryConnect()) return;
        if (!SendFrame(0, "{\"v\":1,\"client_id\":\"" DISCORD_APP_ID "\"}")) {
            PresenceDisconnect();
            return;
        }
        s_ready = false;
    }
    for (i = 0; i < 8; ++i) {
        int got = ReceiveFrame(&opcode, response);
        if (got < 0 || (got > 0 && opcode == 2)) {
            PresenceDisconnect();
            return;
        }
        if (got == 0) break;
        if (opcode == 3) {
            if (!SendFrame(4, response)) { PresenceDisconnect(); return; }
        } else if (opcode == 1 && strstr(response, "\"READY\"") && !s_ready) {
            s_ready = true;
            s_changed = true;
            puts("[discord] Rich Presence connection ready");
        } else if (opcode == 1 && strstr(response, "\"ERROR\"")) {
            fprintf(stderr, "[discord] RPC error: %.300s\n", response);
        }
    }
    if (s_ready && (s_changed || (uint32_t)(milliseconds - s_last_send) > 30000u)) {
        if (s_last_send != 0 && (uint32_t)(milliseconds - s_last_send) < 5000u)
            return;
        if (!SendActivity()) {
            PresenceDisconnect();
            return;
        }
        if (s_changed)
            printf("[discord] Activity submitted: %s\n",
                   s_location[0] ? s_location : "At the main menu");
        s_last_send = milliseconds;
        s_changed = false;
    }
}
