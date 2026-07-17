#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif
#include "../include/zenonet.h"

struct zn_client {
    int sockfd;
    bool connected;
    char host[64];
    uint16_t port;
    char name[64];
    conn_callback on_connect_cb;
    conn_callback on_disconnect_cb;
    data_callback on_data_cb;
    packet_callback on_message_cb;
    void *cb_userdata;
    uint8_t *buffer;
    uint32_t buf_len;
    uint32_t buf_cap;
};

static int cli_winsock_refs = 0;
#ifdef _WIN32
static void cli_ensure_winsock(void) {
    if (cli_winsock_refs++ == 0) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
}
#else
static void cli_ensure_winsock(void) {}
#endif

Client *zn_client_new(void) {
    cli_ensure_winsock();
    Client *cli = (Client *)calloc(1, sizeof(Client));
    if (!cli) return NULL;
    cli->sockfd = -1; cli->connected = false;
    cli->buf_cap = ZN_BUF_SIZE;
    cli->buffer = (uint8_t *)malloc(ZN_BUF_SIZE);
    cli->buf_len = 0;
    strcpy(cli->name, "Player");
    return cli;
}

void zn_client_free(Client *cli) {
    if (cli) { zn_client_disconnect(cli); if (cli->buffer) free(cli->buffer); free(cli); }
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
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(cli->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[!] connect failed\n"); CLOSE_SOCKET(cli->sockfd); cli->sockfd = -1; return false;
    }
    cli->connected = true;
    printf("[*] connected to %s:%d\n", host, port);
    if (cli->on_connect_cb) cli->on_connect_cb(NULL, cli->cb_userdata);
    return true;
}

void zn_client_disconnect(Client *cli) {
    if (!cli) return;
    cli->connected = false;
    if (cli->sockfd >= 0) { CLOSE_SOCKET(cli->sockfd); cli->sockfd = -1; }
    if (cli->on_disconnect_cb) cli->on_disconnect_cb(NULL, cli->cb_userdata);
    printf("[*] client disconnected\n");
}

bool zn_client_send(Client *cli, Packet *pkt) {
    if (!cli || !cli->connected || !pkt) return false;
    uint32_t len; uint8_t *data = zn_packet_serialize(pkt, &len);
    if (!data) return false;
    int sent = 0;
    while (sent < (int)len) {
        int n = send(cli->sockfd, (const char *)(data + sent), len - sent, 0);
        if (n <= 0) { free(data); return false; }
        sent += n;
    }
    free(data); return true;
}

bool zn_client_send_data(Client *cli, const char *json_data) {
    Packet *pkt = zn_packet_create(PKT_DATA, json_data);
    if (!pkt) return false;
    bool result = zn_client_send(cli, pkt);
    zn_packet_free(pkt); return result;
}

bool zn_client_join_room(Client *cli, const char *room_name) {
    char buf[128]; snprintf(buf, sizeof(buf), "{\"room\":\"%s\"}", room_name);
    Packet *pkt = zn_packet_create(PKT_JOIN_ROOM, buf);
    if (!pkt) return false;
    bool result = zn_client_send(cli, pkt);
    zn_packet_free(pkt); return result;
}

bool zn_client_leave_room(Client *cli) {
    Packet *pkt = zn_packet_create(PKT_LEAVE_ROOM, "{}");
    if (!pkt) return false;
    bool result = zn_client_send(cli, pkt);
    zn_packet_free(pkt); return result;
}

bool zn_client_is_connected(Client *cli) { return cli && cli->connected; }
void zn_client_on_connect(Client *cli, conn_callback cb, void *userdata) { if (cli) { cli->on_connect_cb = cb; cli->cb_userdata = userdata; } }
void zn_client_on_disconnect(Client *cli, conn_callback cb, void *userdata) { if (cli) cli->on_disconnect_cb = cb; }
void zn_client_on_data(Client *cli, data_callback cb, void *userdata) { if (cli) cli->on_data_cb = cb; }
void zn_client_on_message(Client *cli, packet_callback cb, void *userdata) { if (cli) cli->on_message_cb = cb; }
uint32_t zn_timestamp(void) { return (uint32_t)time(NULL); }
void zn_log(const char *fmt, ...) { va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); }