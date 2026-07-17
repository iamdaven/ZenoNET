#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET closesocket
#define SOCKET_ERRNO WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#define CLOSE_SOCKET close
#define SOCKET_ERRNO errno
#endif
#include "../include/zenonet.h"

struct zn_server {
    char host[64];
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
    void *cb_userdata;
    uint32_t start_time;
    uint64_t packets_recv;
    uint64_t packets_sent;
};

static int srv_winsock_refs = 0;
#ifdef _WIN32
static void srv_ensure_winsock(void) {
    if (srv_winsock_refs++ == 0) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
}
#else
static void srv_ensure_winsock(void) {}
#endif

Server *zn_server_create(uint16_t port) { return zn_server_create_host("0.0.0.0", port); }

Server *zn_server_create_host(const char *host, uint16_t port) {
    srv_ensure_winsock();
    Server *srv = (Server *)calloc(1, sizeof(Server));
    if (!srv) return NULL;
    if (host) strncpy(srv->host, host, sizeof(srv->host) - 1);
    srv->port = port; srv->sockfd = -1; srv->running = false;
    srv->conn_count = 0; srv->room_count = 0;
    srv->packets_recv = 0; srv->packets_sent = 0;
    srv->on_connect_cb = NULL; srv->on_disconnect_cb = NULL; srv->on_data_cb = NULL;
    return srv;
}

void zn_server_destroy(Server *srv) {
    if (!srv) return;
    zn_server_stop(srv);
    for (int i = 0; i < srv->conn_count; i++) zn_conn_free(srv->connections[i]);
    for (int i = 0; i < srv->room_count; i++) zn_room_destroy(srv->room_list[i]);
    free(srv);
}

bool zn_server_start(Server *srv) {
    if (!srv || srv->running) return false;
    srv->sockfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (srv->sockfd < 0) { printf("[!] socket failed\n"); return false; }
    int opt = 1;
    setsockopt(srv->sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(srv->port);
    addr.sin_addr.s_addr = inet_addr(srv->host);
    if (bind(srv->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[!] bind failed\n"); CLOSE_SOCKET(srv->sockfd); return false;
    }
    listen(srv->sockfd, 128);
    srv->running = true;
    srv->start_time = (uint32_t)time(NULL);
    printf("[*] server on %s:%d\n", srv->host, srv->port);
    fd_set readfds;
    struct timeval tv;
    while (srv->running) {
        FD_ZERO(&readfds); FD_SET(srv->sockfd, &readfds);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        if (select(srv->sockfd + 1, &readfds, NULL, NULL, &tv) < 0) break;
        if (FD_ISSET(srv->sockfd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = (int)accept(srv->sockfd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd >= 0 && srv->conn_count < ZN_MAX_CONNECTIONS) {
                char addr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
                Connection *conn = zn_conn_new(client_fd, addr_str, ntohs(client_addr.sin_port));
                srv->connections[srv->conn_count++] = conn;
                printf("[+] %s\n", zn_conn_get_id(conn));
                if (srv->on_connect_cb) srv->on_connect_cb(conn, srv->cb_userdata);
            }
        }
    }
    return true;
}

void zn_server_stop(Server *srv) {
    if (!srv) return;
    srv->running = false;
    for (int i = 0; i < srv->conn_count; i++) zn_conn_close(srv->connections[i]);
    if (srv->sockfd >= 0) { CLOSE_SOCKET(srv->sockfd); srv->sockfd = -1; }
    printf("[*] server stopped\n");
}

void zn_server_broadcast(Server *srv, Packet *pkt, Connection *exclude) {
    if (!srv || !pkt) return;
    for (int i = 0; i < srv->conn_count; i++) {
        if (srv->connections[i] != exclude) zn_conn_send(srv->connections[i], pkt);
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

int zn_server_conn_count(Server *srv) { return srv ? srv->conn_count : 0; }

void zn_server_on_connect(Server *srv, conn_callback cb, void *userdata) {
    if (srv) { srv->on_connect_cb = cb; srv->cb_userdata = userdata; }
}
void zn_server_on_disconnect(Server *srv, conn_callback cb, void *userdata) { if (srv) srv->on_disconnect_cb = cb; }
void zn_server_on_data(Server *srv, data_callback cb, void *userdata) { if (srv) srv->on_data_cb = cb; }