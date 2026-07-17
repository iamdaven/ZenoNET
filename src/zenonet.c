// zenonet.c, the entire C implementation in one file, core networking plus all the extra subsystems

#include "../include/zenonet.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#define CLOSE_SOCKET closesocket
#define SOCKET_ERRNO WSAGetLastError()
#define SOCKET_TYPE SOCKET
#define SOCKET_INVALID INVALID_SOCKET
#define SOCKET_ERR SOCKET_ERROR
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#define CLOSE_SOCKET close
#define SOCKET_ERRNO errno
#define SOCKET_TYPE int
#define SOCKET_INVALID (-1)
#define SOCKET_ERR (-1)
#endif

// ===========================================================================
// internal helpers
// ===========================================================================

static log_callback g_log_cb = NULL;
static void *g_log_ud = NULL;

void zn_set_log_callback(log_callback cb, void *userdata) {
    g_log_cb = cb; g_log_ud = userdata;
}

void zn_log(const char *fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_log_cb) g_log_cb(buf, g_log_ud);
    else printf("%s", buf);
}

void zn_hexdump(const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16) printf("\n");
}

int zn_version_major(void) { return 0; }
int zn_version_minor(void) { return 3; }
const char *zn_version_string(void) { return ZN_VERSION; }

uint32_t zn_timestamp(void) { return (uint32_t)time(NULL); }

static void gen_id(char *buf, int len) {
    static const char chars[] = "abcdef0123456789";
    for (int i = 0; i < len - 1; i++) buf[i] = chars[rand() % 16];
    buf[len - 1] = '\0';
}

static void safe_strcpy(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (src) {
        size_t i = 0;
        for (; src[i] && i < dst_size - 1; i++) dst[i] = src[i];
        dst[i] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static int g_winsock_refs = 0;

static void ensure_winsock(void) {
#ifdef _WIN32
    if (g_winsock_refs++ == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            zn_log("[!] WSAStartup failed\n");
        }
    }
#endif
}

static void set_nonblock(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

#ifdef _WIN32
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <time.h>
static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}
#define SLEEP_MS(ms) sleep_ms(ms)
#endif

static bool recv_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// hex helpers used by file transfer, replay and voice
static void hex_encode(const uint8_t *data, uint32_t len, char *out) {
    static const char h[] = "0123456789abcdef";
    for (uint32_t i = 0; i < len; i++) {
        out[i * 2] = h[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = h[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static uint32_t hex_decode(const char *hex, uint8_t *out, uint32_t max_out) {
    uint32_t n = (uint32_t)strlen(hex) / 2;
    if (n > max_out) n = max_out;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (uint8_t)((hex_val(hex[i * 2]) << 4) | hex_val(hex[i * 2 + 1]));
    }
    return n;
}

// ===========================================================================
// packet implementation
// ===========================================================================

Packet *zn_packet_create(PacketType type, const char *data_json) {
    Packet *pkt = (Packet *)calloc(1, sizeof(Packet));
    if (!pkt) return NULL;
    gen_id(pkt->id, sizeof(pkt->id));
    pkt->type = type;
    pkt->sequence = 0;
    pkt->room[0] = '\0';
    pkt->target[0] = '\0';
    pkt->priority = ZN_PRIORITY_NORMAL;
    pkt->timestamp = zn_timestamp();
    pkt->client_id = 0;
    pkt->flags = ZN_FLAG_NONE;
    if (data_json) {
        pkt->data_len = (uint32_t)strlen(data_json) + 1;
        pkt->data = (char *)malloc(pkt->data_len);
        if (pkt->data) memcpy(pkt->data, data_json, pkt->data_len);
    } else {
        pkt->data = strdup("{}");
        pkt->data_len = 3;
    }
    return pkt;
}

Packet *zn_packet_create_with_room(PacketType type, const char *data_json, const char *room) {
    Packet *pkt = zn_packet_create(type, data_json);
    if (pkt && room) strncpy(pkt->room, room, sizeof(pkt->room) - 1);
    return pkt;
}

Packet *zn_packet_create_with_target(PacketType type, const char *data_json, const char *target) {
    Packet *pkt = zn_packet_create(type, data_json);
    if (pkt && target) strncpy(pkt->target, target, sizeof(pkt->target) - 1);
    return pkt;
}

void zn_packet_free(Packet *pkt) {
    if (pkt) {
        if (pkt->data) free(pkt->data);
        free(pkt);
    }
}

Packet *zn_packet_clone(const Packet *pkt) {
    if (!pkt) return NULL;
    Packet *clone = (Packet *)calloc(1, sizeof(Packet));
    if (!clone) return NULL;
    memcpy(clone->id, pkt->id, ZN_ID_LEN);
    clone->type = pkt->type;
    memcpy(clone->room, pkt->room, ZN_ROOM_NAME_LEN);
    memcpy(clone->target, pkt->target, ZN_TARGET_LEN);
    clone->sequence = pkt->sequence;
    clone->priority = pkt->priority;
    clone->timestamp = pkt->timestamp;
    clone->client_id = pkt->client_id;
    clone->flags = pkt->flags;
    if (pkt->data && pkt->data_len > 0) {
        clone->data = (char *)malloc(pkt->data_len);
        if (clone->data) memcpy(clone->data, pkt->data, pkt->data_len);
    } else {
        clone->data = strdup("{}");
        clone->data_len = 3;
    }
    return clone;
}

// legacy json based wire format, kept for backward compatibility and tests
uint8_t *zn_packet_serialize(Packet *pkt, uint32_t *out_len) {
    if (!pkt) return NULL;
    char header_buf[512];
    int hlen = snprintf(header_buf, sizeof(header_buf),
        "{\"id\":\"%s\",\"type\":%d,\"room\":\"%s\",\"target\":\"%s\",\"seq\":%u}",
        pkt->id, (int)pkt->type, pkt->room, pkt->target, pkt->sequence);
    if (hlen < 0 || hlen >= (int)sizeof(header_buf)) return NULL;
    uint16_t header_len = (uint16_t)hlen;
    uint32_t total = sizeof(uint32_t) + sizeof(uint16_t) + header_len + pkt->data_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;
    memcpy(buf, &total, 4);
    memcpy(buf + 4, &header_len, 2);
    memcpy(buf + 6, header_buf, header_len);
    memcpy(buf + 6 + header_len, pkt->data, pkt->data_len);
    if (out_len) *out_len = total;
    return buf;
}

static char *json_strval(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    char *start = strstr((char *)json, search);
    if (!start) {
        snprintf(search, sizeof(search), "\"%s\":", key);
        start = strstr((char *)json, search);
        if (!start) return NULL;
        start += strlen(key) + 3;
        char *end = strchr(start, ',');
        if (!end) end = strchr(start, '}');
        if (!end) return NULL;
        char *val = (char *)malloc((size_t)(end - start) + 1);
        if (!val) return NULL;
        memcpy(val, start, (size_t)(end - start));
        val[end - start] = '\0';
        return val;
    }
    start += strlen(key) + 5;
    char *end = strchr(start, '"');
    if (!end) return NULL;
    char *val = (char *)malloc((size_t)(end - start) + 1);
    if (!val) return NULL;
    memcpy(val, start, (size_t)(end - start));
    val[end - start] = '\0';
    return val;
}

Packet *zn_packet_deserialize(const uint8_t *raw, uint32_t len) {
    if (!raw || len < 6) return NULL;
    uint16_t header_len;
    memcpy(&header_len, raw + 4, 2);
    if (6 + header_len > len) return NULL;
    Packet *pkt = (Packet *)calloc(1, sizeof(Packet));
    if (!pkt) return NULL;
    char header_buf[512];
    uint16_t copy_len = header_len < (uint16_t)(sizeof(header_buf) - 1) ? header_len : (uint16_t)(sizeof(header_buf) - 1);
    memcpy(header_buf, raw + 6, copy_len);
    header_buf[copy_len] = '\0';
    char *v;
    v = json_strval(header_buf, "id");
    if (v) { safe_strcpy(pkt->id, v, sizeof(pkt->id)); free(v); }
    v = json_strval(header_buf, "type");
    if (v) { pkt->type = (PacketType)atoi(v); free(v); }
    v = json_strval(header_buf, "room");
    if (v) { safe_strcpy(pkt->room, v, sizeof(pkt->room)); free(v); }
    v = json_strval(header_buf, "target");
    if (v) { safe_strcpy(pkt->target, v, sizeof(pkt->target)); free(v); }
    v = json_strval(header_buf, "seq");
    if (v) { pkt->sequence = (uint32_t)atoi(v); free(v); }
    pkt->priority = ZN_PRIORITY_NORMAL;
    pkt->timestamp = zn_timestamp();
    pkt->client_id = 0;
    pkt->flags = ZN_FLAG_NONE;
    uint32_t data_len = len - 6 - header_len;
    if (data_len > 0 && data_len < ZN_MAX_PACKET) {
        pkt->data = (char *)malloc(data_len + 1);
        if (pkt->data) {
            memcpy(pkt->data, raw + 6 + header_len, data_len);
            pkt->data[data_len] = '\0';
            pkt->data_len = data_len + 1;
        }
    } else {
        pkt->data = strdup("{}");
        pkt->data_len = 3;
    }
    return pkt;
}

char *zn_packet_to_json(const Packet *pkt) {
    if (!pkt) return strdup("null");
    char *buf = (char *)malloc(2048);
    if (!buf) return NULL;
    snprintf(buf, 2048,
        "{\"id\":\"%s\",\"type\":%d,\"room\":\"%s\",\"target\":\"%s\",\"seq\":%u,\"pri\":%u,\"ts\":%u,\"cid\":%u,\"flags\":%u,\"data\":%s}",
        pkt->id, (int)pkt->type, pkt->room, pkt->target, pkt->sequence,
        (unsigned)pkt->priority, pkt->timestamp, pkt->client_id, (unsigned)pkt->flags,
        pkt->data ? pkt->data : "{}");
    return buf;
}

// packet metadata accessors
void zn_packet_set_priority(Packet *pkt, uint8_t priority) { if (pkt) pkt->priority = priority; }
uint8_t zn_packet_get_priority(const Packet *pkt) { return pkt ? pkt->priority : 0; }
void zn_packet_set_timestamp(Packet *pkt, uint32_t ts) { if (pkt) pkt->timestamp = ts; }
uint32_t zn_packet_get_timestamp(const Packet *pkt) { return pkt ? pkt->timestamp : 0; }
void zn_packet_set_client_id(Packet *pkt, uint32_t cid) { if (pkt) pkt->client_id = cid; }
uint32_t zn_packet_get_client_id(const Packet *pkt) { return pkt ? pkt->client_id : 0; }
void zn_packet_set_flags(Packet *pkt, uint8_t flags) { if (pkt) pkt->flags = flags; }
uint8_t zn_packet_get_flags(const Packet *pkt) { return pkt ? pkt->flags : 0; }

// ===========================================================================
// ZNP (Zeno Network Protocol) binary wire format
// header: version, priority, flags, type, client_id, timestamp, sequence
// then id, room, target, data as length prefixed strings
// ===========================================================================

uint8_t zn_znp_version(void) { return ZN_ZNP_VERSION; }

uint8_t *zn_znp_serialize(Packet *pkt, uint32_t *out_len) {
    if (!pkt) return NULL;
    uint16_t id_len = (uint16_t)strlen(pkt->id);
    uint16_t room_len = (uint16_t)strlen(pkt->room);
    uint16_t target_len = (uint16_t)strlen(pkt->target);
    uint32_t data_len = pkt->data_len ? pkt->data_len : 1;
    uint32_t body =
        1 + 1 + 1 + 1 + 4 + 4 + 4 +           // version, priority, flags, type, client_id, timestamp, sequence
        2 + id_len + 2 + room_len + 2 + target_len + 4 + data_len;
    uint32_t total = 4 + body;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;
    uint32_t off = 0;
    memcpy(buf + off, &total, 4); off += 4;
    buf[off++] = ZN_ZNP_VERSION;
    buf[off++] = pkt->priority;
    buf[off++] = pkt->flags;
    buf[off++] = (uint8_t)pkt->type;
    memcpy(buf + off, &pkt->client_id, 4); off += 4;
    memcpy(buf + off, &pkt->timestamp, 4); off += 4;
    memcpy(buf + off, &pkt->sequence, 4); off += 4;
    memcpy(buf + off, &id_len, 2); off += 2;
    memcpy(buf + off, pkt->id, id_len); off += id_len;
    memcpy(buf + off, &room_len, 2); off += 2;
    memcpy(buf + off, pkt->room, room_len); off += room_len;
    memcpy(buf + off, &target_len, 2); off += 2;
    memcpy(buf + off, pkt->target, target_len); off += target_len;
    memcpy(buf + off, &data_len, 4); off += 4;
    if (pkt->data && pkt->data_len > 0) memcpy(buf + off, pkt->data, pkt->data_len);
    else buf[off] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

Packet *zn_znp_deserialize(const uint8_t *raw, uint32_t len) {
    if (!raw || len < 4 + 7 + 2 + 2 + 2 + 4) return NULL;
    uint32_t off = 4; // skip total length
    uint8_t version = raw[off++];
    if (version != ZN_ZNP_VERSION) return NULL;
    Packet *pkt = (Packet *)calloc(1, sizeof(Packet));
    if (!pkt) return NULL;
    pkt->priority = raw[off++];
    pkt->flags = raw[off++];
    pkt->type = (PacketType)raw[off++];
    memcpy(&pkt->client_id, raw + off, 4); off += 4;
    memcpy(&pkt->timestamp, raw + off, 4); off += 4;
    memcpy(&pkt->sequence, raw + off, 4); off += 4;
    uint16_t id_len, room_len, target_len, data_len;
    memcpy(&id_len, raw + off, 2); off += 2;
    if (off + id_len > len) { free(pkt); return NULL; }
    memcpy(pkt->id, raw + off, id_len > ZN_ID_LEN - 1 ? ZN_ID_LEN - 1 : id_len);
    pkt->id[id_len < ZN_ID_LEN ? id_len : ZN_ID_LEN - 1] = '\0';
    off += id_len;
    memcpy(&room_len, raw + off, 2); off += 2;
    if (off + room_len > len) { free(pkt); return NULL; }
    memcpy(pkt->room, raw + off, room_len > ZN_ROOM_NAME_LEN - 1 ? ZN_ROOM_NAME_LEN - 1 : room_len);
    pkt->room[room_len < ZN_ROOM_NAME_LEN ? room_len : ZN_ROOM_NAME_LEN - 1] = '\0';
    off += room_len;
    memcpy(&target_len, raw + off, 2); off += 2;
    if (off + target_len > len) { free(pkt); return NULL; }
    memcpy(pkt->target, raw + off, target_len > ZN_TARGET_LEN - 1 ? ZN_TARGET_LEN - 1 : target_len);
    pkt->target[target_len < ZN_TARGET_LEN ? target_len : ZN_TARGET_LEN - 1] = '\0';
    off += target_len;
    memcpy(&data_len, raw + off, 4); off += 4;
    if (off + data_len > len || data_len > ZN_MAX_PACKET) { free(pkt); return NULL; }
    pkt->data = (char *)malloc(data_len + 1);
    if (pkt->data) {
        memcpy(pkt->data, raw + off, data_len);
        pkt->data[data_len] = '\0';
        pkt->data_len = data_len + 1;
    } else {
        pkt->data = strdup("{}");
        pkt->data_len = 3;
    }
    return pkt;
}

// ===========================================================================
// connection implementation
// ===========================================================================

struct zn_conn {
    int sockfd;
    char id[ZN_ID_LEN];
    char name[ZN_NAME_LEN];
    char addr[64];
    uint16_t port;
    bool alive;
    void *userdata;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint32_t pkts_sent;
    uint32_t pkts_recv;
    uint8_t *read_buf;
    uint32_t read_buf_len;
    uint32_t read_buf_cap;
    // extensions
    uint32_t client_id;
    int team;
    bool spectator;
    TransportType transport;
    bool is_ws;
    struct sockaddr_in udp_peer;
    bool has_udp_peer;
    uint32_t bandwidth_limit;
    uint32_t rtt;
    uint32_t pps_window_start;
    uint32_t pps_count;
    uint64_t bytes_window;
    uint32_t bytes_window_start;
};

Connection *zn_conn_new(int sockfd, const char *addr, uint16_t port) {
    Connection *conn = (Connection *)calloc(1, sizeof(Connection));
    if (!conn) return NULL;
    conn->sockfd = sockfd;
    conn->port = port;
    conn->alive = true;
    conn->userdata = NULL;
    conn->bytes_sent = 0;
    conn->bytes_recv = 0;
    conn->pkts_sent = 0;
    conn->pkts_recv = 0;
    conn->read_buf = NULL;
    conn->read_buf_len = 0;
    conn->read_buf_cap = 0;
    conn->team = -1;
    conn->spectator = false;
    conn->transport = ZN_TCP;
    conn->is_ws = false;
    conn->has_udp_peer = false;
    conn->bandwidth_limit = 0;
    conn->rtt = 0;
    conn->pps_window_start = zn_timestamp();
    conn->pps_count = 0;
    conn->bytes_window = 0;
    conn->bytes_window_start = zn_timestamp();
    if (addr) safe_strcpy(conn->addr, addr, sizeof(conn->addr));
    {
        static int counter = 0;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s:%d-%d", addr ? addr : "?", port, counter++);
        safe_strcpy(conn->id, tmp, sizeof(conn->id));
    }
    safe_strcpy(conn->name, "Player", sizeof(conn->name));
    return conn;
}

void zn_conn_free(Connection *conn) {
    if (conn) {
        zn_conn_close(conn);
        if (conn->read_buf) free(conn->read_buf);
        free(conn);
    }
}

// send a packet using the ZNP binary protocol, with optional bandwidth throttling
static bool conn_send_znp(Connection *conn, Packet *pkt) {
    if (!conn || !conn->alive || !pkt) return false;
    if (conn->bandwidth_limit > 0) {
        uint32_t now = zn_timestamp();
        if (now != conn->bytes_window_start) {
            conn->bytes_window = 0;
            conn->bytes_window_start = now;
        }
        uint32_t len_est = pkt->data_len + 32;
        if (conn->bytes_window + len_est > conn->bandwidth_limit) {
            uint32_t wait = 1000 - (now - conn->bytes_window_start) * 1000;
            if (wait > 0 && wait < 1000) SLEEP_MS(wait);
            conn->bytes_window = 0;
            conn->bytes_window_start = zn_timestamp();
        }
        conn->bytes_window += len_est;
    }
    uint32_t len;
    uint8_t *data = zn_znp_serialize(pkt, &len);
    if (!data) return false;
    int sent = 0;
    while (sent < (int)len) {
        int n = send(conn->sockfd, (const char *)(data + sent), len - sent, 0);
        if (n <= 0) { conn->alive = false; free(data); return false; }
        sent += n;
    }
    conn->bytes_sent += len;
    conn->pkts_sent++;
    free(data);
    return true;
}

bool zn_conn_send(Connection *conn, Packet *pkt) {
    if (!conn || !conn->alive || !pkt) return false;
    if (conn->is_ws) {
        // websocket clients get a ws framed payload
        uint32_t len;
        uint8_t *znp = zn_znp_serialize(pkt, &len);
        if (!znp) return false;
        uint8_t *frame = (uint8_t *)malloc(len + 14);
        if (!frame) { free(znp); return false; }
        uint32_t flen = 0;
        frame[flen++] = 0x82; // FIN + binary
        if (len <= 125) frame[flen++] = (uint8_t)len;
        else if (len <= 65535) { frame[flen++] = 126; frame[flen++] = (len >> 8) & 0xFF; frame[flen++] = len & 0xFF; }
        else { frame[flen++] = 127; for (int i = 7; i >= 0; i--) frame[flen++] = (len >> (i * 8)) & 0xFF; }
        memcpy(frame + flen, znp, len); flen += len;
        int sent = 0;
        while (sent < (int)flen) {
            int n = send(conn->sockfd, (const char *)(frame + sent), flen - sent, 0);
            if (n <= 0) { conn->alive = false; free(znp); free(frame); return false; }
            sent += n;
        }
        free(znp); free(frame);
        conn->bytes_sent += flen; conn->pkts_sent++;
        return true;
    }
    return conn_send_znp(conn, pkt);
}

bool zn_conn_send_data(Connection *conn, const char *json_data) {
    Packet *pkt = zn_packet_create(PKT_DATA, json_data);
    if (!pkt) return false;
    bool result = zn_conn_send(conn, pkt);
    zn_packet_free(pkt);
    return result;
}

bool zn_conn_send_raw(Connection *conn, const uint8_t *data, uint32_t len) {
    if (!conn || !conn->alive || !data) return false;
    int sent = 0;
    while (sent < (int)len) {
        int n = send(conn->sockfd, (const char *)(data + sent), len - sent, 0);
        if (n <= 0) { conn->alive = false; return false; }
        sent += n;
    }
    conn->bytes_sent += len;
    return true;
}

bool zn_conn_send_udp(Connection *conn, Packet *pkt) {
    if (!conn || !pkt || !conn->has_udp_peer) return false;
    uint32_t len;
    uint8_t *data = zn_znp_serialize(pkt, &len);
    if (!data) return false;
    Server *srv = (Server *)conn->userdata; // not used, peer stored on conn
    (void)srv;
    int n = sendto(conn->sockfd, (const char *)data, len, 0,
        (struct sockaddr *)&conn->udp_peer, sizeof(conn->udp_peer));
    free(data);
    if (n <= 0) return false;
    conn->bytes_sent += n;
    conn->pkts_sent++;
    return true;
}

void zn_conn_close(Connection *conn) {
    if (!conn) return;
    conn->alive = false;
    if (conn->sockfd >= 0) {
        CLOSE_SOCKET(conn->sockfd);
        conn->sockfd = -1;
    }
}

bool zn_conn_is_alive(Connection *conn) { return conn && conn->alive; }
void zn_conn_set_name(Connection *conn, const char *name) { if (conn && name) strncpy(conn->name, name, sizeof(conn->name) - 1); }
const char *zn_conn_get_id(Connection *conn) { return conn ? conn->id : ""; }
const char *zn_conn_get_name(Connection *conn) { return conn ? conn->name : ""; }
const char *zn_conn_get_addr(Connection *conn) { return conn ? conn->addr : ""; }
uint16_t zn_conn_get_port(Connection *conn) { return conn ? conn->port : 0; }
void zn_conn_set_userdata(Connection *conn, void *userdata) { if (conn) conn->userdata = userdata; }
void *zn_conn_get_userdata(Connection *conn) { return conn ? conn->userdata : NULL; }
uint64_t zn_conn_bytes_sent(Connection *conn) { return conn ? conn->bytes_sent : 0; }
uint64_t zn_conn_bytes_recv(Connection *conn) { return conn ? conn->bytes_recv : 0; }
uint32_t zn_conn_packets_sent(Connection *conn) { return conn ? conn->pkts_sent : 0; }
uint32_t zn_conn_packets_recv(Connection *conn) { return conn ? conn->pkts_recv : 0; }

// connection extensions
void zn_conn_set_team(Connection *conn, int team) { if (conn) conn->team = team; }
int zn_conn_get_team(Connection *conn) { return conn ? conn->team : -1; }
void zn_conn_set_spectator(Connection *conn, bool spectator) { if (conn) conn->spectator = spectator; }
bool zn_conn_is_spectator(Connection *conn) { return conn ? conn->spectator : false; }
void zn_conn_set_transport(Connection *conn, TransportType t) { if (conn) conn->transport = t; }
TransportType zn_conn_get_transport(Connection *conn) { return conn ? conn->transport : ZN_TCP; }
void zn_conn_set_bandwidth_limit(Connection *conn, uint32_t bytes_per_sec) { if (conn) conn->bandwidth_limit = bytes_per_sec; }
uint32_t zn_conn_bandwidth_limit(Connection *conn) { return conn ? conn->bandwidth_limit : 0; }
uint32_t zn_conn_get_rtt(Connection *conn) { return conn ? conn->rtt : 0; }
void zn_conn_set_rtt(Connection *conn, uint32_t rtt_ms) { if (conn) conn->rtt = rtt_ms; }
uint32_t zn_conn_packets_per_sec(Connection *conn) {
    if (!conn) return 0;
    uint32_t now = zn_timestamp();
    if (now == conn->pps_window_start) return conn->pps_count;
    uint32_t pps = conn->pps_count / (now - conn->pps_window_start > 0 ? now - conn->pps_window_start : 1);
    conn->pps_count = 0;
    conn->pps_window_start = now;
    return pps;
}

// ===========================================================================
// room implementation
// ===========================================================================

struct zn_room {
    char name[ZN_ROOM_NAME_LEN];
    char password[64];
    int max_players;
    bool has_password;
    Connection **connections;
    int conn_count;
    int conn_cap;
};

Room *zn_room_create(const char *name, int max_players) {
    Room *room = (Room *)calloc(1, sizeof(Room));
    if (!room) return NULL;
    if (name) strncpy(room->name, name, sizeof(room->name) - 1);
    room->max_players = max_players > 0 ? max_players : 0;
    room->has_password = false;
    room->password[0] = '\0';
    room->conn_count = 0;
    room->conn_cap = 16;
    room->connections = (Connection **)calloc(room->conn_cap, sizeof(Connection *));
    if (!room->connections) { free(room); return NULL; }
    return room;
}

Room *zn_room_create_with_pw(const char *name, int max_players, const char *password) {
    Room *room = zn_room_create(name, max_players);
    if (room && password) {
        strncpy(room->password, password, sizeof(room->password) - 1);
        room->has_password = true;
    }
    return room;
}

void zn_room_destroy(Room *room) {
    if (room) {
        if (room->connections) free(room->connections);
        free(room);
    }
}

bool zn_room_add(Room *room, Connection *conn) {
    if (!room || !conn) return false;
    if (room->max_players > 0 && room->conn_count >= room->max_players) return false;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i] == conn) return false;
    }
    if (room->conn_count >= room->conn_cap) {
        room->conn_cap *= 2;
        Connection **new_arr = (Connection **)realloc(room->connections, room->conn_cap * sizeof(Connection *));
        if (!new_arr) return false;
        room->connections = new_arr;
    }
    room->connections[room->conn_count++] = conn;
    return true;
}

bool zn_room_remove(Room *room, Connection *conn) {
    if (!room || !conn) return false;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i] == conn) {
            room->connections[i] = room->connections[room->conn_count - 1];
            room->conn_count--;
            return true;
        }
    }
    return false;
}

void zn_room_broadcast(Room *room, Packet *pkt, Connection *exclude) {
    if (!room || !pkt) return;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i] != exclude) zn_conn_send(room->connections[i], pkt);
    }
}

void zn_room_broadcast_data(Room *room, const char *json_data, Connection *exclude) {
    Packet *pkt = zn_packet_create(PKT_DATA, json_data);
    if (pkt) { zn_room_broadcast(room, pkt, exclude); zn_packet_free(pkt); }
}

int zn_room_count(Room *room) { return room ? room->conn_count : 0; }
const char *zn_room_name(Room *room) { return room ? room->name : ""; }
bool zn_room_has_password(Room *room) { return room ? room->has_password : false; }
bool zn_room_check_password(Room *room, const char *password) {
    if (!room || !room->has_password) return true;
    if (!password) return false;
    return strcmp(room->password, password) == 0;
}
void zn_room_set_password(Room *room, const char *password) {
    if (room) {
        if (password && password[0]) {
            strncpy(room->password, password, sizeof(room->password) - 1);
            room->has_password = true;
        } else {
            room->password[0] = '\0';
            room->has_password = false;
        }
    }
}
int zn_room_max_players(Room *room) { return room ? room->max_players : 0; }

int zn_room_team_count(Room *room, int team) {
    if (!room) return 0;
    int c = 0;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i]->team == team) c++;
    }
    return c;
}

int zn_room_spectator_count(Room *room) {
    if (!room) return 0;
    int c = 0;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i]->spectator) c++;
    }
    return c;
}

// ===========================================================================
// server implementation
// ===========================================================================

struct zn_server {
    char host[ZN_HOST_LEN];
    uint16_t port;
    int sockfd;
    bool running;
    Connection *connections[ZN_MAX_CONNECTIONS];
    int conn_count;
    Room *room_list[ZN_MAX_ROOMS];
    int room_count;
    conn_callback on_connect_cb;
    conn_callback on_disconnect_cb;
    data_callback on_data_cb;
    data_callback on_packet_cb;
    void *cb_userdata;
    uint32_t start_time;
    uint64_t packets_recv;
    uint64_t packets_sent;
    // extensions
    int udp_sock;
    uint16_t udp_port;
    bool udp_enabled;
    int ws_sock;
    bool ws_enabled;
    uint16_t ws_port;
    char auth_secret[ZN_TOKEN_LEN];
    bool auth_set;
    uint32_t rl_max_pps;
    uint32_t rl_max_bps;
    uint32_t pps_window_start;
    uint32_t pps_count;
    int log_level;
};

typedef struct {
    struct zn_server *srv;
    Connection *conn;
} conn_thread_arg;

// forward decls
static void server_handle_packet(Server *srv, Connection *conn, Packet *pkt);
static Connection *server_find_conn_by_clientid(Server *srv, uint32_t cid);

#ifdef _WIN32
static DWORD WINAPI conn_read_thread(LPVOID arg) {
#else
static void *conn_read_thread(void *arg) {
#endif
    conn_thread_arg *cta = (conn_thread_arg *)arg;
    Server *srv = cta->srv;
    Connection *conn = cta->conn;
    free(cta);

    uint8_t buf[ZN_MAX_PACKET];
    while (conn->alive && srv->running) {
        int n = recv(conn->sockfd, (char *)buf, sizeof(buf), 0);
        if (n < 0) {
            if (recv_would_block()) { SLEEP_MS(5); continue; }
            zn_log("[-] %s disconnected\n", conn->id);
            if (srv->on_disconnect_cb) srv->on_disconnect_cb(conn, srv->cb_userdata);
            for (int i = 0; i < srv->conn_count; i++) {
                if (srv->connections[i] == conn) {
                    srv->connections[i] = srv->connections[srv->conn_count - 1];
                    srv->conn_count--;
                    break;
                }
            }
            for (int r = 0; r < srv->room_count; r++) zn_room_remove(srv->room_list[r], conn);
            zn_conn_free(conn);
            break;
        }
        if (n == 0) {
            zn_log("[-] %s disconnected\n", conn->id);
            if (srv->on_disconnect_cb) srv->on_disconnect_cb(conn, srv->cb_userdata);
            for (int i = 0; i < srv->conn_count; i++) {
                if (srv->connections[i] == conn) {
                    srv->connections[i] = srv->connections[srv->conn_count - 1];
                    srv->conn_count--;
                    break;
                }
            }
            for (int r = 0; r < srv->room_count; r++) zn_room_remove(srv->room_list[r], conn);
            zn_conn_free(conn);
            break;
        }
        conn->bytes_recv += n;
        uint32_t offset = 0;
        while ((uint32_t)n - offset >= 4) {
            uint32_t msg_len;
            memcpy(&msg_len, buf + offset, 4);
            if (msg_len > ZN_MAX_PACKET || msg_len == 0) break;
            if (offset + msg_len > (uint32_t)n) break;
            Packet *pkt = zn_znp_deserialize(buf + offset, msg_len);
            if (pkt) {
                conn->pkts_recv++;
                srv->packets_recv++;
                conn->pps_count++;
                server_handle_packet(srv, conn, pkt);
                zn_packet_free(pkt);
            }
            offset += msg_len;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

// websocket read thread, decodes ws frames and dispatches as ZNP
#ifdef _WIN32
static DWORD WINAPI ws_read_thread(LPVOID arg) {
#else
static void *ws_read_thread(void *arg) {
#endif
    conn_thread_arg *cta = (conn_thread_arg *)arg;
    Server *srv = cta->srv;
    Connection *conn = cta->conn;
    free(cta);
    uint8_t buf[ZN_MAX_PACKET];
    uint8_t frag[ZN_MAX_PACKET];
    uint32_t frag_len = 0;
    while (conn->alive && srv->running) {
        int n = recv(conn->sockfd, (char *)buf, sizeof(buf), 0);
        if (n <= 0) {
            if (recv_would_block()) { SLEEP_MS(5); continue; }
            if (srv->on_disconnect_cb) srv->on_disconnect_cb(conn, srv->cb_userdata);
            for (int i = 0; i < srv->conn_count; i++) {
                if (srv->connections[i] == conn) { srv->connections[i] = srv->connections[srv->conn_count - 1]; srv->conn_count--; break; }
            }
            zn_conn_free(conn);
            break;
        }
        uint32_t off = 0;
        while (off + 2 <= (uint32_t)n) {
            uint8_t b0 = buf[off], b1 = buf[off + 1];
            uint8_t opcode = b0 & 0x0F;
            bool masked = (b1 & 0x80) != 0;
            uint64_t plen = b1 & 0x7F;
            uint32_t p = off + 2;
            if (plen == 126) { if (p + 2 > (uint32_t)n) break; plen = (buf[p] << 8) | buf[p + 1]; p += 2; }
            else if (plen == 127) { if (p + 8 > (uint32_t)n) break; plen = 0; for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[p + i]; p += 8; }
            uint8_t mask[4] = {0,0,0,0};
            if (masked) { if (p + 4 > (uint32_t)n) break; memcpy(mask, buf + p, 4); p += 4; }
            if (p + plen > (uint32_t)n) break;
            uint8_t *payload = buf + p;
            if (masked) for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i % 4];
            if (opcode == 0x2 || opcode == 0x0) {
                if (frag_len + plen < ZN_MAX_PACKET) { memcpy(frag + frag_len, payload, plen); frag_len += (uint32_t)plen; }
            } else if (opcode == 0x8) {
                zn_conn_close(conn); break;
            }
            if (opcode == 0x2 || opcode == 0x0) {
                // try to parse a full znp frame from frag
                if (frag_len >= 4) {
                    uint32_t mlen; memcpy(&mlen, frag, 4);
                    if (mlen <= frag_len && mlen > 0) {
                        Packet *pkt = zn_znp_deserialize(frag, mlen);
                        if (pkt) {
                            conn->pkts_recv++; srv->packets_recv++;
                            server_handle_packet(srv, conn, pkt);
                            zn_packet_free(pkt);
                        }
                        memmove(frag, frag + mlen, frag_len - mlen);
                        frag_len -= mlen;
                    }
                }
            }
            off = p + (uint32_t)plen;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

// udp receive thread for the server
#ifdef _WIN32
static DWORD WINAPI udp_recv_thread(LPVOID arg) {
#else
static void *udp_recv_thread(void *arg) {
#endif
    Server *srv = (Server *)arg;
    uint8_t buf[ZN_MAX_PACKET];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    while (srv->running && srv->udp_sock >= 0) {
        int n = recvfrom(srv->udp_sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) { SLEEP_MS(5); continue; }
        Packet *pkt = zn_znp_deserialize(buf, (uint32_t)n);
        if (!pkt) continue;
        Connection *conn = server_find_conn_by_clientid(srv, pkt->client_id);
        if (conn) {
            conn->udp_peer = from;
            conn->has_udp_peer = true;
            conn->pkts_recv++;
            srv->packets_recv++;
            server_handle_packet(srv, conn, pkt);
        }
        zn_packet_free(pkt);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

// server ping thread, measures rtt by pinging clients
#ifdef _WIN32
static DWORD WINAPI server_ping_thread(LPVOID arg) {
#else
static void *server_ping_thread(void *arg) {
#endif
    Server *srv = (Server *)arg;
    while (srv->running) {
        SLEEP_MS(5000);
        if (!srv->running) break;
        uint32_t now = zn_timestamp();
        for (int i = 0; i < srv->conn_count; i++) {
            Connection *conn = srv->connections[i];
            if (!conn->alive) continue;
            Packet *ping = zn_packet_create(PKT_PING, "{}");
            if (ping) {
                zn_packet_set_timestamp(ping, now);
                zn_packet_set_client_id(ping, conn->client_id);
                zn_conn_send(conn, ping);
                zn_packet_free(ping);
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void server_handle_packet(Server *srv, Connection *conn, Packet *pkt) {
    if (pkt->type == PKT_PING) {
        Packet pong;
        memset(&pong, 0, sizeof(Packet));
        pong.type = PKT_PONG;
        pong.data = strdup("{}");
        pong.data_len = 3;
        gen_id(pong.id, sizeof(pong.id));
        pong.timestamp = pkt->timestamp; // echo client timestamp so client can compute rtt
        pong.client_id = conn->client_id;
        zn_conn_send(conn, &pong);
        free(pong.data);
        return;
    }
    if (pkt->type == PKT_PONG) {
        uint32_t now = zn_timestamp();
        if (now >= pkt->timestamp) zn_conn_set_rtt(conn, (now - pkt->timestamp) * 1000);
        return;
    }
    if (pkt->type == PKT_JOIN_ROOM) {
        char *room_name = json_strval(pkt->data, "room");
        if (room_name) {
            Room *room = zn_server_find_room(srv, room_name);
            if (!room) {
                room = zn_room_create(room_name, 0);
                if (room && srv->room_count < ZN_MAX_ROOMS) srv->room_list[srv->room_count++] = room;
            }
            if (room) {
                if (zn_room_add(room, conn)) zn_log("[+] %s joined room %s\n", conn->id, room_name);
            }
            free(room_name);
        }
        return;
    }
    if (pkt->type == PKT_LEAVE_ROOM) {
        for (int r = 0; r < srv->room_count; r++) {
            if (zn_room_remove(srv->room_list[r], conn)) {
                zn_log("[-] %s left room %s\n", conn->id, srv->room_list[r]->name);
                break;
            }
        }
        return;
    }
    if (pkt->type == PKT_AUTH) {
        if (srv->auth_set) {
            bool ok = zn_auth_token_verify(srv->auth_secret, pkt->data, conn->id);
            if (!ok) zn_log("[!] auth failed for %s\n", conn->id);
        }
        return;
    }
    if (srv->on_packet_cb) srv->on_packet_cb(conn, pkt, srv->cb_userdata);
    if (srv->on_data_cb) srv->on_data_cb(conn, pkt, srv->cb_userdata);
}

Server *zn_server_create(uint16_t port) { return zn_server_create_host("0.0.0.0", port); }

Server *zn_server_create_host(const char *host, uint16_t port) {
    ensure_winsock();
    Server *srv = (Server *)calloc(1, sizeof(Server));
    if (!srv) return NULL;
    if (host) strncpy(srv->host, host, sizeof(srv->host) - 1);
    srv->port = port;
    srv->sockfd = -1;
    srv->running = false;
    srv->conn_count = 0;
    srv->room_count = 0;
    srv->packets_recv = 0;
    srv->packets_sent = 0;
    srv->udp_sock = -1;
    srv->udp_enabled = false;
    srv->ws_sock = -1;
    srv->ws_enabled = false;
    srv->auth_set = false;
    srv->rl_max_pps = 0;
    srv->rl_max_bps = 0;
    srv->pps_window_start = zn_timestamp();
    srv->pps_count = 0;
    srv->log_level = 0;
    return srv;
}

void zn_server_destroy(Server *srv) {
    if (!srv) return;
    zn_server_stop(srv);
    for (int i = 0; i < srv->conn_count; i++) zn_conn_free(srv->connections[i]);
    for (int i = 0; i < srv->room_count; i++) zn_room_destroy(srv->room_list[i]);
    if (srv->udp_sock >= 0) CLOSE_SOCKET(srv->udp_sock);
    if (srv->ws_sock >= 0) CLOSE_SOCKET(srv->ws_sock);
    free(srv);
}

static void ws_handshake(int fd) {
    char buf[4096];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    char *key = strstr(buf, "Sec-WebSocket-Key:");
    if (!key) return;
    key += 18;
    while (*key == ' ') key++;
    char *end = strchr(key, '\r');
    if (end) *end = '\0';
    // magic guid concat + sha1 would go here, we use a simple hash stand-in
    uint32_t h = zn_hash32((const uint8_t *)key, (uint32_t)strlen(key));
    char accept[64];
    snprintf(accept, sizeof(accept),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %08x\r\n\r\n", h);
    send(fd, accept, (int)strlen(accept), 0);
}

#ifdef _WIN32
static DWORD WINAPI ws_accept_thread(LPVOID arg) {
#else
static void *ws_accept_thread(void *arg) {
#endif
    Server *srv = (Server *)arg;
    while (srv->running && srv->ws_sock >= 0) {
        struct sockaddr_in ca;
        socklen_t calen = sizeof(ca);
        int fd = (int)accept(srv->ws_sock, (struct sockaddr *)&ca, &calen);
        if (fd < 0) { SLEEP_MS(50); continue; }
        if (srv->conn_count >= ZN_MAX_CONNECTIONS) { CLOSE_SOCKET(fd); continue; }
        ws_handshake(fd);
        char addr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ca.sin_addr, addr, sizeof(addr));
        Connection *conn = zn_conn_new(fd, addr, ntohs(ca.sin_port));
        conn->is_ws = true;
        conn->transport = ZN_WEBSOCKET;
        srv->connections[srv->conn_count++] = conn;
        zn_log("[+] ws client %s connected\n", conn->id);
        if (srv->on_connect_cb) srv->on_connect_cb(conn, srv->cb_userdata);
        conn_thread_arg *cta = (conn_thread_arg *)malloc(sizeof(conn_thread_arg));
        if (cta) {
            cta->srv = srv; cta->conn = conn;
#ifdef _WIN32
            HANDLE h = CreateThread(NULL, 0, ws_read_thread, cta, 0, NULL); if (h) CloseHandle(h);
#else
            pthread_t tid; pthread_create(&tid, NULL, ws_read_thread, cta); pthread_detach(tid);
#endif
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI accept_thread(LPVOID arg) {
#else
static void *accept_thread(void *arg) {
#endif
    Server *srv = (Server *)arg;
    fd_set readfds;
    struct timeval tv;
    while (srv->running) {
        FD_ZERO(&readfds);
        FD_SET(srv->sockfd, &readfds);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        if (select(srv->sockfd + 1, &readfds, NULL, NULL, &tv) < 0) break;
        if (!FD_ISSET(srv->sockfd, &readfds)) continue;
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = (int)accept(srv->sockfd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;
        if (srv->conn_count >= ZN_MAX_CONNECTIONS) { CLOSE_SOCKET(client_fd); continue; }
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        set_nonblock(client_fd);
        Connection *conn = zn_conn_new(client_fd, addr_str, ntohs(client_addr.sin_port));
        srv->connections[srv->conn_count++] = conn;
        zn_log("[+] %s connected\n", conn->id);
        if (srv->on_connect_cb) srv->on_connect_cb(conn, srv->cb_userdata);
        conn_thread_arg *cta = (conn_thread_arg *)malloc(sizeof(conn_thread_arg));
        if (cta) {
            cta->srv = srv; cta->conn = conn;
#ifdef _WIN32
            HANDLE hThread = CreateThread(NULL, 0, conn_read_thread, cta, 0, NULL);
            if (hThread) CloseHandle(hThread);
#else
            pthread_t tid; pthread_create(&tid, NULL, conn_read_thread, cta); pthread_detach(tid);
#endif
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

bool zn_server_start(Server *srv) {
    if (!srv || srv->running) return false;
    srv->sockfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (srv->sockfd < 0) { zn_log("[!] socket failed\n"); return false; }
    int opt = 1;
    setsockopt(srv->sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(srv->port);
    addr.sin_addr.s_addr = inet_addr(srv->host);
    if (bind(srv->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zn_log("[!] bind failed on %s:%d\n", srv->host, srv->port);
        CLOSE_SOCKET(srv->sockfd); return false;
    }
    listen(srv->sockfd, 128);
    srv->running = true;
    srv->start_time = (uint32_t)time(NULL);
    zn_log("[*] server on %s:%d\n", srv->host, srv->port);
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, accept_thread, srv, 0, NULL); if (h) CloseHandle(h);
    HANDLE hp = CreateThread(NULL, 0, server_ping_thread, srv, 0, NULL); if (hp) CloseHandle(hp);
#else
    pthread_t tid; pthread_create(&tid, NULL, accept_thread, srv); pthread_detach(tid);
    pthread_create(&tid, NULL, server_ping_thread, srv); pthread_detach(tid);
#endif
    if (srv->udp_enabled) {
        srv->udp_sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (srv->udp_sock >= 0) {
            struct sockaddr_in ua;
            memset(&ua, 0, sizeof(ua));
            ua.sin_family = AF_INET;
            ua.sin_port = htons(srv->udp_port);
            ua.sin_addr.s_addr = inet_addr(srv->host);
            if (bind(srv->udp_sock, (struct sockaddr *)&ua, sizeof(ua)) == 0) {
                zn_log("[*] udp on %s:%d\n", srv->host, srv->udp_port);
#ifdef _WIN32
                HANDLE hu = CreateThread(NULL, 0, udp_recv_thread, srv, 0, NULL); if (hu) CloseHandle(hu);
#else
                pthread_t ut; pthread_create(&ut, NULL, udp_recv_thread, srv); pthread_detach(ut);
#endif
            } else { CLOSE_SOCKET(srv->udp_sock); srv->udp_sock = -1; }
        }
    }
    if (srv->ws_enabled) {
        srv->ws_sock = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (srv->ws_sock >= 0) {
            struct sockaddr_in wa;
            memset(&wa, 0, sizeof(wa));
            wa.sin_family = AF_INET;
            wa.sin_port = htons(srv->ws_port);
            wa.sin_addr.s_addr = inet_addr(srv->host);
            if (bind(srv->ws_sock, (struct sockaddr *)&wa, sizeof(wa)) == 0 && listen(srv->ws_sock, 32) == 0) {
                zn_log("[*] websocket on %s:%d\n", srv->host, srv->ws_port);
#ifdef _WIN32
                HANDLE hw = CreateThread(NULL, 0, ws_accept_thread, srv, 0, NULL); if (hw) CloseHandle(hw);
#else
                pthread_t wt; pthread_create(&wt, NULL, ws_accept_thread, srv); pthread_detach(wt);
#endif
            } else { CLOSE_SOCKET(srv->ws_sock); srv->ws_sock = -1; }
        }
    }
    return true;
}

void zn_server_stop(Server *srv) {
    if (!srv) return;
    srv->running = false;
    for (int i = 0; i < srv->conn_count; i++) zn_conn_close(srv->connections[i]);
    if (srv->sockfd >= 0) { CLOSE_SOCKET(srv->sockfd); srv->sockfd = -1; }
    if (srv->udp_sock >= 0) { CLOSE_SOCKET(srv->udp_sock); srv->udp_sock = -1; }
    if (srv->ws_sock >= 0) { CLOSE_SOCKET(srv->ws_sock); srv->ws_sock = -1; }
    zn_log("[*] server stopped\n");
}

void zn_server_broadcast(Server *srv, Packet *pkt, Connection *exclude) {
    if (!srv || !pkt) return;
    for (int i = 0; i < srv->conn_count; i++) {
        if (srv->connections[i] != exclude) {
            if (zn_conn_send(srv->connections[i], pkt)) srv->packets_sent++;
        }
    }
}

void zn_server_broadcast_data(Server *srv, const char *data, Connection *exclude) {
    Packet *pkt = zn_packet_create(PKT_DATA, data);
    if (pkt) { zn_server_broadcast(srv, pkt, exclude); zn_packet_free(pkt); }
}

Connection *zn_server_get_conn(Server *srv, const char *id) {
    if (!srv || !id) return NULL;
    for (int i = 0; i < srv->conn_count; i++) {
        if (strcmp(srv->connections[i]->id, id) == 0) return srv->connections[i];
    }
    return NULL;
}

static Connection *server_find_conn_by_clientid(Server *srv, uint32_t cid) {
    if (!srv || cid == 0) return NULL;
    for (int i = 0; i < srv->conn_count; i++) {
        if (srv->connections[i]->client_id == cid) return srv->connections[i];
    }
    return NULL;
}

int zn_server_conn_count(Server *srv) { return srv ? srv->conn_count : 0; }
uint64_t zn_server_packets_recv(Server *srv) { return srv ? srv->packets_recv : 0; }
uint64_t zn_server_packets_sent(Server *srv) { return srv ? srv->packets_sent : 0; }
uint32_t zn_server_uptime(Server *srv) { return srv ? (uint32_t)(time(NULL) - srv->start_time) : 0; }

void zn_server_on_connect(Server *srv, conn_callback cb, void *userdata) { if (srv) { srv->on_connect_cb = cb; srv->cb_userdata = userdata; } }
void zn_server_on_disconnect(Server *srv, conn_callback cb, void *userdata) { if (srv) srv->on_disconnect_cb = cb; }
void zn_server_on_data(Server *srv, data_callback cb, void *userdata) { if (srv) srv->on_data_cb = cb; }
void zn_server_on_packet(Server *srv, data_callback cb, void *userdata) { if (srv) srv->on_packet_cb = cb; }

Room *zn_server_find_room(Server *srv, const char *name) {
    if (!srv || !name) return NULL;
    for (int i = 0; i < srv->room_count; i++) {
        if (strcmp(srv->room_list[i]->name, name) == 0) return srv->room_list[i];
    }
    return NULL;
}

Room *zn_server_create_room(Server *srv, const char *name, int max_players) {
    if (!srv || !name) return NULL;
    Room *existing = zn_server_find_room(srv, name);
    if (existing) return existing;
    if (srv->room_count >= ZN_MAX_ROOMS) return NULL;
    Room *room = zn_room_create(name, max_players);
    if (room) srv->room_list[srv->room_count++] = room;
    return room;
}

bool zn_server_destroy_room(Server *srv, const char *name) {
    if (!srv || !name) return false;
    for (int i = 0; i < srv->room_count; i++) {
        if (strcmp(srv->room_list[i]->name, name) == 0) {
            zn_room_destroy(srv->room_list[i]);
            srv->room_list[i] = srv->room_list[srv->room_count - 1];
            srv->room_count--;
            return true;
        }
    }
    return false;
}

int zn_server_room_count(Server *srv) { return srv ? srv->room_count : 0; }

// server transport extensions
bool zn_server_enable_udp(Server *srv, uint16_t udp_port) {
    if (!srv) return false;
    srv->udp_port = udp_port;
    srv->udp_enabled = true;
    return true;
}
bool zn_server_enable_websocket(Server *srv, bool enable) {
    if (!srv) return false;
    srv->ws_enabled = enable;
    srv->ws_port = (uint16_t)(srv->port + 1);
    return true;
}
bool zn_server_websocket_enabled(Server *srv) { return srv ? srv->ws_enabled : false; }

// server security extensions
void zn_server_set_rate_limit(Server *srv, uint32_t max_packets_per_sec, uint32_t max_bytes_per_sec) {
    if (srv) { srv->rl_max_pps = max_packets_per_sec; srv->rl_max_bps = max_bytes_per_sec; }
}
void zn_server_set_auth_secret(Server *srv, const char *secret) {
    if (srv && secret) { strncpy(srv->auth_secret, secret, sizeof(srv->auth_secret) - 1); srv->auth_set = true; }
}
bool zn_server_verify_token(Server *srv, const char *token) {
    if (!srv || !srv->auth_set || !token) return false;
    return zn_auth_token_verify(srv->auth_secret, token, "server");
}

// server dev tool extensions
uint32_t zn_server_avg_ping(Server *srv) {
    if (!srv || srv->conn_count == 0) return 0;
    uint64_t sum = 0; int c = 0;
    for (int i = 0; i < srv->conn_count; i++) { if (srv->connections[i]->rtt > 0) { sum += srv->connections[i]->rtt; c++; } }
    return c > 0 ? (uint32_t)(sum / c) : 0;
}
uint32_t zn_server_packets_per_sec(Server *srv) {
    if (!srv) return 0;
    uint32_t now = zn_timestamp();
    if (now == srv->pps_window_start) return srv->pps_count;
    uint32_t pps = srv->pps_count / (now - srv->pps_window_start > 0 ? now - srv->pps_window_start : 1);
    srv->pps_count = 0; srv->pps_window_start = now;
    return pps;
}
void zn_server_set_log_level(Server *srv, int level) { if (srv) srv->log_level = level; }

// ===========================================================================
// client implementation
// ===========================================================================

struct zn_client {
    int sockfd;
    bool connected;
    bool shutdown_flag;
    bool auto_reconnect;
    uint32_t reconnect_ms;
    char host[ZN_HOST_LEN];
    uint16_t port;
    char name[ZN_NAME_LEN];
    char id[ZN_ID_LEN];
    client_callback on_connect_cb;
    client_callback on_disconnect_cb;
    data_callback on_data_cb;
    packet_callback on_message_cb;
    void *cb_userdata;
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint8_t *buffer;
    uint32_t buf_len;
    uint32_t buf_cap;
    // extensions
    int udp_sock;
    struct sockaddr_in server_addr;
    bool udp_enabled;
    char token[ZN_TOKEN_LEN];
    uint32_t rtt;
    uint32_t last_ping_time;
    char region[32];
};

#ifdef _WIN32
static DWORD WINAPI client_udp_thread(LPVOID arg) {
#else
static void *client_udp_thread(void *arg) {
#endif
    Client *cli = (Client *)arg;
    uint8_t buf[ZN_MAX_PACKET];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    while (cli->connected && cli->udp_sock >= 0) {
        int n = recvfrom(cli->udp_sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) { SLEEP_MS(5); continue; }
        Packet *pkt = zn_znp_deserialize(buf, (uint32_t)n);
        if (pkt) {
            cli->packets_recv++;
            if (pkt->type == PKT_PING) {
                Packet pong; memset(&pong, 0, sizeof(Packet));
                pong.type = PKT_PONG; pong.data = strdup("{}"); pong.data_len = 3;
                gen_id(pong.id, sizeof(pong.id)); pong.timestamp = pkt->timestamp;
                zn_client_send(cli, &pong); free(pong.data);
            } else if (pkt->type == PKT_PONG) {
                uint32_t now = zn_timestamp();
                if (now >= pkt->timestamp) cli->rtt = (now - pkt->timestamp) * 1000;
            } else if (pkt->type == PKT_DATA) {
                if (cli->on_message_cb) cli->on_message_cb(pkt, cli->cb_userdata);
                if (cli->on_data_cb) cli->on_data_cb(NULL, pkt, cli->cb_userdata);
            }
            zn_packet_free(pkt);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI client_read_thread(LPVOID arg) {
#else
static void *client_read_thread(void *arg) {
#endif
    Client *cli = (Client *)arg;
    uint8_t buf[ZN_MAX_PACKET];
    while (true) {
        if (!cli->connected) {
            if (cli->shutdown_flag) break;
            if (cli->auto_reconnect) {
                SLEEP_MS(cli->reconnect_ms);
                if (cli->shutdown_flag) break;
                // re-establish socket
                cli->sockfd = (int)socket(AF_INET, SOCK_STREAM, 0);
                if (cli->sockfd < 0) continue;
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(cli->port);
                addr.sin_addr.s_addr = inet_addr(cli->host);
                if (connect(cli->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                    CLOSE_SOCKET(cli->sockfd); cli->sockfd = -1; continue;
                }
                set_nonblock(cli->sockfd);
                cli->connected = true;
                zn_log("[*] reconnected to %s:%d\n", cli->host, cli->port);
                if (cli->on_connect_cb) cli->on_connect_cb(cli, cli->cb_userdata);
                continue;
            } else break;
        }
        int n = recv(cli->sockfd, (char *)buf, sizeof(buf), 0);
        if (n <= 0) {
            cli->connected = false;
            if (cli->on_disconnect_cb) cli->on_disconnect_cb(cli, cli->cb_userdata);
            if (cli->shutdown_flag) break;
            if (!cli->auto_reconnect) break;
            continue;
        }
        cli->packets_recv++;
        uint32_t offset = 0;
        while ((uint32_t)n - offset >= 4) {
            uint32_t msg_len;
            memcpy(&msg_len, buf + offset, 4);
            if (msg_len > ZN_MAX_PACKET || msg_len == 0) break;
            if (offset + msg_len > (uint32_t)n) break;
            Packet *pkt = zn_znp_deserialize(buf + offset, msg_len);
            if (pkt) {
                if (pkt->type == PKT_PING) {
                    Packet pong; memset(&pong, 0, sizeof(Packet));
                    pong.type = PKT_PONG; pong.data = strdup("{}"); pong.data_len = 3;
                    gen_id(pong.id, sizeof(pong.id)); pong.timestamp = pkt->timestamp;
                    zn_client_send(cli, &pong); free(pong.data);
                } else if (pkt->type == PKT_PONG) {
                    uint32_t now = zn_timestamp();
                    if (now >= pkt->timestamp) cli->rtt = (now - pkt->timestamp) * 1000;
                } else if (pkt->type == PKT_DATA) {
                    if (cli->on_message_cb) cli->on_message_cb(pkt, cli->cb_userdata);
                    if (cli->on_data_cb) cli->on_data_cb(NULL, pkt, cli->cb_userdata);
                }
                zn_packet_free(pkt);
            }
            offset += msg_len;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI client_ping_thread(LPVOID arg) {
#else
static void *client_ping_thread(void *arg) {
#endif
    Client *cli = (Client *)arg;
    while (cli->connected) {
        SLEEP_MS(5000);
        if (cli->connected) {
            Packet *ping = zn_packet_create(PKT_PING, "{}");
            if (ping) {
                uint32_t now = zn_timestamp();
                zn_packet_set_timestamp(ping, now);
                cli->last_ping_time = now;
                zn_client_send(cli, ping);
                zn_packet_free(ping);
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

Client *zn_client_new(void) {
    ensure_winsock();
    Client *cli = (Client *)calloc(1, sizeof(Client));
    if (!cli) return NULL;
    cli->sockfd = -1;
    cli->connected = false;
    cli->shutdown_flag = false;
    cli->auto_reconnect = false;
    cli->reconnect_ms = 2000;
    cli->buf_cap = ZN_BUF_SIZE;
    cli->buffer = (uint8_t *)malloc(ZN_BUF_SIZE);
    cli->buf_len = 0;
    cli->packets_sent = 0;
    cli->packets_recv = 0;
    cli->udp_sock = -1;
    cli->udp_enabled = false;
    cli->rtt = 0;
    cli->last_ping_time = 0;
    strcpy(cli->name, "Player");
    gen_id(cli->id, sizeof(cli->id));
    cli->region[0] = '\0';
    return cli;
}

void zn_client_free(Client *cli) {
    if (cli) {
        zn_client_disconnect(cli);
        if (cli->buffer) free(cli->buffer);
        if (cli->udp_sock >= 0) CLOSE_SOCKET(cli->udp_sock);
        free(cli);
    }
}

bool zn_client_connect(Client *cli, const char *host, uint16_t port) {
    return zn_client_connect_name(cli, host, port, "Player");
}

bool zn_client_connect_name(Client *cli, const char *host, uint16_t port, const char *name) {
    if (!cli || cli->connected) return false;
    if (name) strncpy(cli->name, name, sizeof(cli->name) - 1);
    if (host) strncpy(cli->host, host, sizeof(cli->host) - 1);
    cli->port = port;
    cli->sockfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (cli->sockfd < 0) return false;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(cli->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zn_log("[!] connect failed\n");
        CLOSE_SOCKET(cli->sockfd); cli->sockfd = -1; return false;
    }
    set_nonblock(cli->sockfd);
    cli->connected = true;
    zn_log("[*] connected to %s:%d\n", host, port);
    if (cli->on_connect_cb) cli->on_connect_cb(cli, cli->cb_userdata);
#ifdef _WIN32
    HANDLE h1 = CreateThread(NULL, 0, client_read_thread, cli, 0, NULL); if (h1) CloseHandle(h1);
    HANDLE h2 = CreateThread(NULL, 0, client_ping_thread, cli, 0, NULL); if (h2) CloseHandle(h2);
    if (cli->udp_enabled) { HANDLE h3 = CreateThread(NULL, 0, client_udp_thread, cli, 0, NULL); if (h3) CloseHandle(h3); }
#else
    pthread_t tid;
    pthread_create(&tid, NULL, client_read_thread, cli); pthread_detach(tid);
    pthread_create(&tid, NULL, client_ping_thread, cli); pthread_detach(tid);
    if (cli->udp_enabled) { pthread_create(&tid, NULL, client_udp_thread, cli); pthread_detach(tid); }
#endif
    return true;
}

void zn_client_disconnect(Client *cli) {
    if (!cli) return;
    cli->shutdown_flag = true;
    cli->connected = false;
    if (cli->sockfd >= 0) { CLOSE_SOCKET(cli->sockfd); cli->sockfd = -1; }
    if (cli->on_disconnect_cb) cli->on_disconnect_cb(cli, cli->cb_userdata);
    zn_log("[*] client disconnected\n");
}

bool zn_client_send(Client *cli, Packet *pkt) {
    if (!cli || !cli->connected || !pkt) return false;
    uint32_t len;
    uint8_t *data = zn_znp_serialize(pkt, &len);
    if (!data) return false;
    int sent = 0;
    while (sent < (int)len) {
        int n = send(cli->sockfd, (const char *)(data + sent), len - sent, 0);
        if (n <= 0) { free(data); return false; }
        sent += n;
    }
    cli->packets_sent++;
    free(data);
    return true;
}

bool zn_client_send_data(Client *cli, const char *json_data) {
    Packet *pkt = zn_packet_create(PKT_DATA, json_data);
    if (!pkt) return false;
    bool result = zn_client_send(cli, pkt);
    zn_packet_free(pkt);
    return result;
}

bool zn_client_join_room(Client *cli, const char *room_name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"room\":\"%s\"}", room_name);
    Packet *pkt = zn_packet_create(PKT_JOIN_ROOM, buf);
    if (!pkt) return false;
    bool result = zn_client_send(cli, pkt);
    zn_packet_free(pkt);
    return result;
}

bool zn_client_leave_room(Client *cli) {
    Packet *pkt = zn_packet_create(PKT_LEAVE_ROOM, "{}");
    if (!pkt) return false;
    bool result = zn_client_send(cli, pkt);
    zn_packet_free(pkt);
    return result;
}

bool zn_client_is_connected(Client *cli) { return cli && cli->connected; }
const char *zn_client_get_id(Client *cli) { return cli ? cli->id : ""; }
const char *zn_client_get_name(Client *cli) { return cli ? cli->name : ""; }
uint64_t zn_client_packets_sent(Client *cli) { return cli ? cli->packets_sent : 0; }
uint64_t zn_client_packets_recv(Client *cli) { return cli ? cli->packets_recv : 0; }

void zn_client_on_connect(Client *cli, client_callback cb, void *userdata) { if (cli) { cli->on_connect_cb = cb; cli->cb_userdata = userdata; } }
void zn_client_on_disconnect(Client *cli, client_callback cb, void *userdata) { if (cli) cli->on_disconnect_cb = cb; }
void zn_client_on_data(Client *cli, data_callback cb, void *userdata) { if (cli) cli->on_data_cb = cb; }
void zn_client_on_message(Client *cli, packet_callback cb, void *userdata) { if (cli) cli->on_message_cb = cb; }

// client extensions
void zn_client_set_auto_reconnect(Client *cli, bool enable, uint32_t retry_ms) {
    if (cli) { cli->auto_reconnect = enable; cli->reconnect_ms = retry_ms > 0 ? retry_ms : 2000; }
}
bool zn_client_auto_reconnect_enabled(Client *cli) { return cli ? cli->auto_reconnect : false; }
bool zn_client_enable_udp(Client *cli, uint16_t udp_port) {
    if (!cli) return false;
    cli->udp_sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (cli->udp_sock < 0) return false;
    memset(&cli->server_addr, 0, sizeof(cli->server_addr));
    cli->server_addr.sin_family = AF_INET;
    cli->server_addr.sin_port = htons(udp_port);
    cli->server_addr.sin_addr.s_addr = inet_addr(cli->host);
    cli->udp_enabled = true;
    return true;
}
void zn_client_set_token(Client *cli, const char *token) { if (cli && token) strncpy(cli->token, token, sizeof(cli->token) - 1); }
const char *zn_client_get_token(Client *cli) { return cli ? cli->token : ""; }
uint32_t zn_client_get_rtt(Client *cli) { return cli ? cli->rtt : 0; }
void zn_client_set_region(Client *cli, const char *region) { if (cli && region) strncpy(cli->region, region, sizeof(cli->region) - 1); }
const char *zn_client_get_region(Client *cli) { return cli ? cli->region : ""; }

// ===========================================================================
// security module
// ===========================================================================

struct zn_cipher {
    uint8_t key[256];
    uint32_t key_len;
    uint32_t pos;
};

zn_Cipher *zn_cipher_create(const uint8_t *key, uint32_t key_len) {
    zn_Cipher *c = (zn_Cipher *)calloc(1, sizeof(zn_Cipher));
    if (!c || !key || key_len == 0) { free(c); return NULL; }
    c->key_len = key_len < 256 ? key_len : 256;
    memcpy(c->key, key, c->key_len);
    c->pos = 0;
    return c;
}
void zn_cipher_free(zn_Cipher *c) { free(c); }
void zn_cipher_encrypt(zn_Cipher *c, uint8_t *data, uint32_t len) {
    if (!c || !data) return;
    for (uint32_t i = 0; i < len; i++) {
        data[i] ^= c->key[c->pos % c->key_len];
        c->pos++;
    }
}
void zn_cipher_decrypt(zn_Cipher *c, uint8_t *data, uint32_t len) {
    // xor cipher is symmetric
    zn_cipher_encrypt(c, data, len);
}

uint32_t zn_hash32(const uint8_t *data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

char *zn_auth_token_create(const char *secret, const char *client_id) {
    if (!secret || !client_id) return NULL;
    uint32_t h1 = zn_hash32((const uint8_t *)secret, (uint32_t)strlen(secret));
    uint32_t h2 = zn_hash32((const uint8_t *)client_id, (uint32_t)strlen(client_id));
    char *tok = (char *)malloc(32);
    if (tok) snprintf(tok, 32, "%08x%08x", h1, h2);
    return tok;
}
bool zn_auth_token_verify(const char *secret, const char *token, const char *client_id) {
    if (!secret || !token || !client_id) return false;
    char *expected = zn_auth_token_create(secret, client_id);
    if (!expected) return false;
    bool ok = strcmp(expected, token) == 0;
    free(expected);
    return ok;
}

struct zn_ratelimit {
    uint32_t max_pps;
    uint32_t max_bps;
    uint32_t window_start;
    uint32_t packets;
    uint64_t bytes;
};

zn_RateLimit *zn_ratelimit_create(uint32_t max_packets_per_sec, uint32_t max_bytes_per_sec) {
    zn_RateLimit *rl = (zn_RateLimit *)calloc(1, sizeof(zn_RateLimit));
    if (!rl) return NULL;
    rl->max_pps = max_packets_per_sec;
    rl->max_bps = max_bytes_per_sec;
    rl->window_start = zn_timestamp();
    return rl;
}
void zn_ratelimit_free(zn_RateLimit *rl) { free(rl); }
void zn_ratelimit_tick(zn_RateLimit *rl, uint32_t now_ms) {
    if (!rl) return;
    if (now_ms != rl->window_start) {
        rl->window_start = now_ms;
        rl->packets = 0;
        rl->bytes = 0;
    }
}
bool zn_ratelimit_allow(zn_RateLimit *rl, uint32_t bytes) {
    if (!rl) return true;
    uint32_t now = zn_timestamp();
    zn_ratelimit_tick(rl, now);
    if (rl->max_pps > 0 && rl->packets >= rl->max_pps) return false;
    if (rl->max_bps > 0 && rl->bytes + bytes > rl->max_bps) return false;
    rl->packets++;
    rl->bytes += bytes;
    return true;
}

struct zn_antispam {
    uint32_t max_per_sec;
    uint32_t window_start;
    uint32_t count;
};
zn_AntiSpam *zn_antispam_create(uint32_t max_per_sec) {
    zn_AntiSpam *as = (zn_AntiSpam *)calloc(1, sizeof(zn_AntiSpam));
    if (!as) return NULL;
    as->max_per_sec = max_per_sec;
    as->window_start = zn_timestamp();
    return as;
}
void zn_antispam_free(zn_AntiSpam *as) { free(as); }
bool zn_antispam_check(zn_AntiSpam *as, uint32_t now_ms) {
    if (!as) return true;
    if (now_ms != as->window_start) { as->window_start = now_ms; as->count = 0; }
    as->count++;
    return as->count <= as->max_per_sec;
}

uint32_t zn_cheat_checksum(const uint8_t *data, uint32_t len, uint32_t salt) {
    uint32_t h = salt ^ 0x9E3779B9u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
        h = (h << 13) | (h >> 19);
    }
    return h;
}
bool zn_cheat_validate(uint32_t checksum, const uint8_t *data, uint32_t len, uint32_t salt) {
    return zn_cheat_checksum(data, len, salt) == checksum;
}

// ===========================================================================
// data synchronization module
// ===========================================================================

struct zn_sync_world {
    SyncObject *objs[ZN_MAX_OBJECTS];
    int count;
};

zn_SyncWorld *zn_sync_create(void) {
    zn_SyncWorld *w = (zn_SyncWorld *)calloc(1, sizeof(zn_SyncWorld));
    return w;
}
void zn_sync_free(zn_SyncWorld *w) {
    if (!w) return;
    for (int i = 0; i < w->count; i++) {
        if (w->objs[i]->state) free(w->objs[i]->state);
        free(w->objs[i]);
    }
    free(w);
}
SyncObject *zn_sync_register(zn_SyncWorld *w, const char *id, const char *type, const char *state_json) {
    if (!w || !id || w->count >= ZN_MAX_OBJECTS) return NULL;
    SyncObject *o = (SyncObject *)calloc(1, sizeof(SyncObject));
    if (!o) return NULL;
    strncpy(o->id, id, sizeof(o->id) - 1);
    strncpy(o->type, type ? type : "", sizeof(o->type) - 1);
    o->state = strdup(state_json ? state_json : "{}");
    o->state_len = o->state ? (uint32_t)strlen(o->state) + 1 : 0;
    o->last_update = zn_timestamp();
    w->objs[w->count++] = o;
    return o;
}
SyncObject *zn_sync_get(zn_SyncWorld *w, const char *id) {
    if (!w || !id) return NULL;
    for (int i = 0; i < w->count; i++) if (strcmp(w->objs[i]->id, id) == 0) return w->objs[i];
    return NULL;
}
bool zn_sync_remove(zn_SyncWorld *w, const char *id) {
    if (!w || !id) return false;
    for (int i = 0; i < w->count; i++) {
        if (strcmp(w->objs[i]->id, id) == 0) {
            if (w->objs[i]->state) free(w->objs[i]->state);
            free(w->objs[i]);
            w->objs[i] = w->objs[w->count - 1];
            w->count--;
            return true;
        }
    }
    return false;
}
int zn_sync_count(zn_SyncWorld *w) { return w ? w->count : 0; }

// delta: produce a json object of only the keys whose values differ
char *zn_sync_delta(const char *old_json, const char *new_json) {
    if (!old_json || !new_json) return strdup("{}");
    // simple approach: copy new_json keys that differ, build a small delta
    char *out = (char *)malloc(1024);
    if (!out) return NULL;
    int off = 0;
    out[off++] = '{';
    const char *p = new_json;
    while (*p) {
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++;
        const char *kstart = p;
        while (*p && *p != '"') p++;
        if (!*p) break;
        size_t klen = (size_t)(p - kstart);
        p++; // skip closing quote
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++; // skip colon
        while (*p == ' ' || *p == '\t') p++;
        const char *vstart = p;
        // value ends at comma (outside string) or }
        bool in_str = false;
        while (*p) {
            if (*p == '"' && (p == vstart || p[-1] != '\\')) in_str = !in_str;
            if (!in_str && (*p == ',' || *p == '}')) break;
            p++;
        }
        size_t vlen = (size_t)(p - vstart);
        // compare with old
        char search[128];
        size_t sl = klen < 120 ? klen + 4 : 124;
        snprintf(search, sl, "\"%.*s\":", (int)klen, kstart);
        char *oldv = strstr((char *)old_json, search);
        bool changed = true;
        if (oldv) {
            oldv += strlen(search);
            const char *ovstart = oldv;
            bool oin = false;
            while (*oldv) {
                if (*oldv == '"' && (oldv == ovstart || oldv[-1] != '\\')) oin = !oin;
                if (!oin && (*oldv == ',' || *oldv == '}')) break;
                oldv++;
            }
            size_t ovlen = (size_t)(oldv - ovstart);
            if (ovlen == vlen && strncmp(ovstart, vstart, vlen) == 0) changed = false;
        }
        if (changed) {
            if (off > 1) out[off++] = ',';
            snprintf(out + off, 1024 - off, "\"%.*s\":%.*s", (int)klen, kstart, (int)vlen, vstart);
            off += (int)strlen(out + off);
        }
        if (*p == ',') p++;
    }
    out[off++] = '}';
    out[off] = '\0';
    return out;
}
bool zn_sync_apply_delta(char *state_json, const char *delta_json) {
    if (!state_json || !delta_json) return false;
    // naive merge: append delta keys (overwrites). good enough for demo
    size_t sl = strlen(state_json);
    size_t dl = strlen(delta_json);
    if (sl + dl + 8 >= 4096) return false;
    char *tmp = (char *)malloc(sl + dl + 8);
    if (!tmp) return false;
    // strip trailing } from state and leading { from delta
    if (sl > 0 && state_json[sl - 1] == '}') sl--;
    if (dl > 0 && delta_json[0] == '{') { delta_json++; dl--; }
    if (dl > 0 && delta_json[dl - 1] == '}') dl--;
    memcpy(tmp, state_json, sl);
    if (sl > 1 && dl > 0) tmp[sl++] = ',';
    memcpy(tmp + sl, delta_json, dl);
    tmp[sl + dl] = '}';
    tmp[sl + dl + 1] = '\0';
    strcpy(state_json, tmp);
    free(tmp);
    return true;
}

void zn_sync_start_interp(SyncObject *o, uint32_t now_ms, uint32_t duration_ms, const float *from, const float *to, int n) {
    if (!o) return;
    int m = n > 16 ? 16 : n;
    for (int i = 0; i < m; i++) { o->interp_from[i] = from[i]; o->interp_to[i] = to[i]; }
    o->interp_start = now_ms;
    o->interp_end = now_ms + duration_ms;
}
void zn_sync_sample_interp(SyncObject *o, uint32_t now_ms, float *out, int n) {
    if (!o) return;
    int m = n > 16 ? 16 : n;
    float t = 1.0f;
    if (o->interp_end > o->interp_start) {
        t = (float)(now_ms - o->interp_start) / (float)(o->interp_end - o->interp_start);
        if (t < 0) t = 0; if (t > 1) t = 1;
    }
    for (int i = 0; i < m; i++) out[i] = o->interp_from[i] + (o->interp_to[i] - o->interp_from[i]) * t;
}
void zn_sync_predict(const float *current, const float *velocity, float dt, float *out, int n) {
    if (!current || !velocity || !out) return;
    int m = n > 16 ? 16 : n;
    for (int i = 0; i < m; i++) out[i] = current[i] + velocity[i] * dt;
}

struct zn_rollback {
    uint8_t *frames[256];
    uint32_t lengths[256];
    int max_frames;
    int count;
};
zn_Rollback *zn_rollback_create(int max_frames) {
    zn_Rollback *rb = (zn_Rollback *)calloc(1, sizeof(zn_Rollback));
    if (!rb) return NULL;
    rb->max_frames = max_frames > 256 ? 256 : (max_frames < 1 ? 1 : max_frames);
    return rb;
}
void zn_rollback_free(zn_Rollback *rb) {
    if (!rb) return;
    for (int i = 0; i < rb->count; i++) free(rb->frames[i]);
    free(rb);
}
void zn_rollback_save(zn_Rollback *rb, uint32_t frame, const uint8_t *state, uint32_t len) {
    if (!rb || !state) return;
    int idx = frame % rb->max_frames;
    if (rb->frames[idx]) free(rb->frames[idx]);
    rb->frames[idx] = (uint8_t *)malloc(len);
    if (rb->frames[idx]) { memcpy(rb->frames[idx], state, len); rb->lengths[idx] = len; }
    if (rb->count < rb->max_frames) rb->count++;
}
bool zn_rollback_restore(zn_Rollback *rb, uint32_t frame, uint8_t *out, uint32_t len) {
    if (!rb || !out) return false;
    int idx = frame % rb->max_frames;
    if (!rb->frames[idx]) return false;
    uint32_t cpy = rb->lengths[idx] < len ? rb->lengths[idx] : len;
    memcpy(out, rb->frames[idx], cpy);
    return true;
}

// ===========================================================================
// multiplayer systems, sessions and matchmaking
// ===========================================================================

struct zn_session {
    char player_id[ZN_ID_LEN];
    int score;
    bool ready;
    char keys[32][32];
    char values[32][64];
    int kv_count;
};
zn_Session *zn_session_create(const char *player_id) {
    zn_Session *s = (zn_Session *)calloc(1, sizeof(zn_Session));
    if (!s) return NULL;
    if (player_id) strncpy(s->player_id, player_id, sizeof(s->player_id) - 1);
    return s;
}
void zn_session_free(zn_Session *s) { free(s); }
void zn_session_set_score(zn_Session *s, int score) { if (s) s->score = score; }
int zn_session_get_score(zn_Session *s) { return s ? s->score : 0; }
void zn_session_set_ready(zn_Session *s, bool ready) { if (s) s->ready = ready; }
bool zn_session_is_ready(zn_Session *s) { return s ? s->ready : false; }
void zn_session_set_data(zn_Session *s, const char *key, const char *value) {
    if (!s || !key) return;
    for (int i = 0; i < s->kv_count; i++) {
        if (strcmp(s->keys[i], key) == 0) { strncpy(s->values[i], value ? value : "", sizeof(s->values[i]) - 1); return; }
    }
    if (s->kv_count < 32) {
        strncpy(s->keys[s->kv_count], key, sizeof(s->keys[s->kv_count]) - 1);
        strncpy(s->values[s->kv_count], value ? value : "", sizeof(s->values[s->kv_count]) - 1);
        s->kv_count++;
    }
}
const char *zn_session_get_data(zn_Session *s, const char *key) {
    if (!s || !key) return "";
    for (int i = 0; i < s->kv_count; i++) if (strcmp(s->keys[i], key) == 0) return s->values[i];
    return "";
}

struct zn_matchmaker {
    char players[256][ZN_ID_LEN];
    int ranks[256];
    int count;
};
zn_Matchmaker *zn_matchmaker_create(void) { return (zn_Matchmaker *)calloc(1, sizeof(zn_Matchmaker)); }
void zn_matchmaker_free(zn_Matchmaker *m) { free(m); }
void zn_matchmaker_add_player(zn_Matchmaker *m, const char *player_id, int rank) {
    if (!m || !player_id || m->count >= 256) return;
    strncpy(m->players[m->count], player_id, ZN_ID_LEN - 1);
    m->ranks[m->count] = rank;
    m->count++;
}
const char *zn_matchmaker_find_room(zn_Matchmaker *m, int capacity, int rank) {
    if (!m) return "";
    // bucket by rank/10 to group similar skill, capacity influences room tag
    static char room[64];
    snprintf(room, sizeof(room), "match_%d_cap%d", rank / 10, capacity);
    return room;
}
void zn_matchmaker_remove_player(zn_Matchmaker *m, const char *player_id) {
    if (!m || !player_id) return;
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->players[i], player_id) == 0) {
            strcpy(m->players[i], m->players[m->count - 1]);
            m->ranks[i] = m->ranks[m->count - 1];
            m->count--;
            return;
        }
    }
}

// ===========================================================================
// developer tools, inspector, profiler, monitor
// ===========================================================================

struct zn_inspector {
    char log[256][256];
    int count;
    int head;
};
zn_Inspector *zn_inspector_create(void) { return (zn_Inspector *)calloc(1, sizeof(zn_Inspector)); }
void zn_inspector_free(zn_Inspector *insp) { free(insp); }
void zn_inspector_log(zn_Inspector *insp, const Packet *pkt, bool outgoing) {
    if (!insp || !pkt) return;
    snprintf(insp->log[insp->head], sizeof(insp->log[insp->head]),
        "%s type=%d cid=%u len=%u", outgoing ? "OUT" : "IN", (int)pkt->type, pkt->client_id, pkt->data_len);
    insp->head = (insp->head + 1) % 256;
    if (insp->count < 256) insp->count++;
}
uint32_t zn_inspector_count(zn_Inspector *insp) { return insp ? insp->count : 0; }
const char *zn_inspector_report(zn_Inspector *insp) {
    static char buf[2048];
    if (!insp) return "";
    buf[0] = '\0';
    int start = (insp->head - insp->count + 256) % 256;
    for (int i = 0; i < insp->count; i++) {
        strncat(buf, insp->log[(start + i) % 256], 2047 - strlen(buf));
        strncat(buf, "\n", 2047 - strlen(buf));
    }
    return buf;
}

struct zn_profiler {
    uint32_t samples[1024];
    int count;
    int head;
};
zn_Profiler *zn_profiler_create(void) { return (zn_Profiler *)calloc(1, sizeof(zn_Profiler)); }
void zn_profiler_free(zn_Profiler *p) { free(p); }
void zn_profiler_sample(zn_Profiler *p, uint32_t ms) {
    if (!p) return;
    p->samples[p->head] = ms;
    p->head = (p->head + 1) % 1024;
    if (p->count < 1024) p->count++;
}
uint32_t zn_profiler_avg(zn_Profiler *p) {
    if (!p || p->count == 0) return 0;
    uint64_t s = 0; for (int i = 0; i < p->count; i++) s += p->samples[i];
    return (uint32_t)(s / p->count);
}
uint32_t zn_profiler_max(zn_Profiler *p) {
    if (!p || p->count == 0) return 0;
    uint32_t m = 0; for (int i = 0; i < p->count; i++) if (p->samples[i] > m) m = p->samples[i];
    return m;
}

struct zn_monitor {
    char ids[128][ZN_ID_LEN];
    uint32_t rtt[128];
    uint32_t pps[128];
    int count;
};
zn_Monitor *zn_monitor_create(void) { return (zn_Monitor *)calloc(1, sizeof(zn_Monitor)); }
void zn_monitor_free(zn_Monitor *mon) { free(mon); }
void zn_monitor_update(zn_Monitor *mon, const char *id, uint32_t rtt, uint32_t pps) {
    if (!mon || !id) return;
    for (int i = 0; i < mon->count; i++) {
        if (strcmp(mon->ids[i], id) == 0) { mon->rtt[i] = rtt; mon->pps[i] = pps; return; }
    }
    if (mon->count < 128) {
        strncpy(mon->ids[mon->count], id, ZN_ID_LEN - 1);
        mon->rtt[mon->count] = rtt; mon->pps[mon->count] = pps; mon->count++;
    }
}
const char *zn_monitor_snapshot(zn_Monitor *mon) {
    static char buf[2048];
    if (!mon) return "";
    buf[0] = '\0';
    for (int i = 0; i < mon->count; i++) {
        char line[128];
        snprintf(line, sizeof(line), "%s rtt=%ums pps=%u\n", mon->ids[i], mon->rtt[i], mon->pps[i]);
        strncat(buf, line, 2047 - strlen(buf));
    }
    return buf;
}

// ===========================================================================
// advanced features, file transfer, replay, net sim, server browser, voice
// ===========================================================================

struct zn_filetx {
    uint32_t received;
    uint32_t total;
};
zn_FileTx *zn_filetx_create(void) { return (zn_FileTx *)calloc(1, sizeof(zn_FileTx)); }
void zn_filetx_free(zn_FileTx *ft) { free(ft); }
uint32_t zn_filetx_total_chunks(zn_FileTx *ft, uint32_t file_size, uint32_t chunk_size) {
    (void)ft;
    if (chunk_size == 0) return 0;
    return (file_size + chunk_size - 1) / chunk_size;
}
char *zn_filetx_make_chunk(zn_FileTx *ft, const uint8_t *data, uint32_t file_size, uint32_t chunk_size, uint32_t index) {
    if (!data) return NULL;
    uint32_t start = index * chunk_size;
    if (start >= file_size) return NULL;
    uint32_t this_len = file_size - start;
    if (this_len > chunk_size) this_len = chunk_size;
    uint32_t hexlen = this_len * 2 + 1;
    char *hex = (char *)malloc(hexlen);
    if (!hex) return NULL;
    hex_encode(data + start, this_len, hex);
    char *out = (char *)malloc(hexlen + 64);
    if (!out) { free(hex); return NULL; }
    snprintf(out, hexlen + 64, "{\"index\":%u,\"total\":%u,\"size\":%u,\"data\":\"%s\"}",
        index, zn_filetx_total_chunks(ft, file_size, chunk_size), this_len, hex);
    free(hex);
    return out;
}
bool zn_filetx_collect_chunk(zn_FileTx *ft, const char *chunk_json, uint8_t *out, uint32_t *out_len) {
    if (!ft || !chunk_json || !out) return false;
    char *idx_s = json_strval(chunk_json, "index");
    char *data_s = json_strval(chunk_json, "data");
    if (!idx_s || !data_s) { free(idx_s); free(data_s); return false; }
    uint32_t idx = (uint32_t)atoi(idx_s);
    uint32_t n = hex_decode(data_s, out + idx * 1024, 65536);
    if (out_len) *out_len = idx * 1024 + n;
    ft->received++;
    free(idx_s); free(data_s);
    return true;
}

struct zn_replay {
    uint8_t *frames[4096];
    uint32_t lengths[4096];
    int count;
};
zn_Replay *zn_replay_create(void) { return (zn_Replay *)calloc(1, sizeof(zn_Replay)); }
void zn_replay_free(zn_Replay *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) free(r->frames[i]);
    free(r);
}
void zn_replay_record(zn_Replay *r, const Packet *pkt) {
    if (!r || !pkt || r->count >= 4096) return;
    uint32_t len;
    uint8_t *raw = zn_znp_serialize((Packet *)pkt, &len);
    if (!raw) return;
    r->frames[r->count] = raw;
    r->lengths[r->count] = len;
    r->count++;
}
uint32_t zn_replay_count(zn_Replay *r) { return r ? (uint32_t)r->count : 0; }
Packet *zn_replay_get(zn_Replay *r, uint32_t index) {
    if (!r || index >= (uint32_t)r->count) return NULL;
    return zn_znp_deserialize(r->frames[index], r->lengths[index]);
}
bool zn_replay_save(zn_Replay *r, const char *path) {
    if (!r || !path) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    for (int i = 0; i < r->count; i++) {
        char hex[8];
        snprintf(hex, sizeof(hex), "%08x", r->lengths[i]);
        fwrite(hex, 1, 8, f);
        uint8_t *hexbuf = (uint8_t *)malloc(r->lengths[i] * 2 + 1);
        if (hexbuf) {
            hex_encode(r->frames[i], r->lengths[i], (char *)hexbuf);
            fwrite(hexbuf, 1, r->lengths[i] * 2, f);
            free(hexbuf);
        }
        fputc('\n', f);
    }
    fclose(f);
    return true;
}
bool zn_replay_load(zn_Replay *r, const char *path) {
    if (!r || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[ZN_MAX_PACKET * 2 + 16];
    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 9) continue;
        uint32_t len = (uint32_t)strtoul(line, NULL, 16);
        char *hex = line + 8;
        char *nl = strchr(hex, '\n'); if (nl) *nl = '\0';
        uint8_t *raw = (uint8_t *)malloc(len);
        if (!raw) break;
        uint32_t n = hex_decode(hex, raw, len);
        if (r->count < 4096) { r->frames[r->count] = raw; r->lengths[r->count] = n; r->count++; }
        else { free(raw); break; }
    }
    fclose(f);
    return true;
}

struct zn_netsim {
    uint32_t latency;
    uint32_t jitter;
    float loss;
};
zn_NetSim *zn_netsim_create(void) { return (zn_NetSim *)calloc(1, sizeof(zn_NetSim)); }
void zn_netsim_free(zn_NetSim *ns) { free(ns); }
void zn_netsim_set_latency(zn_NetSim *ns, uint32_t latency_ms) { if (ns) ns->latency = latency_ms; }
void zn_netsim_set_jitter(zn_NetSim *ns, uint32_t jitter_ms) { if (ns) ns->jitter = jitter_ms; }
void zn_netsim_set_loss(zn_NetSim *ns, float loss_pct) { if (ns) ns->loss = loss_pct; }
uint32_t zn_netsim_delay(zn_NetSim *ns) {
    if (!ns) return 0;
    uint32_t d = ns->latency;
    if (ns->jitter > 0) d += (uint32_t)(rand() % (ns->jitter + 1));
    return d;
}
bool zn_netsim_drop(zn_NetSim *ns) {
    if (!ns || ns->loss <= 0) return false;
    return ((float)rand() / (float)RAND_MAX) * 100.0f < ns->loss;
}

struct zn_server_browser {
    char regions[64][32];
    char hosts[64][ZN_HOST_LEN];
    uint16_t ports[64];
    int players[64];
    int max_players[64];
    int count;
};
zn_ServerBrowser *zn_server_browser_create(void) { return (zn_ServerBrowser *)calloc(1, sizeof(zn_ServerBrowser)); }
void zn_server_browser_free(zn_ServerBrowser *sb) { free(sb); }
void zn_server_browser_add(zn_ServerBrowser *sb, const char *region, const char *host, uint16_t port, int players, int max_players) {
    if (!sb || !region || !host || sb->count >= 64) return;
    strncpy(sb->regions[sb->count], region, sizeof(sb->regions[sb->count]) - 1);
    strncpy(sb->hosts[sb->count], host, sizeof(sb->hosts[sb->count]) - 1);
    sb->ports[sb->count] = port;
    sb->players[sb->count] = players;
    sb->max_players[sb->count] = max_players;
    sb->count++;
}
void zn_server_browser_remove(zn_ServerBrowser *sb, const char *host, uint16_t port) {
    if (!sb || !host) return;
    for (int i = 0; i < sb->count; i++) {
        if (strcmp(sb->hosts[i], host) == 0 && sb->ports[i] == port) {
            strcpy(sb->hosts[i], sb->hosts[sb->count - 1]);
            sb->ports[i] = sb->ports[sb->count - 1];
            strcpy(sb->regions[i], sb->regions[sb->count - 1]);
            sb->players[i] = sb->players[sb->count - 1];
            sb->max_players[i] = sb->max_players[sb->count - 1];
            sb->count--;
            return;
        }
    }
}
const char *zn_server_browser_list(zn_ServerBrowser *sb, const char *region) {
    static char buf[2048];
    if (!sb) return "";
    buf[0] = '\0';
    for (int i = 0; i < sb->count; i++) {
        if (region && strcmp(sb->regions[i], region) != 0) continue;
        char line[128];
        snprintf(line, sizeof(line), "[%s] %s:%u (%d/%d)\n", sb->regions[i], sb->hosts[i], sb->ports[i], sb->players[i], sb->max_players[i]);
        strncat(buf, line, 2047 - strlen(buf));
    }
    return buf;
}

struct zn_voice {
    uint32_t sample_rate;
    uint32_t frame_samples;
};
zn_Voice *zn_voice_create(uint32_t sample_rate) {
    zn_Voice *v = (zn_Voice *)calloc(1, sizeof(zn_Voice));
    if (!v) return NULL;
    v->sample_rate = sample_rate;
    v->frame_samples = sample_rate / 100; // 10ms frames
    return v;
}
void zn_voice_free(zn_Voice *v) { free(v); }
uint32_t zn_voice_frame_size(zn_Voice *v) { return v ? v->frame_samples : 0; }
char *zn_voice_encode(zn_Voice *v, const int16_t *pcm, uint32_t samples) {
    if (!v || !pcm) return NULL;
    uint32_t n = samples < v->frame_samples ? samples : v->frame_samples;
    uint32_t hexlen = n * 2 * sizeof(int16_t) + 1;
    char *hex = (char *)malloc(hexlen);
    if (!hex) return NULL;
    hex_encode((const uint8_t *)pcm, n * sizeof(int16_t), hex);
    char *out = (char *)malloc(hexlen + 64);
    if (!out) { free(hex); return NULL; }
    snprintf(out, hexlen + 64, "{\"samples\":%u,\"data\":\"%s\"}", n, hex);
    free(hex);
    return out;
}
bool zn_voice_decode(zn_Voice *v, const char *frame_json, int16_t *pcm, uint32_t *out_samples) {
    if (!v || !frame_json || !pcm) return false;
    char *data_s = json_strval(frame_json, "data");
    char *samp_s = json_strval(frame_json, "samples");
    if (!data_s) { free(samp_s); return false; }
    uint32_t n = hex_decode(data_s, (uint8_t *)pcm, v->frame_samples * sizeof(int16_t));
    if (out_samples) *out_samples = n / sizeof(int16_t);
    free(data_s); free(samp_s);
    return true;
}

// ===========================================================================
// platform support
// ===========================================================================

PlatformId zn_platform(void) {
#ifdef _WIN32
    return ZN_PLATFORM_WINDOWS;
#elif defined(__APPLE__)
    return ZN_PLATFORM_MACOS;
#elif defined(__ANDROID__)
    return ZN_PLATFORM_ANDROID;
#elif defined(__EMSCRIPTEN__)
    return ZN_PLATFORM_WASM;
#elif defined(__linux__)
    return ZN_PLATFORM_LINUX;
#else
    return ZN_PLATFORM_UNKNOWN;
#endif
}
const char *zn_platform_name(void) {
    switch (zn_platform()) {
        case ZN_PLATFORM_WINDOWS: return "Windows";
        case ZN_PLATFORM_LINUX: return "Linux";
        case ZN_PLATFORM_MACOS: return "macOS";
        case ZN_PLATFORM_IOS: return "iOS";
        case ZN_PLATFORM_ANDROID: return "Android";
        case ZN_PLATFORM_WASM: return "WebAssembly";
        default: return "Unknown";
    }
}